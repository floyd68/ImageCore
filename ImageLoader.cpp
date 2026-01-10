#include "ImageLoader.h"
#include "ImageCache.h"
#include "DecodeScheduler.h"
#include "ImageCore.h"
#include <algorithm>
#include <wincodec.h>
#include <comdef.h>
#include <dxgiformat.h>

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

    ImageHandle ImageLoader::RequestDecoded(
        const ImageRequest& request,
        DecodedImageLoadCallback callback)
    {
        if (request.source.empty() || !callback)
        {
            return 0;
        }

        ImageHandle handle = m_nextHandle.fetch_add(1);

        DecodeTask task;
        task.request = request;
        task.handle = handle;
        task.callback = [callback](PipelineResult&& result)
        {
            if (FAILED(result.hr))
            {
                callback(result.hr, DecodedImage {});
                return;
            }

            if (result.image.blocks == nullptr || result.image.blocks->empty() || result.image.dxgiFormat == DXGI_FORMAT_UNKNOWN)
            {
                callback(E_FAIL, DecodedImage {});
                return;
            }

            callback(S_OK, std::move(result.image));
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

