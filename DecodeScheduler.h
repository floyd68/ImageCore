#pragma once

#include "ImageRequest.h"
#include "DecodeTypes.h"
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace ImageCore
{
    // Decode task
    struct DecodeTask
    {
        ImageRequest request;
        std::function<void(PipelineResult&&)> callback;  // Callback that receives the DecodedImage result (move semantics)
        uint64_t handle;

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

        // Set the number of decode worker threads (the serialized thumbnail prefetch
        // thread is separate). With 1, a single shared worker serves full-resolution
        // work first and background work when no full-resolution work is queued; with
        // 2 or more, one worker is dedicated to full-resolution work and the rest
        // (capped) serve background work.
        void SetThreadCount(size_t count);

        // Default decode worker count for a machine: hardware threads - 1, minimum 1.
        static size_t DefaultWorkerCount(unsigned hardwareThreads);

        // Number of running decode worker threads (excludes the prefetch thread).
        size_t WorkerCount() const;

        // Add to the task queue
        void Enqueue(DecodeTask task);

        // 요청 취소 (best-effort: in-flight decode can't be interrupted, but callback will be suppressed)
        void Cancel(uint64_t handle);

        // Wait for all tasks to complete
        void WaitForCompletion();

        // Shutdown
        void Shutdown();

    private:
        enum class WorkerKind
        {
            HighPriority,
            Background,
            Shared      // sole worker: high queue first, then background queue
        };

        void WorkerThread(WorkerKind kind);
        void ThumbPrefetchThread();

        bool IsCanceled_NoLock(uint64_t handle) const;
        bool ConsumeCanceled_NoLock(uint64_t handle);

        std::vector<std::thread> m_highWorkers;
        std::vector<std::thread> m_backgroundWorkers;
        std::thread m_thumbPrefetchWorker;

        std::queue<DecodeTask> m_highQueue;
        std::queue<DecodeTask> m_thumbIoQueue;     // serialized disk reads (thumbnail/preview)
        std::queue<DecodeTask> m_backgroundQueue;  // CPU decode/resize work (from prefetched bytes)
        std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        std::atomic<bool> m_shutdown;

        size_t m_threadCount;

        struct VolumeIoProfile
        {
            bool incursSeek { true };
            size_t prefetchDepth { 2 };
        };

        std::unordered_map<std::wstring, VolumeIoProfile> m_volumeProfiles {};
        size_t m_globalPrefetchCap { 4 };

        // Canceled handles (guarded by m_queueMutex)
        std::unordered_set<uint64_t> m_canceledHandles {};
    };
}

