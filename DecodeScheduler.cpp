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

        static std::shared_ptr<std::vector<uint8_t>> ReadWholeFileBytes(const std::wstring& path)
        {
            IPathByteSource* source = GetPathByteSource();
            if (!source)
            {
                return nullptr;
            }

            // Host adapter reads disk files and archive entries.
            std::vector<uint8_t> data = source->ReadAll(path);
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
        return m_highWorkers.size() + m_backgroundWorkers.size();
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
        // Shutdown also resets volume tuning and FileByteCache (restart semantics).
        Shutdown();

        m_threadCount = std::clamp<size_t>(count, 1, kMaxDecodeWorkers);
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shutdown = false;
        }

        if (m_threadCount == 1)
        {
            // A single worker must serve both queues: full-res first, background work only
            // when the high queue is empty. Worst-case main-image latency is therefore one
            // in-flight thumbnail decode, never the queued backlog.
            m_highWorkers.emplace_back(&DecodeScheduler::WorkerThread, this, WorkerKind::Shared);
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
                m_highWorkers.emplace_back(&DecodeScheduler::WorkerThread, this, WorkerKind::HighPriority);
            }
            for (size_t i = 0; i < bgCount; ++i)
            {
                m_backgroundWorkers.emplace_back(&DecodeScheduler::WorkerThread, this, WorkerKind::Background);
            }
        }

        // One serialized thumbnail I/O thread: reads files sequentially into memory, then schedules CPU work.
        // This avoids HDD seek thrashing while still allowing CPU decode/resize to run in parallel.
        m_thumbPrefetchWorker = std::thread(&DecodeScheduler::ThumbPrefetchThread, this);

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

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_shutdown)
            {
                return false;
            }
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
                m_thumbIoQueue.push(std::move(task));
            }
        }
        // Multiple threads wait on the same condition_variable with different queue predicates
        // (high/background/thumb-IO). Using notify_all avoids a missed wakeup where we wake a thread
        // whose predicate is false, leaving the correct worker asleep even though work is available.
        m_queueCondition.notify_all();
        return true;
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
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_liveRequests.find(handle);
            if (it == m_liveRequests.end())
            {
                return;
            }
            DecodeRequestState& state = *it->second;
            state.canceled.store(true);
            if (!state.decoding && state.holdsPrefetchSlot)
            {
                releasedPath = it->second->prefetchPath;
                releasedSlot = ReleasePrefetch_NoLock(state);
            }
            m_liveRequests.erase(it);
        }

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
        // The shutdown flag is part of every wait predicate, so it must change
        // under m_queueMutex: a waiter that has just evaluated its predicate still
        // holds the mutex until it blocks, and a notification sent before that
        // point is lost. Setting the flag outside the lock let notify_all() race
        // that window, leaving a worker asleep forever and the join below hanging.
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_shutdown)
            {
                return;
            }
            m_shutdown = true;
        }
        m_queueCondition.notify_all();

        for (auto& worker : m_highWorkers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        for (auto& worker : m_backgroundWorkers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        if (m_thumbPrefetchWorker.joinable())
        {
            m_thumbPrefetchWorker.join();
        }

        m_highWorkers.clear();
        m_backgroundWorkers.clear();

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
        FileByteCache::Instance().Clear();
        FileByteCache::Instance().SetMaxEntries(m_globalPrefetchCap);
    }

    void DecodeScheduler::WorkerThread(WorkerKind kind)
    {
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
                        return !m_highQueue.empty() || !m_backgroundQueue.empty() || m_shutdown;
                    });
                }

                if (m_shutdown && m_highQueue.empty() && m_backgroundQueue.empty())
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
                    else if (!m_backgroundQueue.empty())
                    {
                        task = std::move(m_backgroundQueue.front());
                        m_backgroundQueue.pop();
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

                // From here on Cancel leaves the bytes to this worker.
                if (task.state)
                {
                    task.state->decoding = true;
                    bytesHold = task.state->prefetched;
                }
            }

            // Run this task at its own priority (see above).
            setBackgroundMode(tookBackgroundTask);

            // Run decode through the dispatcher (includes format routing + decoder selection)
            PipelineResult result;
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
                else if (isArchivePath)
                {
                    // FullResolution from archive: must read via the host byte source
                    auto bytes = ReadWholeFileBytes(task.request.source);
                    if (bytes && !bytes->empty())
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

            // Deliver the result unless canceled while decoding. The request leaves the
            // live-request table before delivery, so a Cancel that arrives from here on
            // is a no-op and the callback may run after that Cancel returns.
            bool deliver = false;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                deliver = !(task.state && task.state->canceled.load());
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
    }

    void DecodeScheduler::ThumbPrefetchThread()
    {
        // Make thumbnail prefetch low priority (CPU + I/O).
        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

        while (!m_shutdown)
        {
            DecodeTask task;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this]
                {
                    return !m_thumbIoQueue.empty() || m_shutdown;
                });

                if (m_shutdown && m_thumbIoQueue.empty())
                {
                    break;
                }

                if (m_thumbIoQueue.empty())
                {
                    continue;
                }

                task = std::move(m_thumbIoQueue.front());
                m_thumbIoQueue.pop();

                if (task.state && task.state->canceled.load())
                {
                    Finish_NoLock(task);
                    continue;
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
            // than the global and per-volume caps allow.
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this, vol, volCap]
                {
                    if (m_shutdown)
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
            }

            // Read file bytes sequentially on this single thread. The shared cache
            // only de-duplicates reads for same-path tasks; the bytes are attached
            // to this task, which owns them from here on.
            std::shared_ptr<const std::vector<uint8_t>> bytes = FileByteCache::Instance().Get(task.request.source);
            if (!bytes)
            {
                auto read = ReadWholeFileBytes(task.request.source);
                if (read)
                {
                    FileByteCache::Instance().Put(task.request.source, read);
                    bytes = std::move(read);
                }
            }

            // Schedule CPU decode work regardless; the decoder falls back to the file
            // path if the prefetch failed.
            bool canceled = false;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                canceled = task.state && task.state->canceled.load();
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
                    m_backgroundQueue.push(std::move(task));
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
    }
}

