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
        m_scheduler = std::make_shared<DecodeScheduler>();
    }

    std::shared_ptr<DecodeScheduler> ImageLoader::Scheduler() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_scheduler;
    }

    void ImageLoader::Shutdown()
    {
        std::shared_ptr<DecodeScheduler> scheduler;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            scheduler = std::move(m_scheduler);
        }
        if (scheduler)
        {
            // Joins the workers; a request racing with this sees either the
            // nullptr above or a scheduler that rejects it.
            scheduler->Shutdown();
        }
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

        std::shared_ptr<DecodeScheduler> scheduler = Scheduler();
        if (!scheduler)
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

        if (!scheduler->Enqueue(std::move(task)))
        {
            return 0;
        }
        return handle;
    }

    void ImageLoader::Cancel(ImageHandle handle)
    {
        if (handle == 0)
        {
            return;
        }

        if (std::shared_ptr<DecodeScheduler> scheduler = Scheduler())
        {
            scheduler->Cancel(handle);
        }
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
        if (std::shared_ptr<DecodeScheduler> scheduler = Scheduler())
        {
            scheduler->SetThreadCount(count);
        }
    }

    size_t ImageLoader::LiveRequestCount() const
    {
        std::shared_ptr<DecodeScheduler> scheduler = Scheduler();
        return scheduler ? scheduler->LiveRequestCount() : 0;
    }
}
