#pragma once

// NOTE: This file was renamed from ImagePipeline.h to match the new architecture:
// ImageDecodeDispatcher (routing/strategy) + IImageDecoder implementations.

#include "ImageRequest.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <memory>

// DirectXTex includes
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"

namespace ImageCore
{
    // 포맷 타입
    enum class ImageFormat
    {
        Unknown,
        WIC,    // WIC 지원 포맷 (JPEG, PNG, BMP, GIF 등)
        DDS,    // DirectDraw Surface
        HDR,    // Radiance HDR
        TGA,    // Targa
        EXR     // OpenEXR (optional)
    };

    // 포맷 감지
    ImageFormat DetectImageFormat(const std::wstring& path);

    // 디코딩 결과
    struct PipelineResult
    {
        HRESULT hr;
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap;  // WIC 경로용
        std::unique_ptr<DirectX::ScratchImage> scratchImage; // DirectXTex 경로용
        Size imageSize;                                      // 이미지 크기

        PipelineResult()
            : hr(E_FAIL)
            , imageSize{ 0.0f, 0.0f }
        {
        }

        PipelineResult(HRESULT h)
            : hr(h)
            , imageSize{ 0.0f, 0.0f }
        {
        }

        PipelineResult(const PipelineResult&) = delete;
        PipelineResult& operator=(const PipelineResult&) = delete;
        PipelineResult(PipelineResult&&) noexcept = default;
        PipelineResult& operator=(PipelineResult&&) noexcept = default;
    };

    // 디코더 공통 인터페이스 (decode + convert 결과는 PipelineResult로 반환)
    class IImageDecoder
    {
    public:
        virtual ~IImageDecoder() = default;
        virtual PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory) = 0;
    };

    // WIC 디코더들
    class WICImageDecoder : public IImageDecoder
    {
    protected:
        static HRESULT LoadWithWIC(const std::wstring& path, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize);
        static HRESULT DecodeAndScale(const std::wstring& path, const Size& targetSize, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize);
        static bool TryEmbeddedThumbnail(const std::wstring& path, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize);
    };

    class WICThumbnailDecoder final : public WICImageDecoder
    {
    public:
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory) override;
    };

    class WICFullImageDecoder final : public WICImageDecoder
    {
    public:
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory) override;
    };

    // DirectXTex 디코더들
    class DXTexImageDecoder : public IImageDecoder
    {
    protected:
        static HRESULT LoadWithDirectXTex(const std::wstring& path, DirectX::ScratchImage& scratchImage, DirectX::TexMetadata& metadata);
        static HRESULT ConvertScratchImageToWIC(
            const DirectX::ScratchImage& scratchImage,
            const Size& targetSize,
            IWICImagingFactory* wicFactory,
            Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap,
            Size& outSize);
        static bool TryDDSMipmapThumbnail(const DirectX::ScratchImage& scratchImage, const Size& targetSize, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize);
        static HRESULT ConvertScratchImageToBGRA8(const DirectX::ScratchImage& scratchImage, DirectX::ScratchImage& outScratchImage);
    };

    class DXTexThumbnailDecoder final : public DXTexImageDecoder
    {
    public:
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory) override;
    };

    class DXTexFullImageDecoder final : public DXTexImageDecoder
    {
    public:
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory) override;
    };

    // 1) 포맷 판단(routing) + 2) 디코딩 전략 선택(WIC vs DirectXTex) 담당
    // (3) 실제 decode/convert는 각 IImageDecoder 구현에 위임
    class ImageDecodeDispatcher final
    {
    public:
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory);

    private:
        WICThumbnailDecoder m_wicThumb {};
        WICFullImageDecoder m_wicFull {};
        DXTexThumbnailDecoder m_dxThumb {};
        DXTexFullImageDecoder m_dxFull {};
    };
}


