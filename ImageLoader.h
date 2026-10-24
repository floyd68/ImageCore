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

        // Explicit shutdown (call before FD2D::Core::Shutdown / CoUninitialize).
        // Idempotent and final: queued requests are dropped without callbacks,
        // and later requests are rejected.
        void Shutdown();

        // Image load request (async) - unified payload. The callback runs on a
        // worker thread. Returns 0 without ever invoking the callback when the
        // request is rejected (empty source, no callback, or after Shutdown).
        ImageHandle RequestDecoded(
            const ImageRequest& request,
            DecodedImageLoadCallback callback);

        // Best-effort cancel (see DecodeScheduler::Cancel). The callback can still
        // run after Cancel returns if delivery had already started; callers must
        // drop stale results themselves. Unknown or finished handles are a no-op.
        void Cancel(ImageHandle handle);

        // Clear cache
        void ClearCache();

        // Set cache size limit
        void SetCacheSizeLimit(size_t maxMemoryBytes);

        // Set worker thread count
        void SetWorkerThreadCount(size_t count);

        // Tests/diagnostics: requests currently queued or decoding (0 after Shutdown).
        size_t LiveRequestCount() const;

    private:
        ImageLoader();
        ~ImageLoader();
        ImageLoader(const ImageLoader&) = delete;
        ImageLoader& operator=(const ImageLoader&) = delete;

        void Initialize();

        std::shared_ptr<DecodeScheduler> Scheduler() const;

        std::unique_ptr<ImageCache> m_cache;

        // Requests copy the pointer under m_mutex and use the scheduler outside
        // it, so Shutdown (which joins workers) never holds the mutex while a
        // worker callback issues a new request.
        mutable std::mutex m_mutex;
        std::shared_ptr<DecodeScheduler> m_scheduler;

        std::atomic<ImageHandle> m_nextHandle;
    };
}
