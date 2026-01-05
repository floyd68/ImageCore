#include "DecodeScheduler.h"
#include "ImageDecodeDispatcher.h"
#include <algorithm>
#include <objbase.h>

namespace ImageCore
{
    DecodeScheduler::DecodeScheduler()
        : m_shutdown(false)
        , m_threadCount(0)
    {
        // 기본: CPU 코어 수 - 1 (최소 1개)
        size_t coreCount = std::max<size_t>(1, std::thread::hardware_concurrency() - 1);
        SetThreadCount(coreCount);
    }

    DecodeScheduler::~DecodeScheduler()
    {
        Shutdown();
    }

    void DecodeScheduler::SetThreadCount(size_t count)
    {
        Shutdown();

        m_threadCount = std::max<size_t>(1, count);
        m_shutdown = false;

        // Worker thread 시작
        for (size_t i = 0; i < m_threadCount; ++i)
        {
            m_workers.emplace_back(&DecodeScheduler::WorkerThread, this);
        }
    }

    void DecodeScheduler::Enqueue(DecodeTask task)
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_taskQueue.push(std::move(task));
        }
        m_queueCondition.notify_one();
    }

    void DecodeScheduler::WaitForCompletion()
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_queueCondition.wait(lock, [this] { return m_taskQueue.empty(); });
    }

    void DecodeScheduler::Shutdown()
    {
        if (m_shutdown)
        {
            return;
        }

        m_shutdown = true;
        m_queueCondition.notify_all();

        for (auto& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        m_workers.clear();
    }

    void DecodeScheduler::WorkerThread()
    {
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

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this] { return !m_taskQueue.empty() || m_shutdown; });

                if (m_shutdown && m_taskQueue.empty())
                {
                    break;
                }

                if (m_taskQueue.empty())
                {
                    continue;
                }

                task = std::move(m_taskQueue.front());
                m_taskQueue.pop();
            }

            // 디스패처를 통해 디코드 실행 (포맷 라우팅 + 디코더 선택 포함)
            PipelineResult result = dispatcher.Decode(task.request, nullptr);

            // 콜백 호출 (WIC bitmap 또는 ScratchImage 결과 전달)
            if (task.callback)
            {
                task.callback(std::move(result));  // move로 전달
            }
        }

        if (coInitialized)
        {
            CoUninitialize();
        }
    }
}

