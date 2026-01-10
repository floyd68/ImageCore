#include "DecodeScheduler.h"
#include "ImageDecodeDispatcher.h"
#include "FileByteCache.h"
#include "ImageFormatDetector.h"
#include <algorithm>
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
            // Prefer "C:" style volume handle for IOCTL queries.
            // For UNC paths, fall back to empty (unknown -> conservative).
            if (path.size() >= 2 && path[1] == L':')
            {
                std::wstring vol;
                vol.push_back(path[0]);
                vol.push_back(L':');
                return vol;
            }
            return L"";
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

        static size_t PrefetchDepthForPath(const std::wstring& path)
        {
            // Cache per-volume result since querying storage properties is relatively expensive.
            static std::mutex s_mutex;
            static std::unordered_map<std::wstring, bool> s_seekPenaltyByVolume;

            const std::wstring vol = GetVolumePathForFilePath(path);
            bool incursSeek = true;

            {
                std::lock_guard<std::mutex> lock(s_mutex);
                auto it = s_seekPenaltyByVolume.find(vol);
                if (it != s_seekPenaltyByVolume.end())
                {
                    incursSeek = it->second;
                }
                else
                {
                    incursSeek = QueryIncursSeekPenaltyForVolume(vol);
                    s_seekPenaltyByVolume.emplace(vol, incursSeek);
                }
            }

            // SSD/NVMe (no seek penalty) -> deeper prefetch helps hide latency.
            // HDD (seek penalty) -> keep shallow to avoid memory + avoid extra I/O pressure.
            return incursSeek ? 2 : 4;
        }

        static std::shared_ptr<std::vector<uint8_t>> ReadWholeFileBytes(const std::wstring& path)
        {
            HANDLE h = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (h == INVALID_HANDLE_VALUE)
            {
                return nullptr;
            }

            LARGE_INTEGER size {};
            if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0)
            {
                CloseHandle(h);
                return nullptr;
            }

            if (size.QuadPart > static_cast<LONGLONG>(1024LL * 1024LL * 1024LL))
            {
                // Safety: don't prefetch >1GB into memory.
                CloseHandle(h);
                return nullptr;
            }

            auto bytes = std::make_shared<std::vector<uint8_t>>();
            bytes->resize(static_cast<size_t>(size.QuadPart));

            DWORD totalRead = 0;
            uint8_t* dst = bytes->data();
            DWORD remaining = static_cast<DWORD>(bytes->size());
            while (remaining > 0)
            {
                DWORD readNow = 0;
                if (!ReadFile(h, dst + totalRead, remaining, &readNow, nullptr) || readNow == 0)
                {
                    CloseHandle(h);
                    return nullptr;
                }
                totalRead += readNow;
                remaining -= readNow;
            }

            CloseHandle(h);
            return bytes;
        }

        // (debug-only tracing removed)
    }

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

        // Start 1 high-priority worker (interactive full-res), rest background (thumbnails).
        // This prevents thumbnail backlogs from blocking main image loads.
        const size_t highCount = 1;
        // Background workers are CPU-bound in the thumbnail pipeline (I/O is serialized separately).
        // Keep a reasonable cap to avoid over-subscribing on decode-heavy workloads.
        const size_t maxBgCount = 4;
        const size_t bgCountUnclamped = (m_threadCount > highCount) ? (m_threadCount - highCount) : 0;
        const size_t bgCount = (std::min)(bgCountUnclamped, maxBgCount);

        for (size_t i = 0; i < highCount; ++i)
        {
            m_highWorkers.emplace_back(&DecodeScheduler::WorkerThread, this, WorkerKind::HighPriority);
        }
        for (size_t i = 0; i < bgCount; ++i)
        {
            m_backgroundWorkers.emplace_back(&DecodeScheduler::WorkerThread, this, WorkerKind::Background);
        }

        // One serialized thumbnail I/O thread: reads files sequentially into memory, then schedules CPU work.
        // This avoids HDD seek thrashing while still allowing CPU decode/resize to run in parallel.
        m_thumbPrefetchWorker = std::thread(&DecodeScheduler::ThumbPrefetchThread, this);

        // Start with a conservative global cap; it will be auto-tuned as volumes are observed.
        m_globalPrefetchCap = 4;
        FileByteCache::Instance().SetMaxEntries(m_globalPrefetchCap);
    }

    void DecodeScheduler::Enqueue(DecodeTask task)
    {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            // FullResolution is user-interactive (main image). Prioritize it.
            if (task.request.purpose == ImagePurpose::FullResolution)
            {
                m_highQueue.push(std::move(task));
            }
            else
            {
                // Auto-tune per-volume thumbnail prefetch depth based on storage seek penalty.
                const std::wstring vol = FileByteCache::GetVolumeForPath(task.request.source);
                const size_t desiredDepth = PrefetchDepthForPath(task.request.source);
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
    }

    void DecodeScheduler::Cancel(uint64_t handle)
    {
        if (handle == 0)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_canceledHandles.insert(handle);
        }

        // Wake all waiters so any thread blocked on empty queues can re-check cancel state.
        m_queueCondition.notify_all();
    }

    bool DecodeScheduler::IsCanceled_NoLock(uint64_t handle) const
    {
        return m_canceledHandles.find(handle) != m_canceledHandles.end();
    }

    bool DecodeScheduler::ConsumeCanceled_NoLock(uint64_t handle)
    {
        auto it = m_canceledHandles.find(handle);
        if (it == m_canceledHandles.end())
        {
            return false;
        }
        m_canceledHandles.erase(it);
        return true;
    }

    void DecodeScheduler::WaitForCompletion()
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_queueCondition.wait(lock, [this]
        {
            return m_highQueue.empty() && m_thumbIoQueue.empty() && m_backgroundQueue.empty();
        });
    }

    void DecodeScheduler::Shutdown()
    {
        if (m_shutdown)
        {
            return;
        }

        m_shutdown = true;
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

        // Drain queues
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_highQueue.empty())
            {
                m_highQueue.pop();
            }
            while (!m_thumbIoQueue.empty())
            {
                m_thumbIoQueue.pop();
            }
            while (!m_backgroundQueue.empty())
            {
                m_backgroundQueue.pop();
            }
        }
    }

    void DecodeScheduler::WorkerThread(WorkerKind kind)
    {
        // Thread scheduling priority:
        // - HighPriority: faster response for main image loads
        // - Background: lower CPU + lower I/O priority (important for HDD bottlenecks)
        if (kind == WorkerKind::HighPriority)
        {
            (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
        else
        {
            // Background mode lowers CPU scheduling and I/O priority.
            (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
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
                    // Background workers service high queue first (work-steal),
                    // but won't starve the main image because we also have a dedicated high worker.
                    if (!m_highQueue.empty())
                    {
                        task = std::move(m_highQueue.front());
                        m_highQueue.pop();
                    }
                    else if (!m_backgroundQueue.empty())
                    {
                        task = std::move(m_backgroundQueue.front());
                        m_backgroundQueue.pop();
                    }
                    else
                    {
                        continue;
                    }
                }

                // If canceled while sitting in the queue, skip the task.
                if (ConsumeCanceled_NoLock(task.handle))
                {
                    // IMPORTANT:
                    // Thumbnail/Preview requests may have already prefetched bytes into FileByteCache.
                    // If we drop the task here (before decode), we must free any unshared prefetched bytes,
                    // otherwise the prefetch thread can deadlock on cache backpressure and thumbnails
                    // will get stuck "loading" forever after rapid folder navigation.
                    if (task.request.purpose != ImagePurpose::FullResolution)
                    {
                        FileByteCache::Instance().EraseIfUnshared(task.request.source);
                        m_queueCondition.notify_all();
                    }
                    continue;
                }
            }

            // 디스패처를 통해 디코드 실행 (포맷 라우팅 + 디코더 선택 포함)
            PipelineResult result;
            std::shared_ptr<const std::vector<uint8_t>> bytesHold {};
            {
                DecodeInput input {};
                if (task.request.purpose != ImagePurpose::FullResolution)
                {
                    bytesHold = FileByteCache::Instance().Get(task.request.source);
                    if (bytesHold && !bytesHold->empty())
                    {
                        input.bytes = std::span<const uint8_t>(bytesHold->data(), bytesHold->size());
                        const size_t headerSize = (std::min)(bytesHold->size(), ImageFormatDetector::kProbeSize);
                        input.header = std::span<const uint8_t>(bytesHold->data(), headerSize);
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
            }

            // 콜백 호출 (DecodedImage 결과 전달)
            if (task.callback)
            {
                // If canceled while decoding, suppress callback.
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    if (ConsumeCanceled_NoLock(task.handle))
                    {
                        // Intentionally drop.
                        goto after_decode;
                    }
                }
                task.callback(std::move(result));  // move로 전달
            }
after_decode:

            // Thumbnail/Preview prefetched bytes are transient; free them once decode finishes if unshared.
            if (task.request.purpose != ImagePurpose::FullResolution)
            {
                // Release our local hold before attempting EraseIfUnshared.
                bytesHold.reset();
                FileByteCache::Instance().EraseIfUnshared(task.request.source);
                m_queueCondition.notify_all(); // wake prefetch thread if it was waiting for cache space
            }
        }

        if (coInitialized)
        {
            CoUninitialize();
        }

        // End background mode if set (best-effort).
        if (kind == WorkerKind::Background)
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

                if (ConsumeCanceled_NoLock(task.handle))
                {
                    continue;
                }
            }

            // Ensure this stage is only used for thumbnail/preview.
            if (task.request.purpose == ImagePurpose::FullResolution)
            {
                // Should never happen, but push to high queue just in case.
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_highQueue.push(std::move(task));
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

            // Backpressure without Sleep: wait until cache has room for this volume.
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCondition.wait(lock, [this, vol, volCap]
                {
                    if (m_shutdown)
                    {
                        return true;
                    }
                    const size_t total = FileByteCache::Instance().Count();
                    const size_t volCount = FileByteCache::Instance().CountForVolume(vol);
                    return total < m_globalPrefetchCap && volCount < volCap;
                });
                if (m_shutdown)
                {
                    break;
                }
            }

            // Read file bytes sequentially on this single thread.
            // If already prefetched (e.g., duplicate task), skip disk I/O.
            if (!FileByteCache::Instance().Get(task.request.source))
            {
                auto bytes = ReadWholeFileBytes(task.request.source);
                if (bytes)
                {
                    FileByteCache::Instance().Put(task.request.source, bytes);
                }
            }

            // Schedule CPU decode work regardless; decoder can fall back to file path if prefetch failed.
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_backgroundQueue.push(std::move(task));
            }
            m_queueCondition.notify_all();
        }

        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
}

