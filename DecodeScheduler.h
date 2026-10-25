#pragma once

#include "ImageRequest.h"
#include "DecodeTypes.h"
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

        // Thumbnail/preview bytes read by the prefetch stage. Owned by this request,
        // so no other request (canceled, finished, or same-path) can take them away.
        std::shared_ptr<const std::vector<uint8_t>> prefetched;
        bool holdsPrefetchSlot { false };  // counted for prefetch backpressure
        bool decoding { false };           // taken by a worker
        std::wstring prefetchVolume;
        std::wstring prefetchPath;

        // Scheduling (guarded by the queue mutex). The sequence orders requests
        // of one priority by age and survives promotion.
        RequestPriority priority { RequestPriority::Visible };
        uint64_t sequence { 0 };
        enum class Stage { None, Io, Decode } queuedIn { Stage::None };
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
        DecodeTask Pop();
        // Moves the queued task of `state` to `priority`; false if not queued here.
        bool Reprioritize(DecodeRequestState& state, RequestPriority priority);

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

    class DecodeScheduler
    {
    public:
        DecodeScheduler();
        ~DecodeScheduler();

        // Upper bound on decode workers: one dedicated full-resolution worker plus
        // up to four background (thumbnail) workers. Thumbnail I/O is serialized on a
        // separate prefetch thread, so more decode workers only add CPU contention.
        static constexpr size_t kMaxDecodeWorkers = 5;

        // Set the number of decode worker threads (the serialized thumbnail prefetch
        // thread is separate). The count is clamped to [1, kMaxDecodeWorkers];
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
        // by every worker. Thumbnail/preview requests pass the serialized I/O
        // stage and then the decode stage; both take the highest priority first
        // (then the oldest), and a higher-priority request also takes over the
        // next free prefetch slot from a lower-priority one waiting for it.
        bool Enqueue(DecodeTask task);

        // Changes the priority of a queued request, O(log n): promotion moves it
        // ahead of lower-priority work in whichever stage it is waiting, and a
        // request between stages keeps the new priority for the next one. A
        // request already decoding, finished, canceled or unknown is a no-op, so
        // a recycled tile can promote or demote a stale handle safely.
        void SetPriority(uint64_t handle, RequestPriority priority);

        // Best-effort cancel, O(1). A queued task releases its prefetched bytes
        // and prefetch slot at once and is dropped without a callback when a
        // thread reaches it. A byte-source read in progress (thumbnail prefetch
        // or full-resolution archive read) is asked to stop through the
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
        // Enqueue fails afterwards.
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

        // Tests/diagnostics: volumes with recorded prefetch tuning.
        size_t VolumeProfileCount() const;

        // Tests/diagnostics: tasks currently holding prefetched bytes (the
        // quantity bounded by prefetch backpressure).
        size_t PrefetchedTaskCount() const;

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
        };
        using TestHook = std::function<void(TestPoint point, uint64_t handle)>;

        // Installs the hook. Set it before the first Enqueue (it is read without
        // synchronization by the scheduler threads).
        void SetTestHook(TestHook hook);

    private:
        enum class WorkerKind
        {
            HighPriority,
            Background,
            Shared      // sole worker: high queue first, then background queue
        };

        // One scheduler thread (decode worker or prefetch thread) and its read
        // lease. Defined in DecodeScheduler.cpp.
        struct ThreadSlot;

        void WorkerThread(WorkerKind kind, ThreadSlot* slot);
        void ThumbPrefetchThread(ThreadSlot* slot);

        // Last step of every scheduler thread: marks `slot` exited for Shutdown.
        void SignalExit(ThreadSlot& slot);

        // Shared by both Shutdown overloads and SetThreadCount; requires
        // m_lifecycleMutex. Without a deadline it waits until every thread exits.
        bool StopAndJoin(std::optional<std::chrono::steady_clock::time_point> deadline);

        // OS-cancels every read registered on a lease for a canceled request.
        // Runs no host code. Requires m_lifecycleMutex (it walks m_threads).
        void CancelActiveReadsIo();

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

        // Gives back the request's prefetch slot (backpressure count) and drops
        // its bytes. Returns true if a slot was released; the caller then notifies
        // m_queueCondition after unlocking. Requires m_queueMutex.
        bool ReleasePrefetch_NoLock(DecodeRequestState& state);

        // Cached per-volume seek-penalty query. Called without m_queueMutex: the
        // storage query opens the volume device and can block.
        size_t PrefetchDepthForPath(const std::wstring& path);

        // Decode workers and the prefetch thread. Started and joined only by
        // SetThreadCount and Shutdown, under m_lifecycleMutex (which also guards
        // m_joinPending: a bounded Shutdown left threads unjoined).
        std::mutex m_lifecycleMutex;
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

        std::queue<DecodeTask> m_highQueue;
        PriorityTaskQueue m_thumbIoQueue;     // serialized disk reads (thumbnail/preview)
        PriorityTaskQueue m_backgroundQueue;  // CPU decode/resize work (from prefetched bytes)
        uint64_t m_nextSequence { 0 };        // guarded by m_queueMutex
        mutable std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        std::atomic<bool> m_shutdown;

        size_t m_threadCount;

        struct VolumeIoProfile
        {
            bool incursSeek { true };
            size_t prefetchDepth { 2 };
        };

        // Guarded by m_queueMutex.
        std::unordered_map<std::wstring, VolumeIoProfile> m_volumeProfiles {};
        size_t m_globalPrefetchCap { 4 };

        // Tasks holding prefetched bytes, in total and per volume (guarded by
        // m_queueMutex). The prefetch stage waits while these are at their caps.
        size_t m_prefetchedTotal { 0 };
        std::unordered_map<std::wstring, size_t> m_prefetchedByVolume {};

        // Queued or decoding requests by handle (guarded by m_queueMutex).
        std::unordered_map<uint64_t, std::shared_ptr<DecodeRequestState>> m_liveRequests {};

        // Seek-penalty results by volume (guarded by m_seekPenaltyMutex).
        mutable std::mutex m_seekPenaltyMutex;
        std::unordered_map<std::wstring, bool> m_seekPenaltyByVolume {};
    };
}
