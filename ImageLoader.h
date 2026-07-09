#pragma once

#include "ImageRequest.h"
#include "DecodedImage.h"
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <windows.h>

namespace ImageCore
{
    // Forward declarations
    class ImageCache;
    class DecodeScheduler;

    // ImageHandle: async request handle
    using ImageHandle = uint64_t;

    // Unified decoded payload callback (worker thread).
    // No WIC/DirectXTex types leak to FD2D.
    using DecodedImageLoadCallback = std::function<void(
        HRESULT hr,
        DecodedImage image)>;

    class ImageLoader
    {
    public:
        static ImageLoader& Instance();

        // Explicit shutdown (call before FD2D::Core::Shutdown / CoUninitialize)
        void Shutdown();

        // Image load request (async)
        // Image load request (async) - unified payload
        ImageHandle RequestDecoded(
            const ImageRequest& request,
            DecodedImageLoadCallback callback);

        // Cancel request
        void Cancel(ImageHandle handle);

        // Clear cache
        void ClearCache();

        // Set cache size limit
        void SetCacheSizeLimit(size_t maxMemoryBytes);

        // Set worker thread count
        void SetWorkerThreadCount(size_t count);

    private:
        ImageLoader();
        ~ImageLoader();
        ImageLoader(const ImageLoader&) = delete;
        ImageLoader& operator=(const ImageLoader&) = delete;

        void Initialize();

        std::unique_ptr<ImageCache> m_cache;
        std::unique_ptr<DecodeScheduler> m_scheduler;

        std::atomic<ImageHandle> m_nextHandle;
    };
}
