#include "DecodeScheduler.h"
#include "ImageDecodeDispatcher.h"
#include "FileByteCache.h"
#include "ImageFormatDetector.h"
#include "IPathByteSource.h"
#include "ImageCoreLog.h"
#include <algorithm>
#include <filesystem>
#include <objbase.h>
#include <windows.h>
#include <winioctl.h>
#include <unordered_map>

namespace ImageCore
{
    namespace
    {
#ifndef STORAGE_SEEK_PENALTY_DESCRIPTOR
        // Some Windows SDK header sets don't declare this type even though the IOCTL/property is supported.
        typedef struct _STORAGE_SEEK_PENALTY_DESCRIPTOR
        {
            DWORD Version;
            DWORD Size;
            BOOLEAN IncursSeekPenalty;
        } STORAGE_SEEK_PENALTY_DESCRIPTOR, *PSTORAGE_SEEK_PENALTY_DESCRIPTOR;
#endif

        static std::wstring GetVolumePathForFilePath(const std::wstring& path)
        {
            // Resolve archive display paths to the host archive file for volume queries.
            std::wstring actualPath = path;
            if (IPathByteSource* source = GetPathByteSource())
            {
                actualPath = source->ResolveHostPath(path);
            }

            // Prefer "C:" style volume handle for IOCTL queries.
            // For UNC paths, fall back to empty (unknown -> conservative).
            return FileByteCache::GetVolumeForPath(actualPath);
        }

        static bool QueryIncursSeekPenaltyForVolume(const std::wstring& volume)
        {
            if (volume.empty())
            {
                return true; // conservative: assume HDD/seek penalty
            }

            // Open volume device like "\\.\C:"
            std::wstring devicePath = L"\\\\.\\";
            devicePath += volume;

            HANDLE h = CreateFileW(
                devicePath.c_str(),
                0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);

            if (h == INVALID_HANDLE_VALUE)
            {
                return true;
            }

            STORAGE_PROPERTY_QUERY query {};
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            query.QueryType = PropertyStandardQuery;

            STORAGE_SEEK_PENALTY_DESCRIPTOR desc {};
            DWORD bytesReturned = 0;
            const BOOL ok = DeviceIoControl(
                h,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query,
                sizeof(query),
                &desc,
                sizeof(desc),
                &bytesReturned,
                nullptr);

            CloseHandle(h);

            if (!ok || bytesReturned < sizeof(desc) || desc.Version == 0 || desc.Size == 0)
            {
                return true;
            }

            return desc.IncursSeekPenalty ? true : false;
        }

        static std::shared_ptr<std::vector<uint8_t>> ReadWholeFileBytes(const std::wstring& path, std::stop_token stop)
        {
            IPathByteSource* source = GetPathByteSource();
            if (!source)
            {
                return nullptr;
            }

            // Host adapter reads disk files and archive entries.
            std::vector<uint8_t> data = source->ReadAll(path, stop);
            if (data.empty())
            {
                return nullptr;
            }

            // Safety: don't prefetch >1GB into memory.
            if (data.size() > 1024LL * 1024LL * 1024LL)
            {
                return nullptr;
            }

            auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(data));
            return bytes;
        }

        static bool IsArchiveBackedPath(const std::wstring& path)
        {
            IPathByteSource* source = GetPathByteSource();
            if (!source || path.empty())
            {
                return false;
            }

            const std::wstring host = source->ResolveHostPath(path);
            return !host.empty() && host != path;
        }

        // (debug-only tracing removed)
    }

    struct ReadLease
    {
        // Guards both fields. OS cancellation is issued under it, so retiring a
        // read (which takes it) waits for any CancelSynchronousIo aimed at that
        // read, and a late cancel can never hit the thread's next operation.
        std::mutex mutex;
        HANDLE thread { nullptr };                    // real handle of the owning thread
        std::shared_ptr<DecodeRequestState> active;   // request whose read runs there

        ~ReadLease()
        {
            if (thread)
            {
                CloseHandle(thread);
            }
        }
    };

    struct DecodeScheduler::ThreadSlot
    {
        std::thread thread;
        std::shared_ptr<ReadLease> lease { std::make_shared<ReadLease>() };
        bool exited { false };  // guarded by m_exitMutex
    };

    namespace
    {
        // Gives the calling thread's lease a real handle of the thread, for
        // CancelSynchronousIo from other threads (GetCurrentThread() is a
        // pseudo handle).
        void BindLeaseToCurrentThread(ReadLease& lease)
        {
            HANDLE handle = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                handle = nullptr;
            }
            std::lock_guard<std::mutex> lock(lease.mutex);
            lease.thread = handle;
        }
    }

    void PriorityTaskQueue::Push(DecodeTask task, DecodeRequestState::Stage stage)
    {
        DecodeRequestState* state = task.state.get();
        const RequestPriority priority = state ? state->priority : task.priority;
        const uint64_t sequence = state ? state->sequence : 0;
        if (state)
        {
            state->queuedIn = stage;
        }
        m_tasks.emplace(KeyOf(priority, sequence), std::move(task));
    }

    RequestPriority PriorityTaskQueue::TopPriority() const
    {
        return static_cast<RequestPriority>(255 - m_tasks.begin()->first.rank);
    }

    DecodeTask PriorityTaskQueue::Pop()
    {
        auto it = m_tasks.begin();
        DecodeTask task = std::move(it->second);
        m_tasks.erase(it);
        if (task.state)
        {
            task.state->queuedIn = DecodeRequestState::Stage::None;
        }
        return task;
    }

    bool PriorityTaskQueue::Reprioritize(DecodeRequestState& state, RequestPriority priority)
    {
        auto it = m_tasks.find(KeyOf(state.priority, state.sequence));
        if (it == m_tasks.end() || it->second.state.get() != &state)
        {
            return false;
        }
        DecodeTask task = std::move(it->second);
        m_tasks.erase(it);
        state.priority = priority;
        m_tasks.emplace(KeyOf(priority, state.sequence), std::move(task));
        return true;
    }

    DecodeScheduler::DecodeScheduler()
        : m_shutdown(false)
        , m_threadCount(0)
    {
        SetThreadCount(DefaultWorkerCount(std::thread::hardware_concurrency()));
    }

    size_t DecodeScheduler::DefaultWorkerCount(unsigned hardwareThreads)
    {
        // CPU core count - 1, minimum 1. hardware_concurrency() may report 0.
        return (hardwareThreads > 1) ? static_cast<size_t>(hardwareThreads - 1) : 1;
    }

    size_t DecodeScheduler::WorkerCount() const
    {
        return m_workerCount.load();
    }

    size_t DecodeScheduler::LiveRequestCount() const
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_liveRequests.size();
    }

    size_t DecodeScheduler::VolumeProfileCount() const
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_volumeProfiles.size();
    }

    size_t DecodeScheduler::PrefetchDepthForPath(const std::wstring& path)
    {
        // Resolving the host path and querying storage properties can block (host
        // byte source, volume device open), so neither runs under a lock; the
        // per-volume result is cached because the query is relatively expensive.
        const std::wstring vol = GetVolumePathForFilePath(path);

        bool known = false;
        bool incursSeek = true;
        {
            std::lock_guard<std::mutex> lock(m_seekPenaltyMutex);
            auto it = m_seekPenaltyByVolume.find(vol);
            if (it != m_seekPenaltyByVolume.end())
            {
                known = true;
                incursSeek = it->second;
            }
        }
        if (!known)
        {
            incursSeek = QueryIncursSeekPenaltyForVolume(vol);
            std::lock_guard<std::mutex> lock(m_seekPenaltyMutex);
            m_seekPenaltyByVolume.emplace(vol, incursSeek);
        }

        // SSD/NVMe (no seek penalty) -> deeper prefetch helps hide latency.
        // HDD (seek penalty) -> keep shallow to avoid memory + avoid extra I/O pressure.
        return incursSeek ? 2 : 4;
    }

    void DecodeScheduler::SetTestHook(TestHook hook)
    {
        m_testHook = std::move(hook);
    }

    void DecodeScheduler::RunTestHook(TestPoint point, uint64_t handle) const
    {
        if (m_testHook)
        {
            m_testHook(point, handle);
        }
    }

    std::shared_ptr<std::vector<uint8_t>> DecodeScheduler::LeasedRead(
        ReadLease& lease,
        const std::shared_ptr<DecodeRequestState>& state,
        uint64_t handle,
        const std::wstring& path,
        bool& stopped)
    {
        const std::stop_token stop = state->stop.get_token();
        RunTestHook(TestPoint::BeforeReadRegister, handle);
        {
            std::lock_guard<std::mutex> lock(lease.mutex);
            lease.active = state;
        }

        // Registered before the check: a Cancel that this check misses has its
        // stop dispatched later, and the dispatch then finds the read on the
        // lease. The canceled flag is checked too: Cancel sets it at once, while
        // the token is signaled by the stop-dispatch thread.
        std::shared_ptr<std::vector<uint8_t>> bytes;
        if (!stop.stop_requested() && !state->canceled.load())
        {
            try
            {
                bytes = ReadWholeFileBytes(path, stop);
            }
            catch (...)
            {
                // Never let a host exception kill the thread or skip retirement.
                bytes.reset();
            }
        }

        RunTestHook(TestPoint::BeforeReadRetire, handle);
        {
            // Waits for an OS cancel aimed at this read that is still in progress.
            std::lock_guard<std::mutex> lock(lease.mutex);
            lease.active.reset();
        }

        stopped = stop.stop_requested() || state->canceled.load();
        if (stopped)
        {
            bytes.reset();  // empty or partial: never cached, never decoded
        }
        return bytes;
    }

    void DecodeScheduler::StopRequest(const std::shared_ptr<DecodeRequestState>& state)
    {
        // Host stop callbacks run synchronously here; no scheduler lock is held.
        state->stop.request_stop();

        std::shared_ptr<ReadLease> lease;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            lease = state->readLease;
        }
        if (!lease)
        {
            return;  // not taken by a thread yet: it is skipped when taken
        }

        // Best effort: unblocks a synchronous OS call (network path, online-only
        // file) that ignores the token. Issued only while this request's read is
        // registered, under the lease mutex, so it cannot reach another request.
        std::lock_guard<std::mutex> lock(lease->mutex);
        if (lease->active == state && lease->thread)
        {
            RunTestHook(TestPoint::CancelDispatch, state->handle);
            (void)CancelSynchronousIo(lease->thread);
        }
    }

    void DecodeScheduler::CancelActiveReadsIo()
    {
        // Only CancelSynchronousIo (no host code), so the caller's deadline
        // holds. Every read registered during shutdown belongs to a canceled
        // request; the lease mutex keeps the call on that read.
        for (const auto& slot : m_threads)
        {
            ReadLease& lease = *slot->lease;
            std::lock_guard<std::mutex> lock(lease.mutex);
            if (lease.active && lease.active->canceled.load() && lease.thread)
            {
                RunTestHook(TestPoint::CancelDispatch, lease.active->handle);
                (void)CancelSynchronousIo(lease.thread);
            }
        }
    }

    void DecodeScheduler::StopDispatchThread(ThreadSlot* slot)
    {
        // Runs every stop request (and so every host stop callback) for Cancel
        // and Shutdown, so neither blocks its caller on host code. Exits once
        // shutdown has begun and the queue is drained; Shutdown counts it among
        // the threads it joins, so a stop callback that never returns makes a
        // bounded Shutdown time out instead of hanging it.
        for (;;)
        {
            std::shared_ptr<DecodeRequestState> state;
            {
                std::unique_lock<std::mutex> lock(m_stopMutex);
                m_stopCondition.wait(lock, [this] { return !m_stopQueue.empty() || m_stopExit; });
                if (m_stopQueue.empty())
                {
                    break;
                }
                state = std::move(m_stopQueue.front());
                m_stopQueue.pop_front();
            }
            StopRequest(state);
            RunTestHook(TestPoint::StopDispatched, state->handle);
        }

        SignalExit(*slot);
    }

    void DecodeScheduler::Finish_NoLock(const DecodeTask& task)
    {
        auto it = m_liveRequests.find(task.handle);
        if (it != m_liveRequests.end() && it->second == task.state)
        {
            m_liveRequests.erase(it);
        }
    }

    bool DecodeScheduler::ReleasePrefetch_NoLock(DecodeRequestState& state)
    {
        state.prefetched.reset();
        if (!state.holdsPrefetchSlot)
        {
            return false;
        }
        state.holdsPrefetchSlot = false;
        if (m_prefetchedTotal > 0)
        {
            --m_prefetchedTotal;
        }
        auto it = m_prefetchedByVolume.find(state.prefetchVolume);
        if (it != m_prefetchedByVolume.end() && --it->second == 0)
        {
            m_prefetchedByVolume.erase(it);
        }
        return true;
    }

    size_t DecodeScheduler::PrefetchedTaskCount() const
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_prefetchedTotal;
    }

    DecodeScheduler::~DecodeScheduler()
    {
        Shutdown();
    }

    void DecodeScheduler::SetThreadCount(size_t count)
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
        if (m_joinPending)
        {
            // Restarting would need the unbounded join, which would block on the
            // thread a bounded Shutdown could not join.
            IC_LOG_WARN("[DecodeScheduler] SetThreadCount({}) ignored: threads from a timed-out Shutdown are still running.",
                count);
            return;
        }

        // Shutdown also resets volume tuning and FileByteCache (restart semantics).
        (void)StopAndJoin(std::nullopt);

        m_threadCount = std::clamp<size_t>(count, 1, kMaxDecodeWorkers);
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shutdown = false;
        }

        const auto startWorker = [this](WorkerKind kind)
        {
            auto slot = std::make_unique<ThreadSlot>();
            slot->thread = std::thread(&DecodeScheduler::WorkerThread, this, kind, slot.get());
            m_threads.push_back(std::move(slot));
        };
        if (m_threadCount == 1)
        {
            // A single worker must serve both queues: full-res first, background work only
            // when the high queue is empty. Worst-case main-image latency is therefore one
            // in-flight thumbnail decode, never the queued backlog.
            startWorker(WorkerKind::Shared);
        }
        else
        {
            // Start 1 high-priority worker (interactive full-res), rest background (thumbnails).
            // This prevents thumbnail backlogs from blocking main image loads.
            const size_t highCount = 1;
            // m_threadCount is already clamped to kMaxDecodeWorkers (1 high + 4 background).
            const size_t bgCount = m_threadCount - highCount;

            for (size_t i = 0; i < highCount; ++i)
            {
                startWorker(WorkerKind::HighPriority);
            }
            for (size_t i = 0; i < bgCount; ++i)
            {
                startWorker(WorkerKind::Background);
            }
        }
        m_workerCount = m_threads.size();

        // One serialized thumbnail I/O thread: reads files sequentially into memory, then schedules CPU work.
        // This avoids HDD seek thrashing while still allowing CPU decode/resize to run in parallel.
        {
            auto slot = std::make_unique<ThreadSlot>();
            slot->thread = std::thread(&DecodeScheduler::ThumbPrefetchThread, this, slot.get());
            m_threads.push_back(std::move(slot));
        }

        // Stop dispatch (host stop callbacks, OS cancellation) off the callers' threads.
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopQueue.clear();
            m_stopExit = false;
        }
        {
            auto slot = std::make_unique<ThreadSlot>();
            slot->thread = std::thread(&DecodeScheduler::StopDispatchThread, this, slot.get());
            m_threads.push_back(std::move(slot));
        }

        // Start with a conservative global cap; it will be auto-tuned as volumes are observed.
        m_globalPrefetchCap = 4;
        FileByteCache::Instance().SetMaxEntries(m_globalPrefetchCap);
    }

    bool DecodeScheduler::Enqueue(DecodeTask task)
    {
        const bool fullResolution = (task.request.purpose == ImagePurpose::FullResolution);

        // Storage tuning for thumbnail/preview work is resolved before taking the
        // queue mutex: it can block on the host byte source or a volume query,
        // which would otherwise stall every worker, Cancel and Enqueue.
        std::wstring vol;
        size_t desiredDepth = 0;
        if (!fullResolution)
        {
            vol = FileByteCache::GetVolumeForPath(task.request.source);
            desiredDepth = PrefetchDepthForPath(task.request.source);
        }

        task.state = std::make_shared<DecodeRequestState>();
        task.state->priority = task.priority;
        task.state->handle = task.handle;

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_shutdown)
            {
                return false;
            }
            task.state->sequence = ++m_nextSequence;
            if (task.handle != 0)
            {
                m_liveRequests[task.handle] = task.state;
            }

            // FullResolution is user-interactive (main image). Prioritize it.
            if (fullResolution)
            {
                m_highQueue.push(std::move(task));
            }
            else
            {
                // Auto-tune per-volume thumbnail prefetch depth based on storage seek penalty.
                VolumeIoProfile profile {};
                profile.incursSeek = (desiredDepth <= 2);
                profile.prefetchDepth = desiredDepth;
                m_volumeProfiles[vol] = profile;

                // Global cache hard-cap: sum of per-volume depths (bounded), so mixed HDD+SSD doesn't get stuck conservative.
                size_t sumDepth = 0;
                for (const auto& kv : m_volumeProfiles)
                {
                    sumDepth += kv.second.prefetchDepth;
                }
                const size_t hardCap = 8;
                m_globalPrefetchCap = (std::min)(hardCap, (std::max)(static_cast<size_t>(1), sumDepth));
                FileByteCache::Instance().SetMaxEntries(m_globalPrefetchCap);

                // Thumbnail/Preview: go through serialized I/O prefetch stage first.
                m_thumbIoQueue.Push(std::move(task), DecodeRequestState::Stage::Io);
            }
        }
        // Multiple threads wait on the same condition_variable with different queue predicates
        // (high/background/thumb-IO). Using notify_all avoids a missed wakeup where we wake a thread
        // whose predicate is false, leaving the correct worker asleep even though work is available.
        m_queueCondition.notify_all();
        return true;
    }

    void DecodeScheduler::SetPriority(uint64_t handle, RequestPriority priority)
    {
        if (handle == 0)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_liveRequests.find(handle);
            if (it == m_liveRequests.end())
            {
                return;
            }
            DecodeRequestState& state = *it->second;
            if (state.decoding || state.canceled.load() || state.priority == priority)
            {
                return;
            }
            switch (state.queuedIn)
            {
            case DecodeRequestState::Stage::Io:
                (void)m_thumbIoQueue.Reprioritize(state, priority);
                break;
            case DecodeRequestState::Stage::Decode:
                (void)m_backgroundQueue.Reprioritize(state, priority);
                break;
            case DecodeRequestState::Stage::None:
                // Being read, or a full-resolution request: the next stage uses it.
                state.priority = priority;
                break;
            }
        }
        // The prefetch stage may be holding lower-priority work for a slot.
        m_queueCondition.notify_all();
    }

    void DecodeScheduler::Cancel(uint64_t handle)
    {
        if (handle == 0)
        {
            return;
        }

        // The flag lives with the task; the live-request entry is dropped now, so
        // a cancel after completion (or of an unknown handle) never accumulates.
        // A request that is still queued gives back its prefetched bytes and
        // prefetch slot here, so canceled work cannot hold back prefetch; the
        // task itself is discarded when a thread next reaches it. Everything is
        // found through the live-request table: no queue is scanned, so bulk
        // cancellation stays linear. A request already being decoded keeps its
        // bytes until the decode finishes.
        std::wstring releasedPath;
        bool releasedSlot = false;
        std::shared_ptr<DecodeRequestState> canceledState;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_liveRequests.find(handle);
            if (it == m_liveRequests.end())
            {
                return;
            }
            canceledState = it->second;
            DecodeRequestState& state = *it->second;
            state.canceled.store(true);
            if (!state.decoding && state.holdsPrefetchSlot)
            {
                releasedPath = it->second->prefetchPath;
                releasedSlot = ReleasePrefetch_NoLock(state);
            }
            m_liveRequests.erase(it);

            // A read in progress is released through the request's token (and
            // OS cancellation), both dispatched on the stop-dispatch thread, so
            // no host stop callback runs on (or can block) the caller. The stop
            // is published in the same critical section that removes the
            // request: a Shutdown either finds it in the live table or runs
            // after it is queued, so the dispatch thread cannot retire first.
            std::lock_guard<std::mutex> stopLock(m_stopMutex);
            m_stopQueue.push_back(std::move(canceledState));
        }
        m_stopCondition.notify_all();
        RunTestHook(TestPoint::CancelPublished, handle);

        // The request no longer holds its bytes; drop the shared cache entry if
        // nothing else uses it, then wake the prefetch stage for the free slot.
        if (releasedSlot)
        {
            FileByteCache::Instance().EraseIfUnshared(releasedPath);
            m_queueCondition.notify_all();
        }
    }

    void DecodeScheduler::Shutdown()
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
        (void)StopAndJoin(std::nullopt);
    }

    bool DecodeScheduler::Shutdown(std::chrono::milliseconds bound)
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
        return StopAndJoin(std::chrono::steady_clock::now() + bound);
    }

    void DecodeScheduler::SignalExit(ThreadSlot& slot)
    {
        {
            std::lock_guard<std::mutex> lock(m_exitMutex);
            slot.exited = true;
        }
        m_exitCondition.notify_all();
    }

    bool DecodeScheduler::StopAndJoin(std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        // The shutdown flag is part of every wait predicate, so it must change
        // under m_queueMutex: a waiter that has just evaluated its predicate still
        // holds the mutex until it blocks, and a notification sent before that
        // point is lost. Setting the flag outside the lock let notify_all() race
        // that window, leaving a worker asleep forever and the join below hanging.
        std::vector<std::shared_ptr<DecodeRequestState>> live;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_shutdown && !m_joinPending)
            {
                return true;
            }
            m_shutdown = true;
            // Every live request is dropped: none is delivered from here on.
            live.reserve(m_liveRequests.size());
            for (const auto& [handle, state] : m_liveRequests)
            {
                state->canceled.store(true);
                live.push_back(state);
            }
        }
        m_queueCondition.notify_all();

        // A read registered for a request outside the live table (handle 0) is
        // stopped too.
        for (const auto& slot : m_threads)
        {
            std::lock_guard<std::mutex> lock(slot->lease->mutex);
            if (slot->lease->active && !slot->lease->active->canceled.exchange(true))
            {
                live.push_back(slot->lease->active);
            }
        }

        // Stop tokens are signaled on the stop-dispatch thread (host stop
        // callbacks never run here), which then exits; it is joined with the
        // other threads, within the deadline. The OS cancel attempt is made here
        // directly as well, since it runs no host code.
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            for (auto& state : live)
            {
                m_stopQueue.push_back(std::move(state));
            }
            m_stopExit = true;
        }
        m_stopCondition.notify_all();
        CancelActiveReadsIo();

        // Wait for the threads to exit, re-trying the OS cancel meanwhile: it only
        // reaches a call already blocked, and a read may block after an attempt.
        std::vector<ThreadSlot*> exited;
        {
            const auto allExited = [this]
            {
                return std::all_of(m_threads.begin(), m_threads.end(),
                    [](const std::unique_ptr<ThreadSlot>& slot) { return slot->exited; });
            };
            constexpr auto kRetryInterval = std::chrono::milliseconds(50);
            std::unique_lock<std::mutex> lock(m_exitMutex);
            while (!allExited())
            {
                auto wait = std::chrono::steady_clock::duration(kRetryInterval);
                if (deadline)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= *deadline)
                    {
                        break;
                    }
                    wait = (std::min)(wait, *deadline - now);
                }
                if (m_exitCondition.wait_for(lock, wait, allExited))
                {
                    break;
                }
                lock.unlock();
                CancelActiveReadsIo();
                lock.lock();
            }
            for (const auto& slot : m_threads)
            {
                if (slot->exited)
                {
                    exited.push_back(slot.get());
                }
            }
        }

        // Join only threads that have terminated; a stuck one is left running and
        // owned (never detached) for a later call. Signaling exit is the last
        // step of the thread function, but TLS destructors and thread-detach
        // processing still run after it, and they can block (a thread_local
        // created by a byte source or decoder), so wait for the native handle,
        // within the remaining budget, before join().
        size_t joined = 0;
        for (const auto& slot : m_threads)
        {
            if (!slot->thread.joinable())
            {
                ++joined;  // joined by an earlier call
            }
        }
        for (ThreadSlot* slot : exited)
        {
            if (!slot->thread.joinable())
            {
                continue;
            }
            DWORD waitMs = INFINITE;
            if (deadline)
            {
                const auto remaining = *deadline - std::chrono::steady_clock::now();
                waitMs = remaining > std::chrono::steady_clock::duration::zero()
                    ? static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count())
                    : 0;
            }
            if (WaitForSingleObject(static_cast<HANDLE>(slot->thread.native_handle()), waitMs) == WAIT_OBJECT_0)
            {
                slot->thread.join();
                ++joined;
            }
        }
        if (joined != m_threads.size())
        {
            m_joinPending = true;
            IC_LOG_WARN("[DecodeScheduler] Shutdown: {} of {} threads still running after the bound; not joined.",
                m_threads.size() - joined, m_threads.size());
            return false;
        }
        m_joinPending = false;

        // Every read retired before its thread exited; close the lease handles
        // under the lease mutex, so a concurrent Cancel (which only dispatches to
        // a registered read) never uses a closed handle.
        for (auto& slot : m_threads)
        {
            std::lock_guard<std::mutex> lock(slot->lease->mutex);
            slot->lease->active.reset();
            if (slot->lease->thread)
            {
                CloseHandle(slot->lease->thread);
                slot->lease->thread = nullptr;
            }
        }
        m_threads.clear();
        m_workerCount = 0;

        // Drop queued work (no callbacks) and reset per-run state, so a restart
        // cannot inherit stale volume tuning or orphaned prefetched bytes.
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_highQueue = {};
            m_thumbIoQueue = {};
            m_backgroundQueue = {};
            m_prefetchedTotal = 0;
            m_prefetchedByVolume.clear();
            m_liveRequests.clear();
            m_volumeProfiles.clear();
            m_globalPrefetchCap = 4;
        }
        {
            std::lock_guard<std::mutex> lock(m_seekPenaltyMutex);
            m_seekPenaltyByVolume.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopQueue.clear();
        }
        FileByteCache::Instance().Clear();
        FileByteCache::Instance().SetMaxEntries(m_globalPrefetchCap);
        return true;
    }

    void DecodeScheduler::WorkerThread(WorkerKind kind, ThreadSlot* slot)
    {
        BindLeaseToCurrentThread(*slot->lease);

        // Thread scheduling priority follows the task, not the worker:
        // - full-resolution (high queue) work runs above normal for main-image response;
        // - background (thumbnail) work runs in background mode, which lowers CPU and
        //   I/O priority (important for HDD bottlenecks).
        // Background workers idle in background mode and leave it while they run a
        // stolen full-resolution task; the high-priority and shared workers idle above
        // normal and enter background mode only for a background task.
        bool inBackgroundMode = false;
        auto setBackgroundMode = [&inBackgroundMode](bool background)
        {
            if (background == inBackgroundMode)
            {
                return;
            }
            if (background)
            {
                (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            }
            else
            {
                (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            }
            inBackgroundMode = background;
        };
        const bool idleInBackground = (kind == WorkerKind::Background);
        if (idleInBackground)
        {
            setBackgroundMode(true);
        }
        else
        {
            (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }

        // Each worker thread must initialize COM to use WIC/COM components safely.
        bool coInitialized = false;
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr))
        {
            coInitialized = true;
        }
        else if (coHr == RPC_E_CHANGED_MODE)
        {
            coInitialized = false;
        }

        ImageDecodeDispatcher dispatcher;

        while (!m_shutdown)
        {
            DecodeTask task;
            bool tookBackgroundTask = false;
            std::shared_ptr<const std::vector<uint8_t>> bytesHold {};

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                if (kind == WorkerKind::HighPriority)
                {
                    m_queueCondition.wait(lock, [this]
                    {
                        return !m_highQueue.empty() || m_shutdown;
                    });
                }
                else
                {
                    m_queueCondition.wait(lock, [this]
                    {
                        return !m_highQueue.empty() || !m_backgroundQueue.Empty() || m_shutdown;
                    });
                }

                // Queued work is dropped at shutdown (Shutdown canceled it).
                if (m_shutdown)
                {
                    break;
                }

                // High-priority workers never take background tasks.
                if (kind == WorkerKind::HighPriority)
                {
                    if (m_highQueue.empty())
                    {
                        continue;
                    }
                    task = std::move(m_highQueue.front());
                    m_highQueue.pop();
                }
                else
                {
                    // Background and shared workers service the high queue first. Background
                    // workers work-steal (a dedicated high worker also exists); the shared
                    // worker is the only worker and must prefer the main image.
                    if (!m_highQueue.empty())
                    {
                        task = std::move(m_highQueue.front());
                        m_highQueue.pop();
                    }
                    else if (!m_backgroundQueue.Empty())
                    {
                        task = m_backgroundQueue.Pop();
                        tookBackgroundTask = true;
                    }
                    else
                    {
                        continue;
                    }
                }

                // If canceled while sitting in the queue, skip the task.
                if (task.state && task.state->canceled.load())
                {
                    // Cancel already released the bytes and the prefetch slot; this
                    // only discards the task (releasing anything still held).
                    Finish_NoLock(task);
                    if (ReleasePrefetch_NoLock(*task.state))
                    {
                        m_queueCondition.notify_all();
                    }
                    if (task.request.purpose != ImagePurpose::FullResolution)
                    {
                        FileByteCache::Instance().EraseIfUnshared(task.request.source);
                    }
                    continue;
                }

                // From here on Cancel leaves the bytes to this worker, and finds a
                // read this worker runs for the task through its lease.
                if (task.state)
                {
                    task.state->decoding = true;
                    task.state->readLease = slot->lease;
                    bytesHold = task.state->prefetched;
                }
            }

            RunTestHook(TestPoint::TaskTaken, task.handle);

            // Run this task at its own priority (see above).
            setBackgroundMode(tookBackgroundTask);

            // Run decode through the dispatcher (includes format routing + decoder selection).
            // A request canceled before its decode starts is neither decoded nor
            // delivered: without bytes the decoders would open the path themselves,
            // re-reading the file with no way to cancel it.
            PipelineResult result;
            bool skipped = false;
            {
                DecodeInput input {};

                const std::wstring& src = task.request.source;
                const bool isArchivePath = IsArchiveBackedPath(src);

                if (task.request.purpose != ImagePurpose::FullResolution)
                {
                    // Thumbnail/Preview: the bytes the prefetch stage attached to this
                    // request (taken above; never re-looked-up by path, so a same-path
                    // request that was canceled or finished cannot take them away).
                    if (bytesHold && !bytesHold->empty())
                    {
                        input.bytes = std::span<const uint8_t>(bytesHold->data(), bytesHold->size());
                        const size_t headerSize = (std::min)(bytesHold->size(), ImageFormatDetector::kProbeSize);
                        input.header = std::span<const uint8_t>(bytesHold->data(), headerSize);
                    }
                }
                else if (isArchivePath && task.state)
                {
                    // FullResolution from archive: must read via the host byte source
                    bool stopped = false;
                    auto bytes = LeasedRead(*slot->lease, task.state, task.handle, task.request.source, stopped);
                    if (stopped)
                    {
                        skipped = true;
                    }
                    else if (bytes && !bytes->empty())
                    {
                        bytesHold = bytes;
                        input.bytes = std::span<const uint8_t>(bytesHold->data(), bytesHold->size());
                        const size_t headerSize = (std::min)(bytesHold->size(), ImageFormatDetector::kProbeSize);
                        input.header = std::span<const uint8_t>(bytesHold->data(), headerSize);
                    }
                    else
                    {
                        IC_LOG_WARN("[DecodeScheduler] Full-res archive read returned empty bytes.");
                    }
                }

                if (task.state && task.state->canceled.load())
                {
                    skipped = true;
                }

                if (!skipped)
                {
                    RunTestHook(TestPoint::DecodeStart, task.handle);
                    try
                    {
                        result = dispatcher.Decode(task.request, nullptr, input);
                    }
                    catch (...)
                    {
                        // Never let an exception kill the worker thread.
                        result = PipelineResult(E_FAIL);
                    }

                    if (FAILED(result.hr))
                    {
                        IC_LOG_ERROR("[DecodeScheduler] Decode failed ({}): 0x{:08X}",
                            Log::ToUtf8(src),
                            static_cast<unsigned>(result.hr));
                    }
                }
            }

            // Deliver the result unless canceled while decoding. The request leaves the
            // live-request table before delivery, so a Cancel that arrives from here on
            // is a no-op and the callback may run after that Cancel returns.
            bool deliver = false;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                deliver = !skipped && !(task.state && task.state->canceled.load());
                Finish_NoLock(task);
            }
            if (deliver && task.callback)
            {
                task.callback(std::move(result));  // delivered via move
            }

            // Thumbnail/Preview prefetched bytes are transient: give back the
            // prefetch slot and drop the shared cache entry if nothing else uses it.
            if (task.request.purpose != ImagePurpose::FullResolution)
            {
                bytesHold.reset();
                bool released = false;
                {
                    // The prefetch stage's wait predicate reads the slot counts under
                    // m_queueMutex, so they change under it and the notify follows.
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    released = task.state && ReleasePrefetch_NoLock(*task.state);
                }
                FileByteCache::Instance().EraseIfUnshared(task.request.source);
                if (released)
                {
                    m_queueCondition.notify_all();
                }
            }

            setBackgroundMode(idleInBackground);
        }

        // Drop this thread's cached COM objects while the apartment is still alive.
        // Letting the thread_local WIC factory release at thread exit (after
        // CoUninitialize) crashed intermittently once thousands of workers had exited.
        ImageDecodeDispatcher::ReleaseThreadResources();

        if (coInitialized)
        {
            CoUninitialize();
        }

        // End background mode if set (best-effort).
        if (inBackgroundMode)
        {
            (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }

        SignalExit(*slot);
    }

    void DecodeScheduler::ThumbPrefetchThread(ThreadSlot* slot)
    {
        BindLeaseToCurrentThread(*slot->lease);

        // Make thumbnail prefetch low priority (CPU + I/O).
        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

        while (!m_shutdown)
        {
            DecodeTask task;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this]
                {
                    return !m_thumbIoQueue.Empty() || m_shutdown;
                });

                // Queued work is dropped at shutdown (Shutdown canceled it).
                if (m_shutdown)
                {
                    break;
                }

                if (m_thumbIoQueue.Empty())
                {
                    continue;
                }

                task = m_thumbIoQueue.Pop();

                if (task.state && task.state->canceled.load())
                {
                    Finish_NoLock(task);
                    continue;
                }
                if (task.state)
                {
                    task.state->readLease = slot->lease;
                }
            }

            // Ensure this stage is only used for thumbnail/preview.
            if (task.request.purpose == ImagePurpose::FullResolution)
            {
                // Should never happen, but push to high queue just in case.
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    m_highQueue.push(std::move(task));
                }
                m_queueCondition.notify_all();
                continue;
            }

            // Per-volume in-flight cap + global cap.
            const std::wstring vol = FileByteCache::GetVolumeForPath(task.request.source);
            size_t volCap = 2;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                auto it = m_volumeProfiles.find(vol);
                if (it != m_volumeProfiles.end())
                {
                    volCap = it->second.prefetchDepth;
                }
            }

            // Backpressure without Sleep: wait until fewer tasks hold prefetched bytes
            // than the global and per-volume caps allow. A higher-priority request
            // queued (or promoted) meanwhile takes the slot instead: this task goes
            // back to the queue with its age and the loop picks the new top.
            bool yielded = false;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                const auto outranked = [this, &task]
                {
                    return task.state && !m_thumbIoQueue.Empty() &&
                        m_thumbIoQueue.TopPriority() > task.state->priority;
                };
                m_queueCondition.wait(lock, [this, vol, volCap, &outranked]
                {
                    if (m_shutdown || outranked())
                    {
                        return true;
                    }
                    auto it = m_prefetchedByVolume.find(vol);
                    const size_t volCount = (it != m_prefetchedByVolume.end()) ? it->second : 0;
                    return m_prefetchedTotal < m_globalPrefetchCap && volCount < volCap;
                });
                if (m_shutdown)
                {
                    break;
                }
                if (outranked())
                {
                    m_thumbIoQueue.Push(std::move(task), DecodeRequestState::Stage::Io);
                    yielded = true;
                }
            }
            if (yielded)
            {
                continue;
            }

            // Read file bytes sequentially on this single thread. The shared cache
            // only de-duplicates reads for same-path tasks; the bytes are attached
            // to this task, which owns them from here on. A canceled read is never
            // cached, so a same-path request behind it reads its own bytes.
            std::shared_ptr<const std::vector<uint8_t>> bytes = FileByteCache::Instance().Get(task.request.source);
            bool stopped = false;
            if (!bytes)
            {
                auto read = LeasedRead(*slot->lease, task.state, task.handle, task.request.source, stopped);
                if (read && !task.state->canceled.load())
                {
                    FileByteCache::Instance().Put(task.request.source, read);
                    bytes = std::move(read);
                }
            }

            // Schedule CPU decode work unless canceled; the decoder falls back to the
            // file path if an uncanceled prefetch failed.
            bool canceled = false;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                canceled = stopped || task.state->canceled.load();
                if (canceled)
                {
                    // Canceled while its bytes were being read (it was in no queue).
                    Finish_NoLock(task);
                }
                else
                {
                    if (bytes && task.state)
                    {
                        task.state->prefetched = std::move(bytes);
                        task.state->holdsPrefetchSlot = true;
                        task.state->prefetchVolume = vol;
                        task.state->prefetchPath = task.request.source;
                        ++m_prefetchedTotal;
                        ++m_prefetchedByVolume[vol];
                    }
                    m_backgroundQueue.Push(std::move(task), DecodeRequestState::Stage::Decode);
                }
            }
            if (canceled)
            {
                bytes.reset();
                FileByteCache::Instance().EraseIfUnshared(task.request.source);
                continue;
            }
            m_queueCondition.notify_all();
        }

        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);

        SignalExit(*slot);
    }
}

