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

    // ImageHandle: 비동기 요청 핸들
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

        // 명시적 종료 (FD2D::Core::Shutdown / CoUninitialize 전에 호출)
        void Shutdown();

        // 이미지 로딩 요청 (비동기)
        // 이미지 로딩 요청 (비동기) - unified payload
        ImageHandle RequestDecoded(
            const ImageRequest& request,
            DecodedImageLoadCallback callback);

        // 요청 취소
        void Cancel(ImageHandle handle);

        // 캐시 정리
        void ClearCache();

        // 캐시 크기 제한 설정
        void SetCacheSizeLimit(size_t maxMemoryBytes);

        // Worker thread 개수 설정
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

