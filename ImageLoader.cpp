#include "ImageLoader.h"
#include "ImageCache.h"
#include "DecodeScheduler.h"
#include "ImageCore.h"
#include <algorithm>
#include <wincodec.h>
#include <comdef.h>

namespace ImageCore
{
    ImageLoader& ImageLoader::Instance()
    {
        static ImageLoader instance;
        return instance;
    }

    ImageLoader::ImageLoader()
        : m_nextHandle(1)
    {
        Initialize();
    }

    ImageLoader::~ImageLoader()
    {
        Shutdown();
    }

    void ImageLoader::Initialize()
    {
        // Ensure built-in decoders are registered (safe to call multiple times)
        RegisterBuiltInDecoders();

        m_cache = std::make_unique<ImageCache>();
        m_scheduler = std::make_unique<DecodeScheduler>();
    }

    void ImageLoader::Shutdown()
    {
        if (m_scheduler)
        {
            m_scheduler->Shutdown();
        }
        m_scheduler.reset();
        m_cache.reset();
    }

    ImageHandle ImageLoader::Request(
        const ImageRequest& request,
        ImageLoadCallback callback)
    {
        if (request.source.empty() || !callback)
        {
            return 0;
        }

        ImageHandle handle = m_nextHandle.fetch_add(1);

        // 1. 캐시 확인은 제거 (D2D1Bitmap 캐시는 FD2D 레이어에서 관리)

        // 2. 비동기 디코드 작업 큐에 추가
        DecodeTask task;
        task.request = request;
        task.handle = handle;
        task.callback = [callback](PipelineResult&& result)
        {
            if (FAILED(result.hr))
            {
                callback(result.hr, nullptr, nullptr);
                return;
            }

            // 콜백은 worker thread에서 즉시 호출됨.
            // UI 레이어(FD2D)는 여기서 받은 WIC/Scratch를 저장하고 InvalidateRect 등으로 렌더를 유도하면 됨.
            callback(S_OK, std::move(result.wicBitmap), std::move(result.scratchImage));
        };

        m_scheduler->Enqueue(std::move(task));

        return handle;
    }

    void ImageLoader::Cancel(ImageHandle handle)
    {
        if (handle == 0 || !m_scheduler)
        {
            return;
        }

        m_scheduler->Cancel(handle);
    }

    void ImageLoader::ClearCache()
    {
        if (m_cache)
        {
            m_cache->Clear();
        }
    }

    void ImageLoader::SetCacheSizeLimit(size_t maxMemoryBytes)
    {
        if (m_cache)
        {
            m_cache->SetSizeLimit(maxMemoryBytes);
        }
    }

    void ImageLoader::SetWorkerThreadCount(size_t count)
    {
        if (m_scheduler)
        {
            m_scheduler->SetThreadCount(count);
        }
    }
}

