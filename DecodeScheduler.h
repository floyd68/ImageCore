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
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>

namespace ImageCore
{
    // Per-request state, owned by the task (and by the scheduler's live-request
    // table while the task is queued or decoding). The prefetch fields are guarded
    // by the scheduler's queue mutex.
    struct DecodeRequestState
    {
        std::atomic<bool> canceled { false };

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
        // before requests are issued.
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
        // thread reaches it; a
        // decode in progress cannot be interrupted, but its callback is suppressed
        // if the cancel is observed before delivery starts. A callback that has
        // already been selected for delivery can still run concurrently with, or
        // after, Cancel returning, so callers must validate results themselves
        // (for example with a generation). Canceling an unknown or finished handle
        // is a no-op and leaves no state behind.
        void Cancel(uint64_t handle);

        // Idempotent. Stops and joins all threads; queued tasks are dropped
        // without callbacks. Clears volume tuning and FileByteCache so a later
        // SetThreadCount starts from a clean state. Enqueue fails afterwards.
        void Shutdown();

        // Tests/diagnostics: requests currently queued or decoding (cancellation
        // state is only held for these).
        size_t LiveRequestCount() const;

        // Tests/diagnostics: volumes with recorded prefetch tuning.
        size_t VolumeProfileCount() const;

        // Tests/diagnostics: tasks currently holding prefetched bytes (the
        // quantity bounded by prefetch backpressure).
        size_t PrefetchedTaskCount() const;

    private:
        enum class WorkerKind
        {
            HighPriority,
            Background,
            Shared      // sole worker: high queue first, then background queue
        };

        void WorkerThread(WorkerKind kind);
        void ThumbPrefetchThread();

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

        std::vector<std::thread> m_highWorkers;
        std::vector<std::thread> m_backgroundWorkers;
        std::thread m_thumbPrefetchWorker;

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
