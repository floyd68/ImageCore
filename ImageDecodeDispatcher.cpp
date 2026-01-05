#include "ImageDecodeDispatcher.h"
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"
#ifdef USE_OPENEXR
#include "../external/DirectXTex/Auxiliary/DirectXTexEXR.h"
#endif
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <float.h>
#include <cctype>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace ImageCore
{
    // 포맷 감지
    ImageFormat DetectImageFormat(const std::wstring& path)
    {
        if (path.empty())
        {
            return ImageFormat::Unknown;
        }

        // 파일 경로에서 확장자 추출
        size_t lastDot = path.find_last_of(L'.');
        size_t lastSlash = path.find_last_of(L"\\/");

        if (lastDot == std::wstring::npos || (lastSlash != std::wstring::npos && lastDot < lastSlash))
        {
            // 확장자가 없거나 경로에 포함된 경우
            return ImageFormat::Unknown;
        }

        std::wstring ext = path.substr(lastDot);

        // 소문자로 변환
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

        if (ext == L".dds")
        {
            return ImageFormat::DDS;
        }
        else if (ext == L".hdr")
        {
            return ImageFormat::HDR;
        }
        else if (ext == L".tga")
        {
            return ImageFormat::TGA;
        }
        else if (ext == L".exr")
        {
            return ImageFormat::EXR;
        }
        else
        {
            // JPEG, PNG, BMP, GIF, ICO 등은 WIC로 처리
            return ImageFormat::WIC;
        }
    }

    // WICThumbnailDecoder
    PipelineResult WICThumbnailDecoder::Decode(const ImageRequest& request, IWICImagingFactory* wicFactory)
    {
        if (request.source.empty() || wicFactory == nullptr)
        {
            return PipelineResult(E_INVALIDARG);
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap;
        Size imageSize;

        // Embedded thumbnail 시도
        if (TryEmbeddedThumbnail(request.source, wicFactory, wicBitmap, imageSize))
        {
            PipelineResult result;
            result.hr = S_OK;
            result.wicBitmap = wicBitmap;
            result.imageSize = imageSize;
            return result;
        }

        // WIC로 직접 로드 및 스케일링
        HRESULT hr = DecodeAndScale(request.source, request.targetSize, wicFactory, wicBitmap, imageSize);
        if (FAILED(hr))
        {
            return PipelineResult(hr);
        }

        PipelineResult result;
        result.hr = S_OK;
        result.wicBitmap = wicBitmap;
        result.imageSize = imageSize;
        return result;
    }

    // DXTexThumbnailDecoder
    PipelineResult DXTexThumbnailDecoder::Decode(const ImageRequest& request, IWICImagingFactory* wicFactory)
    {
        if (request.source.empty() || wicFactory == nullptr)
        {
            return PipelineResult(E_INVALIDARG);
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap;
        Size imageSize;

        // DirectXTex로 로드 (DDS, HDR, TGA, EXR)
        DirectX::ScratchImage scratchImage;
        DirectX::TexMetadata metadata;
        HRESULT hr = LoadWithDirectXTex(request.source, scratchImage, metadata);
        if (FAILED(hr))
        {
            return PipelineResult(hr);
        }

        // DDS mipmap 썸네일 시도
        if (DetectImageFormat(request.source) == ImageFormat::DDS)
        {
            if (TryDDSMipmapThumbnail(scratchImage, request.targetSize, wicFactory, wicBitmap, imageSize))
            {
                PipelineResult result;
                result.hr = S_OK;
                result.wicBitmap = wicBitmap;
                result.imageSize = imageSize;
                return result;
            }
        }

        hr = ConvertScratchImageToWIC(scratchImage, request.targetSize, wicFactory, wicBitmap, imageSize);
        if (FAILED(hr))
        {
            return PipelineResult(hr);
        }

        PipelineResult result;
        result.hr = S_OK;
        result.wicBitmap = wicBitmap;
        result.imageSize = imageSize;
        return result;
    }

    HRESULT DXTexImageDecoder::LoadWithDirectXTex(const std::wstring& path, DirectX::ScratchImage& scratchImage, DirectX::TexMetadata& metadata)
    {
        ImageFormat format = DetectImageFormat(path);
        HRESULT hr = E_FAIL;

        switch (format)
        {
        case ImageFormat::DDS:
            hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, scratchImage);
            break;

        case ImageFormat::HDR:
            hr = DirectX::LoadFromHDRFile(path.c_str(), &metadata, scratchImage);
            break;

        case ImageFormat::TGA:
            hr = DirectX::LoadFromTGAFile(path.c_str(), DirectX::TGA_FLAGS_NONE, &metadata, scratchImage);
            break;

        case ImageFormat::EXR:
#ifdef USE_OPENEXR
            hr = DirectX::LoadFromEXRFile(path.c_str(), &metadata, scratchImage);
#else
            hr = E_NOTIMPL;
#endif
            break;

        case ImageFormat::WIC:
        default:
            // WIC 포맷은 이 함수를 호출하지 않음 (직접 WIC 사용)
            hr = E_NOTIMPL;
            break;
        }

        return hr;
    }

    HRESULT DXTexImageDecoder::ConvertScratchImageToWIC(
        const DirectX::ScratchImage& scratchImage,
        const Size& targetSize,
        IWICImagingFactory* wicFactory,
        Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap,
        Size& outSize)
    {
        if (wicFactory == nullptr)
        {
            return E_POINTER;
        }

        const DirectX::Image* image = scratchImage.GetImage(0, 0, 0);
        if (image == nullptr)
        {
            return E_FAIL;
        }

        // 압축 포맷인 경우 먼저 Decompress
        DirectX::ScratchImage decompressed;
        const DirectX::Image* sourceImage = image;

        if (DirectX::IsCompressed(image->format))
        {
            HRESULT hr = DirectX::Decompress(
                *image,
                DXGI_FORMAT_UNKNOWN, // 자동으로 적절한 포맷 선택
                decompressed);
            if (FAILED(hr))
            {
                return hr;
            }
            sourceImage = decompressed.GetImage(0, 0, 0);
            if (sourceImage == nullptr)
            {
                return E_FAIL;
            }
        }

        // BGRA8로 변환
        DirectX::ScratchImage converted;
        HRESULT hr = DirectX::Convert(
            *sourceImage,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT,
            DirectX::TEX_THRESHOLD_DEFAULT,
            converted);
        if (FAILED(hr))
        {
            return hr;
        }

        const DirectX::Image* convertedImage = converted.GetImage(0, 0, 0);
        if (convertedImage == nullptr)
        {
            return E_FAIL;
        }

        // 스케일링 필요 여부 확인
        bool needScale = (targetSize.w > 0.0f && targetSize.h > 0.0f) &&
            (static_cast<float>(convertedImage->width) > targetSize.w || static_cast<float>(convertedImage->height) > targetSize.h);

        const DirectX::Image* finalImage = convertedImage;

        if (needScale)
        {
            // 비율 유지하면서 스케일링
            float scaleX = targetSize.w / static_cast<float>(convertedImage->width);
            float scaleY = targetSize.h / static_cast<float>(convertedImage->height);
            float scale = (std::min)(scaleX, scaleY);

            size_t scaledWidth = static_cast<size_t>(std::round(convertedImage->width * scale));
            size_t scaledHeight = static_cast<size_t>(std::round(convertedImage->height * scale));

            DirectX::ScratchImage scaled;
            hr = DirectX::Resize(
                *convertedImage,
                scaledWidth,
                scaledHeight,
                DirectX::TEX_FILTER_CUBIC,
                scaled);
            if (SUCCEEDED(hr))
            {
                finalImage = scaled.GetImage(0, 0, 0);
            }
        }

        // WIC bitmap 생성
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        hr = wicFactory->CreateBitmap(
            static_cast<UINT>(finalImage->width),
            static_cast<UINT>(finalImage->height),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnDemand,
            &bitmap);
        if (FAILED(hr))
        {
            return hr;
        }

        // 픽셀 데이터 복사
        WICRect rect = { 0, 0, static_cast<INT>(finalImage->width), static_cast<INT>(finalImage->height) };
        UINT stride = static_cast<UINT>(finalImage->rowPitch);
        UINT bufferSize = static_cast<UINT>(finalImage->slicePitch);
        hr = bitmap->CopyPixels(&rect, stride, bufferSize, finalImage->pixels);
        if (FAILED(hr))
        {
            return hr;
        }

        outBitmap = bitmap;
        outSize = { static_cast<float>(finalImage->width), static_cast<float>(finalImage->height) };
        return S_OK;
    }

    bool WICImageDecoder::TryEmbeddedThumbnail(const std::wstring& path, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize)
    {
        if (wicFactory == nullptr)
        {
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
        if (FAILED(hr))
        {
            return false;
        }

        // Thumbnail 시도
        Microsoft::WRL::ComPtr<IWICBitmapSource> thumbnail;
        hr = decoder->GetThumbnail(&thumbnail);
        if (SUCCEEDED(hr) && thumbnail)
        {
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            hr = wicFactory->CreateFormatConverter(&converter);
            if (SUCCEEDED(hr))
            {
                hr = converter->Initialize(
                    thumbnail.Get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0f,
                    WICBitmapPaletteTypeMedianCut);
                if (SUCCEEDED(hr))
                {
                    UINT width, height;
                    hr = converter->GetSize(&width, &height);
                    if (SUCCEEDED(hr))
                    {
                        outBitmap = converter;
                        outSize = { static_cast<float>(width), static_cast<float>(height) };
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool DXTexImageDecoder::TryDDSMipmapThumbnail(const DirectX::ScratchImage& scratchImage, const Size& targetSize, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize)
    {
        const DirectX::TexMetadata& metadata = scratchImage.GetMetadata();

        // Mip level 선택: targetSize에 맞는 가장 작은 mip level
        if (metadata.mipLevels <= 1 || (targetSize.w <= 0.0f && targetSize.h <= 0.0f))
        {
            return false;
        }

        size_t bestMip = 0;
        float bestDiff = FLT_MAX;

        for (size_t mip = 0; mip < metadata.mipLevels; ++mip)
        {
            const DirectX::Image* image = scratchImage.GetImage(mip, 0, 0);
            if (image == nullptr)
            {
                continue;
            }

            float diff = std::abs(static_cast<float>(image->width) - targetSize.w) +
                std::abs(static_cast<float>(image->height) - targetSize.h);

            if (diff < bestDiff)
            {
                bestDiff = diff;
                bestMip = mip;
            }
        }

        const DirectX::Image* mipImage = scratchImage.GetImage(bestMip, 0, 0);
        if (mipImage == nullptr)
        {
            return false;
        }

        // 압축 포맷인 경우 먼저 Decompress
        DirectX::ScratchImage decompressed;
        const DirectX::Image* sourceImage = mipImage;

        if (DirectX::IsCompressed(mipImage->format))
        {
            HRESULT hr = DirectX::Decompress(
                *mipImage,
                DXGI_FORMAT_UNKNOWN, // 자동으로 적절한 포맷 선택
                decompressed);
            if (FAILED(hr))
            {
                return false;
            }
            sourceImage = decompressed.GetImage(0, 0, 0);
            if (sourceImage == nullptr)
            {
                return false;
            }
        }

        // BGRA8로 변환
        DirectX::ScratchImage converted;
        HRESULT hr = DirectX::Convert(
            *sourceImage,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT,
            DirectX::TEX_THRESHOLD_DEFAULT,
            converted);
        if (FAILED(hr))
        {
            return false;
        }

        const DirectX::Image* convertedImage = converted.GetImage(0, 0, 0);
        if (convertedImage == nullptr)
        {
            return false;
        }

        // WIC bitmap 생성
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        hr = wicFactory->CreateBitmap(
            static_cast<UINT>(convertedImage->width),
            static_cast<UINT>(convertedImage->height),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnDemand,
            &bitmap);
        if (FAILED(hr))
        {
            return false;
        }

        // 픽셀 데이터 복사
        WICRect rect = { 0, 0, static_cast<INT>(convertedImage->width), static_cast<INT>(convertedImage->height) };
        UINT stride = static_cast<UINT>(convertedImage->rowPitch);
        UINT bufferSize = static_cast<UINT>(convertedImage->slicePitch);
        hr = bitmap->CopyPixels(&rect, stride, bufferSize, convertedImage->pixels);
        if (FAILED(hr))
        {
            return false;
        }

        outBitmap = bitmap;
        outSize = { static_cast<float>(convertedImage->width), static_cast<float>(convertedImage->height) };
        return true;
    }

    HRESULT WICImageDecoder::DecodeAndScale(const std::wstring& path, const Size& targetSize, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize)
    {
        if (wicFactory == nullptr)
        {
            return E_POINTER;
        }

        // WIC decoder 생성
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            return hr;
        }

        UINT width, height;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr))
        {
            return hr;
        }

        bool needScale = (targetSize.w > 0.0f && targetSize.h > 0.0f) &&
            (static_cast<float>(width) > targetSize.w || static_cast<float>(height) > targetSize.h);

        Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;

        if (needScale)
        {
            float scaleX = targetSize.w / static_cast<float>(width);
            float scaleY = targetSize.h / static_cast<float>(height);
            float scale = (std::min)(scaleX, scaleY);

            UINT scaledWidth = static_cast<UINT>(std::round(width * scale));
            UINT scaledHeight = static_cast<UINT>(std::round(height * scale));

            Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
            hr = wicFactory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr))
            {
                hr = scaler->Initialize(
                    frame.Get(),
                    scaledWidth,
                    scaledHeight,
                    WICBitmapInterpolationModeCubic);
                if (SUCCEEDED(hr))
                {
                    source = scaler;
                }
            }
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = converter->Initialize(
            source.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr))
        {
            return hr;
        }

        UINT finalWidth, finalHeight;
        hr = converter->GetSize(&finalWidth, &finalHeight);
        if (SUCCEEDED(hr))
        {
            outBitmap = converter;
            outSize = { static_cast<float>(finalWidth), static_cast<float>(finalHeight) };
        }
        return hr;
    }

    // WICFullImageDecoder
    PipelineResult WICFullImageDecoder::Decode(const ImageRequest& request, IWICImagingFactory* wicFactory)
    {
        if (request.source.empty() || wicFactory == nullptr)
        {
            return PipelineResult(E_INVALIDARG);
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap;
        Size imageSize;
        HRESULT hr = LoadWithWIC(request.source, wicFactory, wicBitmap, imageSize);
        if (FAILED(hr))
        {
            return PipelineResult(hr);
        }

        PipelineResult result;
        result.hr = S_OK;
        result.wicBitmap = wicBitmap;
        result.imageSize = imageSize;
        return result;
    }

    // DXTexFullImageDecoder
    PipelineResult DXTexFullImageDecoder::Decode(const ImageRequest& request, IWICImagingFactory* wicFactory)
    {
        UNREFERENCED_PARAMETER(wicFactory);
        if (request.source.empty())
        {
            return PipelineResult(E_INVALIDARG);
        }

        DirectX::ScratchImage scratchImage;
        DirectX::TexMetadata metadata;
        HRESULT hr = LoadWithDirectXTex(request.source, scratchImage, metadata);
        if (FAILED(hr))
        {
            return PipelineResult(hr);
        }

        auto convertedScratchImage = std::make_unique<DirectX::ScratchImage>();
        hr = ConvertScratchImageToBGRA8(scratchImage, *convertedScratchImage);
        if (FAILED(hr))
        {
            return PipelineResult(hr);
        }

        const DirectX::Image* image = convertedScratchImage->GetImage(0, 0, 0);
        if (!image)
        {
            return PipelineResult(E_FAIL);
        }

        PipelineResult result;
        result.hr = S_OK;
        result.scratchImage = std::move(convertedScratchImage);
        result.imageSize = { static_cast<float>(image->width), static_cast<float>(image->height) };
        return result;
    }

    HRESULT DXTexImageDecoder::ConvertScratchImageToBGRA8(const DirectX::ScratchImage& scratchImage, DirectX::ScratchImage& outScratchImage)
    {
        const DirectX::Image* image = scratchImage.GetImage(0, 0, 0);
        if (image == nullptr)
        {
            return E_FAIL;
        }

        DirectX::ScratchImage decompressed;
        const DirectX::Image* sourceImage = image;

        if (DirectX::IsCompressed(image->format))
        {
            HRESULT hr = DirectX::Decompress(
                *image,
                DXGI_FORMAT_UNKNOWN,
                decompressed);
            if (FAILED(hr))
            {
                return hr;
            }
            sourceImage = decompressed.GetImage(0, 0, 0);
            if (sourceImage == nullptr)
            {
                return E_FAIL;
            }
        }

        return DirectX::Convert(
            *sourceImage,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT,
            DirectX::TEX_THRESHOLD_DEFAULT,
            outScratchImage);
    }

    HRESULT WICImageDecoder::LoadWithWIC(const std::wstring& path, IWICImagingFactory* wicFactory, Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap, Size& outSize)
    {
        if (wicFactory == nullptr)
        {
            return E_POINTER;
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = wicFactory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            return hr;
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeMedianCut);
        if (FAILED(hr))
        {
            return hr;
        }

        UINT width, height;
        hr = converter->GetSize(&width, &height);
        if (SUCCEEDED(hr))
        {
            outBitmap = converter;
            outSize = { static_cast<float>(width), static_cast<float>(height) };
        }
        return hr;
    }

    // Dispatcher
    PipelineResult ImageDecodeDispatcher::Decode(const ImageRequest& request, IWICImagingFactory* wicFactory)
    {
        if (request.source.empty() || wicFactory == nullptr)
        {
            return PipelineResult(E_INVALIDARG);
        }

        const bool wantThumb = (request.purpose == ImagePurpose::Thumbnail || request.purpose == ImagePurpose::Preview);
        const ImageFormat format = DetectImageFormat(request.source);

        switch (format)
        {
        case ImageFormat::WIC:
            return wantThumb ? m_wicThumb.Decode(request, wicFactory) : m_wicFull.Decode(request, wicFactory);

        case ImageFormat::DDS:
        case ImageFormat::HDR:
        case ImageFormat::TGA:
        case ImageFormat::EXR:
            return wantThumb ? m_dxThumb.Decode(request, wicFactory) : m_dxFull.Decode(request, wicFactory);

        case ImageFormat::Unknown:
        default:
            break;
        }

        // Unknown이면 WIC 먼저, 실패하면 DXTex로도 시도
        {
            PipelineResult r = wantThumb ? m_wicThumb.Decode(request, wicFactory) : m_wicFull.Decode(request, wicFactory);
            if (SUCCEEDED(r.hr))
            {
                return r;
            }
            return wantThumb ? m_dxThumb.Decode(request, wicFactory) : m_dxFull.Decode(request, wicFactory);
        }
    }
}


