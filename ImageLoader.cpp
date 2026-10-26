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
    namespace
    {
        // Set when the Instance() static begins construction.
        std::atomic<bool> s_instantiated { false };
    }

    ImageLoader& ImageLoader::Instance()
    {
        static ImageLoader instance;
        return instance;
    }

    bool ImageLoader::IsInstantiated()
    {
        return s_instantiated.load();
    }

    bool ImageLoader::ShutdownIfCreated(std::chrono::milliseconds bound)
    {
        if (!s_instantiated.load())
        {
            return true;
        }
        // Constructed, or being constructed: Instance() waits for the
        // initialization of its static to finish.
        return Instance().Shutdown(bound);
    }

    ImageLoader::ImageLoader()
        : m_nextHandle(1)
    {
        s_instantiated.store(true);
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

    bool ImageLoader::Shutdown(std::chrono::milliseconds bound)
    {
        std::shared_ptr<DecodeScheduler> scheduler = Scheduler();
        if (scheduler && !scheduler->Shutdown(bound))
        {
            // Threads are still running: keep the scheduler and the cache they
            // may use. The scheduler rejects requests meanwhile.
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_scheduler == scheduler)
            {
                m_scheduler.reset();
            }
        }
        m_cache.reset();
        return true;
    }

    ImageHandle ImageLoader::RequestDecoded(
        const ImageRequest& request,
        DecodedImageLoadCallback callback)
    {
        return RequestDecoded(request, std::move(callback), DefaultPriority(request.purpose));
    }

    ImageHandle ImageLoader::RequestDecoded(
        const ImageRequest& request,
        DecodedImageLoadCallback callback,
        RequestPriority priority)
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
        task.priority = priority;
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

    void ImageLoader::SetPriority(ImageHandle handle, RequestPriority priority)
    {
        if (handle == 0)
        {
            return;
        }

        if (std::shared_ptr<DecodeScheduler> scheduler = Scheduler())
        {
            scheduler->SetPriority(handle, priority);
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
