#pragma once

#include "ImageRequest.h"
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"

namespace ImageCore
{
    // Forward declarations
    class ImageCache;
    class DecodeScheduler;

    // ImageHandle: 비동기 요청 핸들
    using ImageHandle = uint64_t;

    // 로딩 완료 콜백 (worker thread에서 호출됨)
    // IWICBitmapSource 또는 ScratchImage를 전달, D2D bitmap 변환은 UI 레이어에서 처리
    using ImageLoadCallback = std::function<void(
        HRESULT hr,
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap,
        std::unique_ptr<DirectX::ScratchImage> scratchImage)>;

    class ImageLoader
    {
    public:
        static ImageLoader& Instance();

        // 명시적 종료 (FD2D::Core::Shutdown / CoUninitialize 전에 호출)
        void Shutdown();

        // 이미지 로딩 요청 (비동기)
        ImageHandle Request(
            const ImageRequest& request,
            ImageLoadCallback callback);

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
        std::mutex m_mutex;
    };
}

