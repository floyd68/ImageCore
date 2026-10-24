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
    };

    // Decode task
    struct DecodeTask
    {
        ImageRequest request;
        std::function<void(PipelineResult&&)> callback;  // Callback that receives the DecodedImage result (move semantics)
        uint64_t handle;
        std::shared_ptr<DecodeRequestState> state;       // set by Enqueue

        DecodeTask()
            : handle(0)
        {
        }
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

        // Add to the task queue. Returns false (and never invokes the callback)
        // once the scheduler has been shut down.
        bool Enqueue(DecodeTask task);

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
        std::queue<DecodeTask> m_thumbIoQueue;     // serialized disk reads (thumbnail/preview)
        std::queue<DecodeTask> m_backgroundQueue;  // CPU decode/resize work (from prefetched bytes)
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
