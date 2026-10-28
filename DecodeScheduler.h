#pragma once

#include "ImageRequest.h"
#include "DecodeTypes.h"
#include "IPathByteSource.h"
#include <thread>
#include <vector>
#include <queue>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <unordered_map>

namespace ImageCore
{
    // Read lease of one scheduler thread (defined in DecodeScheduler.cpp): which
    // request's byte-source read runs on that thread right now. OS cancellation
    // (CancelSynchronousIo) for a request is issued through the lease, under its
    // mutex, and only while that request's read is registered on it.
    struct ReadLease;

    // Per-request state, owned by the task (and by the scheduler's live-request
    // table while the task is queued or decoding). The prefetch fields are guarded
    // by the scheduler's queue mutex.
    struct DecodeRequestState
    {
        std::atomic<bool> canceled { false };
        uint64_t handle { 0 };

        // Request-owned I/O cancellation: Cancel and Shutdown have stop requested
        // on it by the scheduler's stop-dispatch thread. Its token is passed to
        // the byte source.
        std::stop_source stop;

        // Lease of the thread that took this request (guarded by the queue
        // mutex; set once, when a prefetch or decode thread takes the task).
        std::shared_ptr<ReadLease> readLease;

        // Bytes read by an I/O lane: thumbnail/preview bytes, or the input of a
        // full-resolution request read by a main-image reader. Owned by this
        // request, so no other request (canceled, finished, or same-path) can
        // take them away.
        std::shared_ptr<const std::vector<uint8_t>> prefetched;
        bool holdsPrefetchSlot { false };  // thumbnail bytes, counted for prefetch backpressure
        bool holdsMainInput { false };     // main-image input, counted against kMainImageInputCap
        bool decoding { false };           // taken by a worker
        // An Archive/Remote lane read returned no bytes: the request is
        // delivered as a failure without decoding, since the decoders would
        // reopen the path on a decode worker (outside the lanes).
        bool slowReadFailed { false };
        std::wstring laneKey;              // I/O lane (empty: a local full-resolution request)
        std::wstring prefetchPath;

        // Scheduling (guarded by the queue mutex). The sequence orders requests
        // of one priority by age and survives promotion.
        RequestPriority priority { RequestPriority::Visible };
        uint64_t sequence { 0 };
        // Io: a lane's thumbnail queue; MainIo: a lane's main-image queue.
        enum class Stage { None, Io, MainIo, Decode } queuedIn { Stage::None };
    };

    // Decode task
    struct DecodeTask
    {
        ImageRequest request;
        std::function<void(PipelineResult&&)> callback;  // Callback that receives the DecodedImage result (move semantics)
        uint64_t handle;
        RequestPriority priority { RequestPriority::Visible };  // initial priority
        std::shared_ptr<DecodeRequestState> state;       // set by Enqueue

        DecodeTask()
            : handle(0)
        {
        }
    };

    // Thumbnail/preview queue ordered by priority, then age. Promotion re-keys a
    // queued task in O(log n). Canceled tasks stay until popped (see Cancel).
    // Not thread-safe: the scheduler guards it with its queue mutex.
    class PriorityTaskQueue
    {
    public:
        // Inserts `task` by its state's priority and sequence; records `stage`.
        void Push(DecodeTask task, DecodeRequestState::Stage stage);
        bool Empty() const { return m_tasks.empty(); }
        size_t Size() const { return m_tasks.size(); }
        RequestPriority TopPriority() const;
        uint64_t TopSequence() const;
        DecodeTask Pop();
        // Moves the queued task of `state` to `priority`; false if not queued here.
        bool Reprioritize(DecodeRequestState& state, RequestPriority priority);
        // Removes the queued task of `state` into `out`; false if not queued here.
        bool Remove(DecodeRequestState& state, DecodeTask& out);

    private:
        struct Key
        {
            uint8_t rank { 0 };      // 255 - priority: highest priority first
            uint64_t sequence { 0 };
            bool operator<(const Key& other) const
            {
                return rank != other.rank ? rank < other.rank : sequence < other.sequence;
            }
        };
        static Key KeyOf(RequestPriority priority, uint64_t sequence)
        {
            return Key { static_cast<uint8_t>(255 - static_cast<uint8_t>(priority)), sequence };
        }

        std::map<Key, DecodeTask> m_tasks;
    };

    // Slow-source I/O lanes (floyd68/ImageCore#15)
    // =============================================
    //
    // Byte-source reads (thumbnail/preview prefetch, and full-resolution reads
    // of archive entries and remote files) run on I/O reader threads, never on
    // a decode worker, and are scheduled in lanes so that a slow read that is
    // still wanted (a deep entry of a solid 7z, a network or cloud file, a
    // first open held up by on-access scanning) only delays work behind it in
    // its own lane.
    //
    // Logical lanes. The host classifies each path once at Enqueue
    // (IPathByteSource::ClassifyPath; IoLaneFor maps the kind to a lane):
    // Local (also Unknown, the conservative default), Archive and Remote
    // (network paths and cloud placeholders). A lane queue is keyed by
    //   Local:   the physical device of the file;
    //   Archive: the physical device of the host archive file, and on a device
    //            without a seek penalty also the archive itself (two archives
    //            on one SSD are separate lanes, on one HDD they share one);
    //   Remote:  the server of a UNC path, or the drive letter (no local
    //            device query is ever made for a Remote path).
    // Each lane reads at most one thumbnail/preview at a time (in priority
    // order, then age), so reads of one device stay sequential within a lane.
    //
    // Physical-device admission. A device is identified from a volume
    // (IOCTL_STORAGE_GET_DEVICE_NUMBER on "\\.\X:", so several volume letters
    // on one disk are one device; a volume the query cannot place is its own
    // device). Local and Archive thumbnail reads are admitted per device: at
    // most kMaxReadsPerSeekDevice on a device with a seek penalty (HDD, or
    // unknown) and kMaxReadsPerSolidStateDevice otherwise. Remote reads are
    // not admitted against a local device: they wait on the network.
    //
    // Same-HDD tradeoff (chosen: a bounded override, not strict
    // serialization). Strict one-reader admission per HDD cannot also let a
    // local read progress past a blocked archive read on that disk. A device
    // with a seek penalty therefore admits two reads, one per logical lane
    // (Local and Archive): a slow archive extraction (mostly decompression)
    // and local thumbnails on the same HDD both progress, at the cost of at
    // most two interleaved read streams on that disk. Two local folders of one
    // HDD (even on different volume letters) still share one serialized lane,
    // as do two archives on one HDD. Strict per-device serialization is not
    // promised.
    //
    // Readers and bounds (process-wide per scheduler; ImageLoader owns one):
    // - kReadersPerLane reader threads per logical lane (Local, Archive,
    //   Remote), started with the scheduler. A lane kind can use only its own
    //   readers, so blocked Archive or Remote reads never take a reader from
    //   another lane kind (static reservation); with more active lanes of one
    //   kind than its readers, the excess lanes wait (all lanes busy).
    // - Fairness among lanes of one kind: a free reader takes the admissible
    //   lane whose next request has the highest priority; on a tie, the lane
    //   served least recently (a newly active lane counts as never served),
    //   then the oldest request. So a newly active source is served by the
    //   next free reader of its kind instead of after the backlog of busier
    //   lanes.
    // - Prefetch memory is bounded by counts, not bytes: thumbnail/preview
    //   tasks holding read bytes plus thumbnail reads in progress never exceed
    //   the global prefetch cap (at most 8: the sum of the per-device depths,
    //   2 on a seek-penalty device and 4 otherwise), and each lane holds at
    //   most its device's depth. When one prefetch slot is left, a reader does
    //   not take it for lower-priority work while an idle reader of another
    //   lane kind has higher-priority work waiting (#11 across lanes).
    //   FileByteCache is capped at the same count. Each read is at most 1 GiB
    //   (ReadWholeFileBytes), so the worst case is (cap + main-image inputs) x
    //   1 GiB; there is no aggregate byte budget. The host's own budgets add to
    //   this one (FICture2's ArchiveEntryHandoff documents the composition).
    // - Lanes are created on demand and reclaimed as soon as one has no queued
    //   request, no read in progress and no bytes held. Reader threads are a
    //   fixed pool (3 x kReadersPerLane + kMainImageReaders).
    //
    // Main images. Full-resolution requests of Local paths are queued for the
    // decode workers directly (the decoder reads the file, as before).
    // Archive and Remote ones are read first by one of kMainImageReaders
    // main-image reader threads, at above-normal priority, and then queued for
    // the decode workers with their bytes; so a slow main-image read never
    // occupies a decode worker (with SetThreadCount(1) the only worker keeps
    // decoding thumbnails and local images meanwhile). Each lane also has a
    // main-image queue that runs beside its thumbnail reads (a main image never
    // waits for a thumbnail read of its lane, as before #15), one main-image
    // read per lane at a time; the main-image lane of an archive entry is
    // always the archive itself (also on an HDD), since main-image reads are
    // not admitted against the device (as before #15). Main-image reads in
    // progress plus inputs held for decoding are capped at kMainImageInputCap.
    //
    // Failed slow reads. An Archive or Remote read (thumbnail or main image)
    // that returns no bytes without being stopped is delivered as
    // HRESULT_FROM_WIN32(ERROR_READ_FAULT) without decoding: the decoders'
    // path fallback would reopen the source on a decode worker, outside the
    // lanes. A failed Local read still falls back to the path, as before.
    //
    // Cancellation and admission waits. A request waits for lane, device and
    // slot capacity in its lane queue, holding nothing; Cancel removes it from
    // that queue at once (no callback), which can reclaim the lane. A read in
    // progress is stopped as before (#14 stop token, read lease and
    // CancelSynchronousIo on the reader thread). Capacity is only ever held by
    // a read in progress or by bytes waiting for a decode worker, and decode
    // workers never wait for I/O, so neither cancellation nor main-image work
    // can deadlock behind an admission wait.
    //
    // Priority policy for the readers: a thumbnail/preview read enters
    // background mode (THREAD_MODE_BACKGROUND_BEGIN) only around the read
    // itself, as before #15 (SetBackgroundWorkPriority selects Normal for
    // tests); readers wait at normal priority. Main-image readers run above
    // normal. Rationale: lanes confine a starved or scan-delayed background
    // read to its own lane, and raising only the read priority would not make
    // thumbnails progress under all-core normal-priority load, because their
    // decode also runs in background mode; changing both is a separate policy
    // decision (see README "Priority").
    //
    // Topology cache: the device and seek-penalty answer of each volume is
    // cached for kTopologyRefreshInterval, then queried again by the next
    // Enqueue that needs it (outside the queue mutex); a restart clears it.
    class DecodeScheduler
    {
    public:
        DecodeScheduler();
        ~DecodeScheduler();

        // Upper bound on decode workers: one dedicated full-resolution worker plus
        // up to four background (thumbnail) workers. Byte-source I/O runs on the
        // separate lane readers, so more decode workers only add CPU contention.
        static constexpr size_t kMaxDecodeWorkers = 5;

        // I/O lanes (see the design note above).
        static constexpr size_t kReadersPerLane = 2;          // per logical lane
        static constexpr size_t kMainImageReaders = 2;        // Archive/Remote full-resolution reads
        static constexpr size_t kMainImageInputCap = 2;       // main-image reads + inputs held
        static constexpr size_t kMaxReadsPerSeekDevice = 2;   // thumbnail reads per HDD
        static constexpr size_t kMaxReadsPerSolidStateDevice = 3;
        static constexpr size_t kMaxPrefetchCap = 8;          // global prefetch cap
        static constexpr std::chrono::seconds kTopologyRefreshInterval { 60 };

        // Set the number of decode worker threads (the I/O lane readers are
        // separate). The count is clamped to [1, kMaxDecodeWorkers];
        // WorkerCount() reports the number actually started. With 1, a single shared
        // worker serves full-resolution work first and background work when no
        // full-resolution work is queued; with 2 or more, one worker is dedicated to
        // full-resolution work and the rest serve background work.
        //
        // Restart semantics: the running scheduler is shut down first (queued work
        // is dropped without callbacks, see Shutdown), then started fresh with no
        // volume tuning and an empty FileByteCache. Intended for configuration
        // before requests are issued. While a bounded Shutdown has left threads
        // unjoined (it returned false, and no later call has joined them), this
        // returns at once without restarting, logs a warning and leaves the
        // threads and WorkerCount() untouched.
        void SetThreadCount(size_t count);

        // Default decode worker count for a machine: hardware threads - 1, minimum 1.
        static size_t DefaultWorkerCount(unsigned hardwareThreads);

        // Number of running decode worker threads (excludes the prefetch thread).
        size_t WorkerCount() const;

        // Add to the task queue at `task.priority`. Returns false (and never
        // invokes the callback) once the scheduler has been shut down.
        //
        // Priority: full-resolution requests have their own queue, served first
        // by every worker (Archive/Remote ones are read by a main-image reader
        // first). Thumbnail/preview requests pass their I/O lane and then the
        // decode stage; both take the highest priority first (then the oldest),
        // and a higher-priority request also takes the next free prefetch slot
        // before a lower-priority one waiting for it.
        //
        // Lane selection calls the byte source's ClassifyPath and
        // ResolveHostPath, and on the first request of a volume (and after
        // kTopologyRefreshInterval) a volume device query; all of it runs on the
        // calling thread before the queue mutex is taken.
        bool Enqueue(DecodeTask task);

        // Changes the priority of a queued request, O(log n): promotion moves it
        // ahead of lower-priority work in whichever stage it is waiting, and a
        // request between stages keeps the new priority for the next one. A
        // request already decoding, finished, canceled or unknown is a no-op, so
        // a recycled tile can promote or demote a stale handle safely.
        void SetPriority(uint64_t handle, RequestPriority priority);

        // Best-effort cancel, O(log n). A task waiting in an I/O lane is removed
        // at once (no callback). A task waiting for decode releases its read
        // bytes and prefetch slot at once and is dropped without a callback when
        // a worker reaches it. A byte-source read in progress (thumbnail prefetch
        // or full-resolution archive/remote read) is asked to stop through the
        // request's stop token, and CancelSynchronousIo is tried on the reading
        // thread (best effort); its result is discarded, never cached, and the
        // request never reaches the decode stage (so the decoders' own path
        // fallback cannot re-read the file). A request canceled before its decode
        // starts is not decoded. A decode in progress cannot be interrupted, but
        // its callback is suppressed if the cancel is observed before delivery
        // starts. Cancel itself sets only the canceled flag (enough for every
        // check above); the token is signaled, and CancelSynchronousIo tried,
        // shortly after on the scheduler's stop-dispatch thread, with no
        // scheduler lock held. So a stop callback never runs on, or blocks, the
        // caller, and it may call back into the scheduler. A callback that has
        // already been selected for delivery can still run concurrently with, or
        // after, Cancel returning, so callers must validate results themselves
        // (for example with a generation). Canceling an unknown or finished handle
        // is a no-op and leaves no state behind.
        void Cancel(uint64_t handle);

        // Idempotent. Rejects new requests, cancels every live request (stop
        // token, signaled on the stop-dispatch thread, plus best-effort OS
        // cancellation of reads in progress), then stops and joins all threads,
        // the stop-dispatch thread included; queued and in-flight
        // requests are dropped without callbacks. Clears volume tuning and
        // FileByteCache so a later SetThreadCount starts from a clean state.
        // Enqueue fails afterwards. The threads are raised above normal priority
        // for their exit, out of background mode's starvation-prone priority, so
        // a CPU-saturated machine does not stretch the join to seconds.
        //
        // The join has no bound: a byte-source read that honours neither its
        // token nor OS cancellation (or a decode or stop callback that never
        // returns) keeps this waiting. Threads are never detached.
        void Shutdown();

        // Bounded Shutdown: the same steps, but waits at most `bound` for the
        // threads to exit and joins only threads that have exited, so it never
        // blocks on a stuck one (a stuck read, decode callback or stop callback:
        // no host code runs on the calling thread). Returns true when every
        // thread was joined (the
        // state is then exactly as after Shutdown()). On false nothing is
        // detached: the scheduler keeps the unjoined threads and everything they
        // use, keeps rejecting requests and must not be destroyed while they run
        // (the destructor joins without a bound); a later Shutdown(bound) or
        // Shutdown() retries the join. Harmless before any request and when
        // repeated. With a legacy (ReadAll-only) byte source a blocked read is not
        // interrupted, so false is expected until that read returns. Stop
        // callbacks (on the stop-dispatch thread, no scheduler lock held) may use
        // the other scheduler calls but not Shutdown or SetThreadCount.
        bool Shutdown(std::chrono::milliseconds bound);

        // Tests/diagnostics: requests currently queued or decoding (cancellation
        // state is only held for these).
        size_t LiveRequestCount() const;

        // Tests/diagnostics: devices with recorded prefetch tuning.
        size_t VolumeProfileCount() const;

        // Tests/diagnostics: tasks currently holding prefetched bytes (the
        // quantity bounded by prefetch backpressure).
        size_t PrefetchedTaskCount() const;

        // Tests/diagnostics: a point-in-time view of the scheduler, for reports
        // when a progress wait times out. Each scheduler thread publishes its
        // phase and the handle it works on with relaxed atomic stores; this
        // reads them together with the queue state and asks the OS for each
        // thread's priority and CPU time. Not for use on a hot path: it takes the
        // queue mutex, and tries the lifecycle mutex briefly (while a restart or
        // Shutdown holds it, `threadsListed` is false and `threads` is empty).
        struct Diagnostics
        {
            enum class Role : uint8_t
            {
                HighPriority,   // full-resolution worker
                Background,     // thumbnail decode worker (also steals full-resolution work)
                Shared,         // sole worker: full-resolution first, then thumbnails
                Prefetch,       // thumbnail/preview I/O lane reader (see `lane`)
                StopDispatch,   // runs stop requests for Cancel and Shutdown
                MainReader,     // full-resolution Archive/Remote reader
            };
            enum class Phase : uint8_t
            {
                Starting,       // thread started, not yet waiting for work
                Idle,           // waiting for work (a reader: no admissible lane)
                Taken,          // worker: task taken, entering its priority mode
                Reading,        // byte-source read in progress
                Decoding,       // decode stage in progress
                Delivering,     // result callback running
                Stopping,       // stop-dispatch: running a stop request
                Exited,         // left its loop
            };
            struct Thread
            {
                Role role { Role::Background };
                IoLane lane { IoLane::Local };  // Prefetch readers: the lane kind served
                Phase phase { Phase::Starting };
                uint64_t handle { 0 };        // request in progress (0: none)
                bool backgroundMode { false };// entering, in or leaving background mode
                unsigned long threadId { 0 };
                int priority { 0 };           // GetThreadPriority (background mode reads low)
                double cpuMs { 0.0 };         // kernel + user time so far
            };

            bool shutdown { false };
            size_t highQueued { 0 };        // full-resolution queue
            size_t ioQueued { 0 };          // thumbnail/preview requests in lane queues
            size_t mainIoQueued { 0 };      // full-resolution requests in lane queues
            size_t decodeQueued { 0 };      // thumbnail decode queue
            size_t liveRequests { 0 };      // queued or in progress
            size_t prefetchedTasks { 0 };   // admitted: holding prefetched bytes
            size_t prefetchReads { 0 };     // thumbnail reads in progress (hold a slot too)
            size_t prefetchCap { 0 };       // global prefetch cap
            size_t mainInputs { 0 };        // main-image reads in progress + inputs held
            size_t lanes { 0 };             // lanes alive (reclaimed when idle)
            bool threadsListed { false };
            std::vector<Thread> threads;
        };
        Diagnostics GetDiagnostics() const;

        // Test-only seam: points in a request's life at which the hook runs, on
        // the thread named. No scheduler lock is held, except CancelDispatch,
        // which runs under the reading thread's read-lease mutex (so the worker
        // cannot retire that read or start another one until the hook returns).
        // CancelDispatch runs on the stop-dispatch thread for Cancel, and also on
        // the thread calling Shutdown.
        enum class TestPoint
        {
            TaskTaken,          // decode worker, after taking a task, before any read or decode
            BeforeReadRegister, // reading thread, before registering a read on its lease
            BeforeReadRetire,   // reading thread, after the read returned, before retiring it
            CancelDispatch,     // just before CancelSynchronousIo (see above)
            DecodeStart,        // decode worker, just before the decode stage runs
            StopDispatched,     // stop-dispatch thread, after a request's stop and OS cancel attempt
            CancelPublished,    // canceling thread, after Cancel removed the request and queued its stop
            StopDispatchExit,   // stop-dispatch thread, queue drained after shutdown, just before it exits (handle 0)
        };
        using TestHook = std::function<void(TestPoint point, uint64_t handle)>;

        // Installs the hook. Set it before the first Enqueue (it is read without
        // synchronization by the scheduler threads).
        void SetTestHook(TestHook hook);

        // How thumbnail/preview work (the prefetch read and the background
        // decode) runs. BackgroundMode, the default and the only mode used in
        // production, enters THREAD_MODE_BACKGROUND_BEGIN (low CPU and I/O
        // priority). Under normal-priority load on every CPU such a thread runs
        // only when the OS starvation boost picks it, every few seconds, so its
        // progress has no useful bound (see floyd68/ImageCore#15). Normal runs
        // that work at THREAD_PRIORITY_NORMAL with normal I/O priority.
        enum class BackgroundWorkPriority
        {
            BackgroundMode,
            Normal,
        };

        // Test seam: tests of scheduling logic (ordering, backpressure,
        // cancellation) select Normal so that their progress deadlines do not
        // depend on the absence of other load on the machine; tests of the
        // priority policy keep BackgroundMode. Each thread reads it when it
        // starts a task, so it may be called at any time.
        void SetBackgroundWorkPriority(BackgroundWorkPriority priority);

        // Physical placement of a volume ("C:"): `device` identifies the disk
        // (volumes with equal ids share admission), `incursSeekPenalty` selects
        // the HDD admission bound and prefetch depth.
        struct VolumeTopology
        {
            std::wstring device;
            bool incursSeekPenalty { true };
        };
        using VolumeTopologyQuery = std::function<VolumeTopology(const std::wstring& volume)>;

        // Test seam: replaces the volume device query (an empty function
        // restores it). Called from Enqueue, outside the queue mutex, with the
        // volume of the file (or of the host archive); never for Remote paths
        // or paths without a drive letter. Set it before the first Enqueue.
        void SetVolumeTopologyQuery(VolumeTopologyQuery query);

    private:
        enum class WorkerKind
        {
            HighPriority,
            Background,
            Shared      // sole worker: high queue first, then background queue
        };

        // One scheduler thread (decode worker or reader) and its read lease.
        // Defined in DecodeScheduler.cpp.
        struct ThreadSlot;

        // One I/O lane queue (see the design note). Defined in DecodeScheduler.cpp.
        struct Lane;

        // Where a request's bytes are read: its lane key and the lane's
        // admission parameters. Computed outside the queue mutex.
        struct Route
        {
            IoLane lane { IoLane::Local };
            std::wstring key;
            std::wstring device;
            size_t depth { 2 };
            size_t deviceLimit { kMaxReadsPerSeekDevice };
        };

        void WorkerThread(WorkerKind kind, ThreadSlot* slot);
        void LaneReaderThread(IoLane lane, ThreadSlot* slot);
        void MainReaderThread(ThreadSlot* slot);

        // Lane of `request` (host classification, host path, volume topology).
        // Called without m_queueMutex: the host and the device query can block.
        Route RouteFor(const ImageRequest& request, IoLane lane);
        VolumeTopology TopologyForVolume(const std::wstring& volume);

        // Lane selection for a free reader; null when none is admissible.
        // Require m_queueMutex.
        Lane* SelectThumbnailLane_NoLock(IoLane kind);
        Lane* SelectMainLane_NoLock();
        bool ThumbnailAdmissible_NoLock(const Lane& lane) const;
        // Erases `key`'s lane when it has nothing queued, in progress or held.
        void ReclaimLane_NoLock(const std::wstring& key);

        // Last step of every scheduler thread: marks `slot` exited for Shutdown.
        void SignalExit(ThreadSlot& slot);

        // Shared by both Shutdown overloads and SetThreadCount; requires
        // m_lifecycleMutex. Without a deadline it waits until every thread exits.
        bool StopAndJoin(std::optional<std::chrono::steady_clock::time_point> deadline);

        // OS-cancels every read registered on a lease for a canceled request.
        // Runs no host code. Requires m_lifecycleMutex (it walks m_threads).
        void CancelActiveReadsIo();

        // Raises every scheduler thread above normal priority through its
        // handle, overriding background mode, so a thread starved by other
        // processes still reaches its exit within a shutdown bound. Requires
        // m_lifecycleMutex (it walks m_threads).
        void RaiseThreadPriorities();

        void StopDispatchThread(ThreadSlot* slot);

        // Reads `path` for `state` through the host byte source with the
        // request's stop token, registered on `lease` for the duration of the
        // read. Returns null on failure, and also when stop was requested
        // (`stopped` is then true): a canceled read's bytes are never used.
        std::shared_ptr<std::vector<uint8_t>> LeasedRead(
            ReadLease& lease,
            const std::shared_ptr<DecodeRequestState>& state,
            uint64_t handle,
            const std::wstring& path,
            bool& stopped);

        // Signals stop on `state`'s token, then tries CancelSynchronousIo on the
        // thread whose lease has `state`'s read registered. Runs only on the
        // stop-dispatch thread, without any scheduler lock: stop callbacks run
        // synchronously in here.
        void StopRequest(const std::shared_ptr<DecodeRequestState>& state);

        void RunTestHook(TestPoint point, uint64_t handle) const;

        // Removes `task` from the live-request table (if it is still the entry
        // for its handle). Requires m_queueMutex.
        void Finish_NoLock(const DecodeTask& task);

        // Gives back the request's prefetch slot or main-image input (capacity
        // counts) and drops its bytes. Returns true if capacity was released;
        // the caller then notifies m_queueCondition. Requires m_queueMutex.
        bool ReleasePrefetch_NoLock(DecodeRequestState& state);

        // Decode workers and the I/O readers. Started and joined only by
        // SetThreadCount and Shutdown, under m_lifecycleMutex (which also guards
        // m_joinPending: a bounded Shutdown left threads unjoined).
        mutable std::mutex m_lifecycleMutex;
        std::vector<std::unique_ptr<ThreadSlot>> m_threads;
        bool m_joinPending { false };
        // ThreadSlot::exited is guarded by m_exitMutex; m_exitCondition is
        // notified as each thread exits.
        std::mutex m_exitMutex;
        std::condition_variable m_exitCondition;

        // Stop-dispatch queue (guarded by m_stopMutex). m_stopExit lets the
        // dispatch thread exit once the queue is drained. Lock order:
        // m_queueMutex, then m_stopMutex (Cancel publishes under both).
        std::mutex m_stopMutex;
        std::condition_variable m_stopCondition;
        std::deque<std::shared_ptr<DecodeRequestState>> m_stopQueue;
        bool m_stopExit { false };
        std::atomic<size_t> m_workerCount { 0 };  // decode workers in m_threads
        TestHook m_testHook;
        std::atomic<BackgroundWorkPriority> m_backgroundWorkPriority { BackgroundWorkPriority::BackgroundMode };

        std::queue<DecodeTask> m_highQueue;
        PriorityTaskQueue m_backgroundQueue;  // CPU decode/resize work (from prefetched bytes)
        uint64_t m_nextSequence { 0 };        // guarded by m_queueMutex
        mutable std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        std::atomic<bool> m_shutdown;

        size_t m_threadCount;

        // I/O lanes by key, and their capacity counts (guarded by m_queueMutex).
        std::map<std::wstring, std::unique_ptr<Lane>> m_lanes;
        std::unordered_map<std::wstring, size_t> m_readsByDevice {};  // thumbnail reads in progress
        size_t m_readsInProgress[3] {};   // thumbnail reads in progress per IoLane
        uint64_t m_serviceClock { 0 };    // fairness: lane service order

        // Prefetch depth per device (guarded by m_queueMutex); the global cap
        // is their sum, at most kMaxPrefetchCap.
        std::unordered_map<std::wstring, size_t> m_volumeProfiles {};
        size_t m_globalPrefetchCap { 4 };

        // Tasks holding prefetched thumbnail bytes and thumbnail reads in
        // progress (guarded by m_queueMutex); together at most the global cap.
        size_t m_prefetchedTotal { 0 };
        size_t m_prefetchReads { 0 };

        // Main-image reads in progress and inputs held (guarded by m_queueMutex).
        size_t m_mainReads { 0 };
        size_t m_mainHeld { 0 };

        // Queued or decoding requests by handle (guarded by m_queueMutex).
        std::unordered_map<uint64_t, std::shared_ptr<DecodeRequestState>> m_liveRequests {};

        // Volume topology by volume, with the time of the query (guarded by
        // m_topologyMutex), and the test seam replacing the query.
        struct CachedTopology
        {
            VolumeTopology topology;
            std::chrono::steady_clock::time_point queried;
        };
        mutable std::mutex m_topologyMutex;
        std::unordered_map<std::wstring, CachedTopology> m_topologyByVolume {};
        VolumeTopologyQuery m_topologyQuery;
    };
}
