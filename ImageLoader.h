#pragma once

#include "ImageRequest.h"
#include "DecodedImage.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <windows.h>

namespace ImageCore
{
    // Forward declarations
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
        // and later requests are rejected. Joins the decode threads without a
        // bound (see DecodeScheduler::Shutdown()), so a read that honours neither
        // its stop token nor OS cancellation keeps it waiting.
        void Shutdown();

        // Bounded shutdown (see DecodeScheduler::Shutdown(bound)): true when
        // every decode thread was joined within `bound`; the loader then drops
        // its scheduler as Shutdown() does. On false it keeps the scheduler
        // (the unjoined threads may still use it) and keeps rejecting
        // requests; a later call retries the join. The host should take a
        // forced-exit path rather than run static destruction with threads
        // still running (~ImageLoader joins without a bound).
        bool Shutdown(std::chrono::milliseconds bound);

        // Bounded shutdown that never constructs the loader: true at once when
        // Instance() was never called (so no scheduler and no thread is created
        // just to be joined), otherwise Instance().Shutdown(bound). A call that
        // observes a construction in progress waits for it, then shuts it down.
        // A first Instance() call after this returned is not supported.
        static bool ShutdownIfCreated(std::chrono::milliseconds bound);

        // Tests/diagnostics: whether Instance() has begun constructing the loader.
        static bool IsInstantiated();

        // Image load request (async) - unified payload. The callback runs on a
        // worker thread. Returns 0 without ever invoking the callback when the
        // request is rejected (empty source, no callback, or after Shutdown).
        // Without a priority, DefaultPriority(request.purpose) applies.
        ImageHandle RequestDecoded(
            const ImageRequest& request,
            DecodedImageLoadCallback callback);
        ImageHandle RequestDecoded(
            const ImageRequest& request,
            DecodedImageLoadCallback callback,
            RequestPriority priority);

        // Promotes or demotes a queued request (see DecodeScheduler::SetPriority).
        // Running, finished, canceled and unknown handles are a no-op.
        void SetPriority(ImageHandle handle, RequestPriority priority);

        // Best-effort cancel (see DecodeScheduler::Cancel). The callback can still
        // run after Cancel returns if delivery had already started; callers must
        // drop stale results themselves. Unknown or finished handles are a no-op.
        void Cancel(ImageHandle handle);

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

        // Requests copy the pointer under m_mutex and use the scheduler outside
        // it, so Shutdown (which joins workers) never holds the mutex while a
        // worker callback issues a new request.
        mutable std::mutex m_mutex;
        std::shared_ptr<DecodeScheduler> m_scheduler;

        std::atomic<ImageHandle> m_nextHandle;
    };
}
