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

        // Device and seek penalty of a volume ("C:"), through its volume device
        // "\\.\C:". Several volume letters on one disk report the same device
        // number; a volume the query cannot place (spanned volume, RAM disk,
        // an unopenable device) is its own device. A failed seek-penalty query
        // is conservative: seek penalty assumed.
        static DecodeScheduler::VolumeTopology QueryVolumeTopology(const std::wstring& volume)
        {
            DecodeScheduler::VolumeTopology topology;
            topology.device = L"volume:" + volume;
            topology.incursSeekPenalty = true;

            const std::wstring devicePath = L"\\\\.\\" + volume;
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
                return topology;
            }

            STORAGE_DEVICE_NUMBER number {};
            DWORD bytesReturned = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number),
                &bytesReturned, nullptr) && bytesReturned >= sizeof(number))
            {
                topology.device = L"disk:" + std::to_wstring(number.DeviceType) + L":" +
                    std::to_wstring(number.DeviceNumber);
            }

            STORAGE_PROPERTY_QUERY query {};
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            query.QueryType = PropertyStandardQuery;

            STORAGE_SEEK_PENALTY_DESCRIPTOR desc {};
            bytesReturned = 0;
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

            if (ok && bytesReturned >= sizeof(desc) && desc.Version != 0 && desc.Size != 0)
            {
                topology.incursSeekPenalty = desc.IncursSeekPenalty ? true : false;
            }
            return topology;
        }

        std::wstring Lowercase(std::wstring text)
        {
            for (auto& c : text)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            return text;
        }

        // Lexical root of a path for Remote lanes and for paths without a drive
        // letter: the server of a UNC path ("\\server\share\..." or
        // "\\?\UNC\server\share\..."), lower-cased, or empty.
        std::wstring UncServerOf(const std::wstring& path)
        {
            size_t start = std::wstring::npos;
            if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
            {
                start = 8;
            }
            else if (path.rfind(L"\\\\", 0) == 0 && path.rfind(L"\\\\?\\", 0) != 0 && path.rfind(L"\\\\.\\", 0) != 0)
            {
                start = 2;
            }
            if (start == std::wstring::npos || start >= path.size())
            {
                return {};
            }
            const size_t end = path.find_first_of(L"\\/", start);
            return Lowercase(path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        }

        // Drive volume ("C:") of a path, also behind a "\\?\" prefix; empty
        // when the path has no drive letter.
        std::wstring DriveVolumeOf(const std::wstring& path)
        {
            const std::wstring_view view = (path.rfind(L"\\\\?\\", 0) == 0) ? std::wstring_view(path).substr(4)
                                                                         : std::wstring_view(path);
            if (view.size() >= 2 && view[1] == L':' && iswalpha(view[0]))
            {
                return std::wstring { static_cast<wchar_t>(towupper(view[0])), L':' };
            }
            return {};
        }

        size_t LaneIndex(IoLane lane)
        {
            return static_cast<size_t>(lane);
        }

        const wchar_t* LaneTag(IoLane lane)
        {
            switch (lane)
            {
            case IoLane::Archive: return L"archive";
            case IoLane::Remote: return L"remote";
            case IoLane::Local: break;
            }
            return L"local";
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

        // Published for GetDiagnostics only (relaxed stores, never read for
        // scheduling decisions).
        Diagnostics::Role role { Diagnostics::Role::Background };
        IoLane lane { IoLane::Local };
        std::atomic<Diagnostics::Phase> phase { Diagnostics::Phase::Starting };
        std::atomic<uint64_t> handle { 0 };
        std::atomic<bool> backgroundMode { false };

        void Publish(Diagnostics::Phase newPhase, uint64_t newHandle)
        {
            handle.store(newHandle, std::memory_order_relaxed);
            phase.store(newPhase, std::memory_order_relaxed);
        }
    };

    // One I/O lane (all fields guarded by the queue mutex). See the design
    // note in DecodeScheduler.h.
    struct DecodeScheduler::Lane
    {
        std::wstring key;
        IoLane kind { IoLane::Local };
        std::wstring device;          // admission key (Local/Archive lanes)
        size_t depth { 2 };           // thumbnail tasks this lane may hold bytes for
        size_t deviceLimit { kMaxReadsPerSeekDevice };

        PriorityTaskQueue queue;      // thumbnail/preview requests
        PriorityTaskQueue mainQueue;  // full-resolution requests
        bool reading { false };       // a thumbnail read in progress
        bool mainReading { false };   // a main-image read in progress
        size_t held { 0 };            // thumbnail tasks holding bytes read here
        uint64_t lastServed { 0 };    // m_serviceClock at the last thumbnail read (0: never)
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

    uint64_t PriorityTaskQueue::TopSequence() const
    {
        return m_tasks.begin()->first.sequence;
    }

    bool PriorityTaskQueue::Remove(DecodeRequestState& state, DecodeTask& out)
    {
        auto it = m_tasks.find(KeyOf(state.priority, state.sequence));
        if (it == m_tasks.end() || it->second.state.get() != &state)
        {
            return false;
        }
        out = std::move(it->second);
        m_tasks.erase(it);
        state.queuedIn = DecodeRequestState::Stage::None;
        return true;
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

    void DecodeScheduler::SetVolumeTopologyQuery(VolumeTopologyQuery query)
    {
        std::lock_guard<std::mutex> lock(m_topologyMutex);
        m_topologyQuery = std::move(query);
        m_topologyByVolume.clear();
    }

    DecodeScheduler::VolumeTopology DecodeScheduler::TopologyForVolume(const std::wstring& volume)
    {
        // The device query opens the volume device and can block, so it runs
        // without any lock held; its result is cached per volume and refreshed
        // after kTopologyRefreshInterval (a drive letter can be remapped).
        const auto now = std::chrono::steady_clock::now();
        VolumeTopologyQuery query;
        {
            std::lock_guard<std::mutex> lock(m_topologyMutex);
            auto it = m_topologyByVolume.find(volume);
            if (it != m_topologyByVolume.end() && now - it->second.queried < kTopologyRefreshInterval)
            {
                return it->second.topology;
            }
            query = m_topologyQuery;
        }

        VolumeTopology topology;
        try
        {
            topology = query ? query(volume) : QueryVolumeTopology(volume);
        }
        catch (...)
        {
            topology = VolumeTopology { L"volume:" + volume, true };
        }
        if (topology.device.empty())
        {
            topology.device = L"volume:" + volume;
        }

        std::lock_guard<std::mutex> lock(m_topologyMutex);
        m_topologyByVolume[volume] = CachedTopology { topology, now };
        return topology;
    }

    DecodeScheduler::Route DecodeScheduler::RouteFor(const ImageRequest& request, IoLane lane)
    {
        const bool mainImage = (request.purpose == ImagePurpose::FullResolution);
        Route route;
        route.lane = lane;
        const std::wstring& path = request.source;

        if (lane == IoLane::Remote)
        {
            // Never a local device query for a remote path: the lane is the
            // server (UNC) or the drive letter, lexically.
            std::wstring root = UncServerOf(path);
            if (root.empty())
            {
                root = DriveVolumeOf(path);
            }
            route.device = L"remote:" + root;
            route.depth = 2;
            route.deviceLimit = kReadersPerLane;  // not device-admitted
            route.key = std::wstring(LaneTag(lane)) + L"|" + root;
            return route;
        }

        // Local and Archive lanes are admitted against the device that holds
        // the file (for an archive entry, the host archive file).
        std::wstring host = path;
        if (lane == IoLane::Archive)
        {
            if (IPathByteSource* source = GetPathByteSource())
            {
                try
                {
                    host = source->ResolveHostPath(path);
                }
                catch (...)
                {
                    host = path;
                }
            }
        }

        const std::wstring volume = DriveVolumeOf(host);
        VolumeTopology topology;
        if (!volume.empty())
        {
            topology = TopologyForVolume(volume);
        }
        else
        {
            // No drive letter: a UNC host is its own device; anything else
            // shares one conservative unknown device.
            const std::wstring server = UncServerOf(host);
            topology.device = server.empty() ? L"unknown" : L"unc:" + server;
            topology.incursSeekPenalty = true;
        }

        route.device = topology.device;
        // SSD/NVMe (no seek penalty) -> deeper prefetch helps hide latency.
        // HDD (seek penalty) -> keep shallow to avoid memory + avoid extra I/O pressure.
        route.depth = topology.incursSeekPenalty ? 2 : 4;
        route.deviceLimit = topology.incursSeekPenalty ? kMaxReadsPerSeekDevice : kMaxReadsPerSolidStateDevice;
        route.key = std::wstring(LaneTag(lane)) + L"|" + topology.device;
        if (lane == IoLane::Archive && (mainImage || !topology.incursSeekPenalty))
        {
            // Without a seek penalty, each archive is its own lane (bounded by
            // the archive readers and the device limit). Main-image reads are
            // not device-admitted, so they are serialized per archive only.
            route.key += L"|" + Lowercase(host);
        }
        return route;
    }

    bool DecodeScheduler::ThumbnailAdmissible_NoLock(const Lane& lane) const
    {
        if (lane.queue.Empty() || lane.reading || lane.held >= lane.depth)
        {
            return false;
        }
        if (lane.kind == IoLane::Remote)
        {
            return true;
        }
        auto it = m_readsByDevice.find(lane.device);
        const size_t reads = (it != m_readsByDevice.end()) ? it->second : 0;
        return reads < lane.deviceLimit;
    }

    DecodeScheduler::Lane* DecodeScheduler::SelectThumbnailLane_NoLock(IoLane kind)
    {
        // Global count bound: tasks holding bytes plus reads in progress.
        const size_t used = m_prefetchedTotal + m_prefetchReads;
        if (used >= m_globalPrefetchCap)
        {
            return nullptr;
        }
        const size_t freeSlots = m_globalPrefetchCap - used;

        // Priority, then least recently served lane, then the oldest request.
        const auto better = [](const Lane& a, const Lane& b)
        {
            const auto pa = a.queue.TopPriority();
            const auto pb = b.queue.TopPriority();
            if (pa != pb)
            {
                return pa > pb;
            }
            if (a.lastServed != b.lastServed)
            {
                return a.lastServed < b.lastServed;
            }
            return a.queue.TopSequence() < b.queue.TopSequence();
        };

        Lane* best = nullptr;
        for (auto& [key, lane] : m_lanes)
        {
            if (lane->kind == kind && ThumbnailAdmissible_NoLock(*lane) && (!best || better(*lane, *best)))
            {
                best = lane.get();
            }
        }
        if (!best)
        {
            return nullptr;
        }

        // Priority across lane kinds for the last free slots: leave them to
        // higher-priority work that an idle reader of another kind can take.
        const RequestPriority priority = best->queue.TopPriority();
        size_t ahead = 0;
        for (IoLane other : { IoLane::Local, IoLane::Archive, IoLane::Remote })
        {
            if (other == kind)
            {
                continue;
            }
            const size_t busy = m_readsInProgress[LaneIndex(other)];
            const size_t idle = busy < kReadersPerLane ? kReadersPerLane - busy : 0;
            size_t higher = 0;
            for (auto& [key, lane] : m_lanes)
            {
                if (lane->kind == other && ThumbnailAdmissible_NoLock(*lane) && lane->queue.TopPriority() > priority)
                {
                    ++higher;
                }
            }
            ahead += (std::min)(idle, higher);
        }
        return ahead < freeSlots ? best : nullptr;
    }

    DecodeScheduler::Lane* DecodeScheduler::SelectMainLane_NoLock()
    {
        if (m_mainReads + m_mainHeld >= kMainImageInputCap)
        {
            return nullptr;
        }
        Lane* best = nullptr;
        for (auto& [key, lane] : m_lanes)
        {
            if (lane->mainQueue.Empty() || lane->mainReading)
            {
                continue;
            }
            if (!best ||
                lane->mainQueue.TopPriority() > best->mainQueue.TopPriority() ||
                (lane->mainQueue.TopPriority() == best->mainQueue.TopPriority() &&
                    lane->mainQueue.TopSequence() < best->mainQueue.TopSequence()))
            {
                best = lane.get();
            }
        }
        return best;
    }

    void DecodeScheduler::ReclaimLane_NoLock(const std::wstring& key)
    {
        auto it = m_lanes.find(key);
        if (it == m_lanes.end())
        {
            return;
        }
        const Lane& lane = *it->second;
        if (lane.queue.Empty() && lane.mainQueue.Empty() && !lane.reading && !lane.mainReading && lane.held == 0)
        {
            m_lanes.erase(it);
        }
    }

    void DecodeScheduler::SetTestHook(TestHook hook)
    {
        m_testHook = std::move(hook);
    }

    void DecodeScheduler::SetBackgroundWorkPriority(BackgroundWorkPriority priority)
    {
        m_backgroundWorkPriority.store(priority, std::memory_order_relaxed);
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
        // the token is signaled by the stop-dispatch thread. So is the shutdown
        // flag: a read that registers after Shutdown scanned the leases (which
        // the dispatch thread may outlive only briefly) must not start, and one
        // registered before the scan is found by it and stopped.
        std::shared_ptr<std::vector<uint8_t>> bytes;
        if (!stop.stop_requested() && !state->canceled.load() && !m_shutdown.load())
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

        stopped = stop.stop_requested() || state->canceled.load() || m_shutdown.load();
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
            slot->Publish(Diagnostics::Phase::Idle, 0);
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
            slot->Publish(Diagnostics::Phase::Stopping, state->handle);
            StopRequest(state);
            RunTestHook(TestPoint::StopDispatched, state->handle);
        }

        slot->Publish(Diagnostics::Phase::Exited, 0);
        RunTestHook(TestPoint::StopDispatchExit, 0);
        SignalExit(*slot);
    }

    void DecodeScheduler::RaiseThreadPriorities()
    {
        // A thread in background mode runs at base priority 4, below every
        // normal-priority thread: on a CPU-saturated machine it is scheduled only
        // by the OS starvation boost, once every few seconds, so it cannot see the
        // shutdown flag and exit within a bound (an unbounded join just takes that
        // long). Only the thread itself can leave background mode, but setting its
        // priority through its handle replaces the background CPU priority, so it
        // runs to its exit. Joinable handles stay valid until joined.
        for (const auto& slot : m_threads)
        {
            if (slot->thread.joinable())
            {
                (void)SetThreadPriority(static_cast<HANDLE>(slot->thread.native_handle()),
                    THREAD_PRIORITY_ABOVE_NORMAL);
            }
        }
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
        if (state.holdsMainInput)
        {
            state.holdsMainInput = false;
            if (m_mainHeld > 0)
            {
                --m_mainHeld;
            }
            return true;
        }
        if (!state.holdsPrefetchSlot)
        {
            return false;
        }
        state.holdsPrefetchSlot = false;
        if (m_prefetchedTotal > 0)
        {
            --m_prefetchedTotal;
        }
        auto it = m_lanes.find(state.laneKey);
        if (it != m_lanes.end())
        {
            if (it->second->held > 0)
            {
                --it->second->held;
            }
            ReclaimLane_NoLock(state.laneKey);
        }
        return true;
    }

    size_t DecodeScheduler::PrefetchedTaskCount() const
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_prefetchedTotal;
    }

    DecodeScheduler::Diagnostics DecodeScheduler::GetDiagnostics() const
    {
        Diagnostics diagnostics;

        // m_threads changes only under m_lifecycleMutex, which a restart or a
        // Shutdown holds while it joins; never wait behind a stuck join.
        std::unique_lock<std::mutex> lifecycle(m_lifecycleMutex, std::defer_lock);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (!lifecycle.try_lock() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (lifecycle.owns_lock())
        {
            diagnostics.threadsListed = true;
            for (const auto& slot : m_threads)
            {
                Diagnostics::Thread thread;
                thread.role = slot->role;
                thread.lane = slot->lane;
                thread.phase = slot->phase.load(std::memory_order_relaxed);
                thread.handle = slot->handle.load(std::memory_order_relaxed);
                thread.backgroundMode = slot->backgroundMode.load(std::memory_order_relaxed);
                if (slot->thread.joinable())
                {
                    const HANDLE handle = static_cast<HANDLE>(const_cast<std::thread&>(slot->thread).native_handle());
                    thread.threadId = GetThreadId(handle);
                    thread.priority = GetThreadPriority(handle);
                    FILETIME created {}, exited {}, kernel {}, user {};
                    if (GetThreadTimes(handle, &created, &exited, &kernel, &user))
                    {
                        const auto ticks = [](const FILETIME& time)
                        {
                            return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                        };
                        thread.cpuMs = static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
                    }
                }
                else
                {
                    thread.phase = Diagnostics::Phase::Exited;
                }
                diagnostics.threads.push_back(thread);
            }
        }

        std::lock_guard<std::mutex> lock(m_queueMutex);
        diagnostics.shutdown = m_shutdown.load();
        diagnostics.highQueued = m_highQueue.size();
        for (const auto& [key, lane] : m_lanes)
        {
            diagnostics.ioQueued += lane->queue.Size();
            diagnostics.mainIoQueued += lane->mainQueue.Size();
        }
        diagnostics.decodeQueued = m_backgroundQueue.Size();
        diagnostics.liveRequests = m_liveRequests.size();
        diagnostics.prefetchedTasks = m_prefetchedTotal;
        diagnostics.prefetchReads = m_prefetchReads;
        diagnostics.prefetchCap = m_globalPrefetchCap;
        diagnostics.mainInputs = m_mainReads + m_mainHeld;
        diagnostics.lanes = m_lanes.size();
        return diagnostics;
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
            slot->role = (kind == WorkerKind::HighPriority) ? Diagnostics::Role::HighPriority
                : (kind == WorkerKind::Shared) ? Diagnostics::Role::Shared
                : Diagnostics::Role::Background;
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

        // I/O lane readers: a fixed pool per logical lane, plus the main-image
        // readers for Archive/Remote full-resolution requests. Each lane reads
        // sequentially (one read per lane, admitted per device), then schedules
        // CPU work, so HDDs are not thrashed while decode/resize runs in parallel.
        for (IoLane lane : { IoLane::Local, IoLane::Archive, IoLane::Remote })
        {
            for (size_t i = 0; i < kReadersPerLane; ++i)
            {
                auto slot = std::make_unique<ThreadSlot>();
                slot->role = Diagnostics::Role::Prefetch;
                slot->lane = lane;
                slot->thread = std::thread(&DecodeScheduler::LaneReaderThread, this, lane, slot.get());
                m_threads.push_back(std::move(slot));
            }
        }
        for (size_t i = 0; i < kMainImageReaders; ++i)
        {
            auto slot = std::make_unique<ThreadSlot>();
            slot->role = Diagnostics::Role::MainReader;
            slot->thread = std::thread(&DecodeScheduler::MainReaderThread, this, slot.get());
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
            slot->role = Diagnostics::Role::StopDispatch;
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

        // The lane is resolved before taking the queue mutex: classification,
        // the host path and the volume device query can block on the host byte
        // source or the OS, which would otherwise stall every reader, worker,
        // Cancel and Enqueue.
        PathSourceKind kind = PathSourceKind::Unknown;
        if (IPathByteSource* source = GetPathByteSource())
        {
            try
            {
                kind = source->ClassifyPath(task.request.source);
            }
            catch (...)
            {
                kind = PathSourceKind::Unknown;
            }
        }
        const IoLane laneKind = IoLaneFor(kind);
        // Local full-resolution requests are read by the decoder itself.
        const bool needsLane = !fullResolution || laneKind != IoLane::Local;
        Route route;
        if (needsLane)
        {
            route = RouteFor(task.request, laneKind);
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

            if (!needsLane)
            {
                // FullResolution is user-interactive (main image). Prioritize it.
                m_highQueue.push(std::move(task));
            }
            else
            {
                std::unique_ptr<Lane>& lane = m_lanes[route.key];
                if (!lane)
                {
                    lane = std::make_unique<Lane>();
                    lane->key = route.key;
                    lane->kind = route.lane;
                    lane->device = route.device;
                }
                // The latest topology applies to the lane's admission.
                lane->depth = route.depth;
                lane->deviceLimit = route.deviceLimit;
                task.state->laneKey = route.key;

                if (fullResolution)
                {
                    lane->mainQueue.Push(std::move(task), DecodeRequestState::Stage::MainIo);
                }
                else
                {
                    // Prefetch depth per device; the global cap (and FileByteCache's
                    // entry limit) is their sum, bounded, so mixed HDD+SSD doesn't get
                    // stuck conservative.
                    m_volumeProfiles[route.device] = route.depth;
                    size_t sumDepth = 0;
                    for (const auto& kv : m_volumeProfiles)
                    {
                        sumDepth += kv.second;
                    }
                    m_globalPrefetchCap = (std::min)(kMaxPrefetchCap, (std::max)(static_cast<size_t>(1), sumDepth));
                    FileByteCache::Instance().SetMaxEntries(m_globalPrefetchCap);

                    lane->queue.Push(std::move(task), DecodeRequestState::Stage::Io);
                }
            }
        }
        // Multiple threads wait on the same condition_variable with different predicates
        // (high/background queues, lanes). Using notify_all avoids a missed wakeup where we wake a
        // thread whose predicate is false, leaving the correct one asleep even though work is available.
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
            case DecodeRequestState::Stage::MainIo:
            {
                auto lane = m_lanes.find(state.laneKey);
                if (lane != m_lanes.end())
                {
                    PriorityTaskQueue& queue = (state.queuedIn == DecodeRequestState::Stage::Io)
                        ? lane->second->queue : lane->second->mainQueue;
                    (void)queue.Reprioritize(state, priority);
                }
                break;
            }
            case DecodeRequestState::Stage::Decode:
                (void)m_backgroundQueue.Reprioritize(state, priority);
                break;
            case DecodeRequestState::Stage::None:
                // Being read, or a full-resolution request: the next stage uses it.
                state.priority = priority;
                break;
            }
        }
        // A promoted request may now win the next free reader or slot.
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
        // A request waiting in an I/O lane is removed from it here (it holds no
        // capacity; an emptied lane is reclaimed), and one waiting for decode
        // gives back its read bytes and capacity, so canceled work cannot hold
        // back prefetch; a task waiting for decode is discarded when a worker
        // next reaches it. Everything is found through the live-request table
        // and the lane key: no queue is scanned, so bulk cancellation stays
        // O(n log n). A request already being decoded keeps its bytes until the
        // decode finishes.
        std::wstring releasedPath;
        bool releasedSlot = false;
        bool removedFromLane = false;
        DecodeTask removed;  // destroyed after the locks are released
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
            if (state.queuedIn == DecodeRequestState::Stage::Io || state.queuedIn == DecodeRequestState::Stage::MainIo)
            {
                auto lane = m_lanes.find(state.laneKey);
                if (lane != m_lanes.end())
                {
                    PriorityTaskQueue& queue = (state.queuedIn == DecodeRequestState::Stage::Io)
                        ? lane->second->queue : lane->second->mainQueue;
                    removedFromLane = queue.Remove(state, removed);
                    ReclaimLane_NoLock(state.laneKey);
                }
            }
            if (!state.decoding && (state.holdsPrefetchSlot || state.holdsMainInput))
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
        // nothing else uses it, then wake the readers for the free capacity (a
        // removed lane task can also unblock another lane kind's slot choice).
        if (releasedSlot)
        {
            FileByteCache::Instance().EraseIfUnshared(releasedPath);
        }
        if (releasedSlot || removedFromLane)
        {
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
        RaiseThreadPriorities();

        // Every read registered now is stopped too: one for a request outside
        // the live table (handle 0), and one whose Cancel already removed it
        // from the table (stopping twice is harmless). A read that registers
        // after this scan sees m_shutdown and does not start (see LeasedRead),
        // so the dispatch thread may retire once this queue is drained.
        for (const auto& slot : m_threads)
        {
            std::lock_guard<std::mutex> lock(slot->lease->mutex);
            if (slot->lease->active)
            {
                slot->lease->active->canceled.store(true);
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
        // The priority raise is repeated too: a thread that took a background
        // task just before shutdown may enter background mode after the raise.
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
                RaiseThreadPriorities();
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
            m_backgroundQueue = {};
            m_lanes.clear();
            m_readsByDevice.clear();
            std::fill(std::begin(m_readsInProgress), std::end(m_readsInProgress), size_t { 0 });
            m_serviceClock = 0;
            m_prefetchedTotal = 0;
            m_prefetchReads = 0;
            m_mainReads = 0;
            m_mainHeld = 0;
            m_liveRequests.clear();
            m_volumeProfiles.clear();
            m_globalPrefetchCap = 4;
        }
        {
            std::lock_guard<std::mutex> lock(m_topologyMutex);
            m_topologyByVolume.clear();
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
        // Every worker idles above normal and is in background mode only while it runs
        // a background task. A thread waiting in background mode would wake (for a
        // stolen full-resolution task, or for shutdown) at background priority, which
        // normal-priority load starves for seconds before it can even leave the mode.
        // With BackgroundWorkPriority::Normal (a test seam) background tasks run at
        // normal priority instead, still below full-resolution work.
        bool inBackgroundMode = false;
        bool inBackgroundTask = false;
        auto setBackgroundTask = [this, &inBackgroundMode, &inBackgroundTask, slot](bool background)
        {
            if (background == inBackgroundTask)
            {
                return;
            }
            if (background)
            {
                if (m_backgroundWorkPriority.load(std::memory_order_relaxed) == BackgroundWorkPriority::BackgroundMode)
                {
                    slot->backgroundMode.store(true, std::memory_order_relaxed);
                    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                    inBackgroundMode = true;
                }
                else
                {
                    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                }
            }
            else
            {
                if (inBackgroundMode)
                {
                    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                    slot->backgroundMode.store(false, std::memory_order_relaxed);
                    inBackgroundMode = false;
                }
                (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            }
            inBackgroundTask = background;
        };
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

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

            slot->Publish(Diagnostics::Phase::Idle, 0);
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

            slot->Publish(Diagnostics::Phase::Taken, task.handle);
            RunTestHook(TestPoint::TaskTaken, task.handle);

            // Run this task at its own priority (see above).
            setBackgroundTask(tookBackgroundTask);

            // Run decode through the dispatcher (includes format routing + decoder selection).
            // A request canceled before its decode starts is neither decoded nor
            // delivered: without bytes the decoders would open the path themselves,
            // re-reading the file with no way to cancel it.
            PipelineResult result;
            bool skipped = false;
            {
                DecodeInput input {};

                const std::wstring& src = task.request.source;

                // The bytes an I/O lane attached to this request: thumbnail/preview
                // bytes, or the input of an Archive/Remote full-resolution request
                // read by a main-image reader (taken above; never re-looked-up by
                // path, so a same-path request that was canceled or finished cannot
                // take them away). A local full-resolution request has none: the
                // decoder reads its file.
                if (bytesHold && !bytesHold->empty())
                {
                    input.bytes = std::span<const uint8_t>(bytesHold->data(), bytesHold->size());
                    const size_t headerSize = (std::min)(bytesHold->size(), ImageFormatDetector::kProbeSize);
                    input.header = std::span<const uint8_t>(bytesHold->data(), headerSize);
                }

                // Work taken just before shutdown began is dropped as well (a
                // handle-0 task is in no live table, so Shutdown cannot mark it).
                if ((task.state && task.state->canceled.load()) || m_shutdown.load())
                {
                    skipped = true;
                }

                if (!skipped && task.state && task.state->slowReadFailed)
                {
                    // No path fallback: it would block this worker on the slow source.
                    result = PipelineResult(HRESULT_FROM_WIN32(ERROR_READ_FAULT));
                    IC_LOG_ERROR("[DecodeScheduler] Slow-source read failed ({})", Log::ToUtf8(src));
                }
                else if (!skipped)
                {
                    RunTestHook(TestPoint::DecodeStart, task.handle);
                    slot->Publish(Diagnostics::Phase::Decoding, task.handle);
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
                slot->Publish(Diagnostics::Phase::Delivering, task.handle);
                task.callback(std::move(result));  // delivered via move
            }

            // Read bytes are transient: give back the prefetch slot (or main-image
            // input) and drop the shared cache entry if nothing else uses it.
            {
                bytesHold.reset();
                bool released = false;
                {
                    // The readers' selection reads the capacity counts under
                    // m_queueMutex, so they change under it and the notify follows.
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    released = task.state && ReleasePrefetch_NoLock(*task.state);
                }
                if (task.request.purpose != ImagePurpose::FullResolution)
                {
                    FileByteCache::Instance().EraseIfUnshared(task.request.source);
                }
                if (released)
                {
                    m_queueCondition.notify_all();
                }
            }

            setBackgroundTask(false);
        }

        slot->Publish(Diagnostics::Phase::Exited, 0);

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

    void DecodeScheduler::LaneReaderThread(IoLane kind, ThreadSlot* slot)
    {
        BindLeaseToCurrentThread(*slot->lease);

        // The read runs in background mode (low CPU and I/O priority); the thread
        // waits and schedules outside it, at normal priority, so it wakes for
        // shutdown or a new task without being starved by normal-priority load.
        //
        // The thread selects its next lane in the same critical section that
        // finishes the previous read, so it continues a lane it just served
        // when that lane is still the best choice (and reads of one lane stay
        // on one thread while it keeps up).

        // A canceled or stopped request, destroyed outside the queue mutex
        // (its callback may own host objects).
        DecodeTask discard;
        std::shared_ptr<const std::vector<uint8_t>> discardBytes;
        std::wstring discardPath;
        bool discardPending = false;
        const auto flushDiscard = [&]
        {
            discardBytes.reset();
            if (!discardPath.empty())
            {
                FileByteCache::Instance().EraseIfUnshared(discardPath);
                discardPath.clear();
            }
            discard = DecodeTask {};
            discardPending = false;
        };

        const size_t laneIndex = LaneIndex(kind);
        const bool deviceAdmitted = (kind != IoLane::Remote);

        std::unique_lock<std::mutex> lock(m_queueMutex);
        for (;;)
        {
            // Queued work is dropped at shutdown (Shutdown canceled it).
            if (m_shutdown)
            {
                break;
            }

            Lane* lane = SelectThumbnailLane_NoLock(kind);
            if (!lane)
            {
                if (discardPending)
                {
                    lock.unlock();
                    flushDiscard();
                    lock.lock();
                    continue;
                }
                slot->Publish(Diagnostics::Phase::Idle, 0);
                m_queueCondition.wait(lock);
                continue;
            }

            DecodeTask task = lane->queue.Pop();
            const std::wstring laneKey = lane->key;
            if (!task.state || task.state->canceled.load())
            {
                // Cancel removes queued tasks, so this is a task of a Shutdown.
                Finish_NoLock(task);
                ReclaimLane_NoLock(laneKey);
                lock.unlock();
                task = DecodeTask {};
                lock.lock();
                continue;
            }

            // Admitted: the lane reads, and the read counts against the global
            // prefetch cap, the lane kind and the device until it is finished.
            const std::wstring device = lane->device;
            lane->reading = true;
            lane->lastServed = ++m_serviceClock;
            ++m_prefetchReads;
            ++m_readsInProgress[laneIndex];
            if (deviceAdmitted)
            {
                ++m_readsByDevice[device];
            }
            task.state->readLease = slot->lease;
            lock.unlock();
            if (discardPending)
            {
                flushDiscard();
            }

            // Read the file bytes. The shared cache only de-duplicates reads for
            // same-path tasks; the bytes are attached to this task, which owns them
            // from here on. A canceled read is never cached, so a same-path request
            // behind it reads its own bytes.
            std::shared_ptr<const std::vector<uint8_t>> bytes = FileByteCache::Instance().Get(task.request.source);
            bool stopped = false;
            if (!bytes)
            {
                slot->Publish(Diagnostics::Phase::Reading, task.handle);
                const bool backgroundMode = m_backgroundWorkPriority.load(std::memory_order_relaxed) ==
                    BackgroundWorkPriority::BackgroundMode;
                if (backgroundMode)
                {
                    slot->backgroundMode.store(true, std::memory_order_relaxed);
                    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                }
                auto read = LeasedRead(*slot->lease, task.state, task.handle, task.request.source, stopped);
                if (backgroundMode)
                {
                    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                    slot->backgroundMode.store(false, std::memory_order_relaxed);
                }
                if (read && !task.state->canceled.load())
                {
                    FileByteCache::Instance().Put(task.request.source, read);
                    bytes = std::move(read);
                }
            }

            lock.lock();
            // The lane cannot have been reclaimed while it was reading.
            Lane& done = *m_lanes.at(laneKey);
            done.reading = false;
            --m_prefetchReads;
            --m_readsInProgress[laneIndex];
            if (deviceAdmitted)
            {
                auto it = m_readsByDevice.find(device);
                if (it != m_readsByDevice.end() && --it->second == 0)
                {
                    m_readsByDevice.erase(it);
                }
            }

            // Schedule CPU decode work unless canceled. After a failed Local read
            // the decoder falls back to the file path; a failed Archive/Remote
            // read is delivered as a failure (slowReadFailed).
            if (stopped || task.state->canceled.load())
            {
                // Canceled while its bytes were being read (it was in no queue).
                Finish_NoLock(task);
                discardPath = task.request.source;
                discardBytes = std::move(bytes);
                discard = std::move(task);
                discardPending = true;
            }
            else
            {
                if ((!bytes || bytes->empty()) && kind != IoLane::Local)
                {
                    task.state->slowReadFailed = true;
                }
                if (bytes)
                {
                    task.state->prefetched = std::move(bytes);
                    task.state->holdsPrefetchSlot = true;
                    task.state->prefetchPath = task.request.source;
                    ++m_prefetchedTotal;
                    ++done.held;
                }
                m_backgroundQueue.Push(std::move(task), DecodeRequestState::Stage::Decode);
            }
            ReclaimLane_NoLock(laneKey);
            // A decode is queued, or capacity was freed for the other readers.
            m_queueCondition.notify_all();
        }
        lock.unlock();
        flushDiscard();

        slot->Publish(Diagnostics::Phase::Exited, 0);
        SignalExit(*slot);
    }

    void DecodeScheduler::MainReaderThread(ThreadSlot* slot)
    {
        BindLeaseToCurrentThread(*slot->lease);

        // Main-image reads are user-interactive: above normal, like the
        // full-resolution worker, for the thread's whole life.
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        DecodeTask discard;  // destroyed outside the queue mutex
        bool discardPending = false;

        std::unique_lock<std::mutex> lock(m_queueMutex);
        for (;;)
        {
            if (m_shutdown)
            {
                break;
            }

            Lane* lane = SelectMainLane_NoLock();
            if (!lane)
            {
                if (discardPending)
                {
                    lock.unlock();
                    discard = DecodeTask {};
                    discardPending = false;
                    lock.lock();
                    continue;
                }
                slot->Publish(Diagnostics::Phase::Idle, 0);
                m_queueCondition.wait(lock);
                continue;
            }

            DecodeTask task = lane->mainQueue.Pop();
            const std::wstring laneKey = lane->key;
            if (!task.state || task.state->canceled.load())
            {
                // Cancel removes queued tasks, so this is a task of a Shutdown.
                Finish_NoLock(task);
                ReclaimLane_NoLock(laneKey);
                lock.unlock();
                task = DecodeTask {};
                lock.lock();
                continue;
            }

            lane->mainReading = true;
            ++m_mainReads;
            task.state->readLease = slot->lease;
            lock.unlock();
            if (discardPending)
            {
                discard = DecodeTask {};
                discardPending = false;
            }

            bool stopped = false;
            slot->Publish(Diagnostics::Phase::Reading, task.handle);
            auto bytes = LeasedRead(*slot->lease, task.state, task.handle, task.request.source, stopped);
            if (!stopped && (!bytes || bytes->empty()))
            {
                IC_LOG_WARN("[DecodeScheduler] Full-res lane read returned empty bytes.");
            }

            lock.lock();
            m_lanes.at(laneKey)->mainReading = false;
            --m_mainReads;
            if (stopped || task.state->canceled.load())
            {
                Finish_NoLock(task);
                discard = std::move(task);
                discardPending = true;
            }
            else
            {
                if (bytes && !bytes->empty())
                {
                    task.state->prefetched = std::move(bytes);
                    task.state->holdsMainInput = true;
                    ++m_mainHeld;
                }
                else
                {
                    // Main-image readers serve Archive/Remote lanes only.
                    task.state->slowReadFailed = true;
                }
                m_highQueue.push(std::move(task));
            }
            ReclaimLane_NoLock(laneKey);
            m_queueCondition.notify_all();
        }
        lock.unlock();
        discard = DecodeTask {};

        slot->Publish(Diagnostics::Phase::Exited, 0);
        SignalExit(*slot);
    }
}

