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

        // 모든 작업 완료 대기
        void WaitForCompletion();

        // 종료
        void Shutdown();

    private:
        void WorkerThread();

        std::vector<std::thread> m_workers;
        std::queue<DecodeTask> m_taskQueue;
        std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        std::atomic<bool> m_shutdown;

        size_t m_threadCount;
    };
}

