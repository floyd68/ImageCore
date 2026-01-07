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
#include <wincodec.h>
#include <unordered_map>
#include <unordered_set>

namespace ImageCore
{
    // 디코드 작업
    struct DecodeTask
    {
        ImageRequest request;
        std::function<void(PipelineResult&&)> callback;  // WIC bitmap 또는 ScratchImage 결과를 받는 콜백 (move semantics)
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

        // Worker thread 개수 설정
        void SetThreadCount(size_t count);

        // 작업 큐에 추가
        void Enqueue(DecodeTask task);

        // 요청 취소 (best-effort: in-flight decode can't be interrupted, but callback will be suppressed)
        void Cancel(uint64_t handle);

        // 모든 작업 완료 대기
        void WaitForCompletion();

        // 종료
        void Shutdown();

    private:
        enum class WorkerKind
        {
            HighPriority,
            Background
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

