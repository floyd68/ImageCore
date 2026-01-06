#include "ImageDecodeDispatcher.h"
#include "DecoderRegistry.h"
#include "IImageDecoderFactory.h"
#include "ImageCore.h"
#include "ImageFormatDetector.h"

#include "../external/DirectXTex/DirectXTex/DirectXTex.h"
#ifdef USE_OPENEXR
#include "../external/DirectXTex/Auxiliary/DirectXTexEXR.h"
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <objbase.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace ImageCore
{
    namespace
    {
        static std::wstring GetLowerExtension(const std::wstring& path)
        {
            std::filesystem::path p(path);
            std::wstring ext = p.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            return ext;
        }

        // ---------- WIC common ----------
        class WICDecoderCommon
        {
        protected:
            static HRESULT CreateDecoder(
                const std::wstring& path,
                std::span<const uint8_t> bytes,
                Microsoft::WRL::ComPtr<IWICBitmapDecoder>& outDecoder)
            {
                Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory = GetWicFactory();
                if (!wicFactory)
                {
                    return E_POINTER;
                }

                HRESULT hr = E_FAIL;
                if (!bytes.empty() && bytes.size() <= static_cast<size_t>(UINT32_MAX))
                {
                    Microsoft::WRL::ComPtr<IWICStream> stream;
                    hr = wicFactory->CreateStream(&stream);
                    if (SUCCEEDED(hr) && stream)
                    {
                        hr = stream->InitializeFromMemory(
                            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
                            static_cast<DWORD>(bytes.size()));
                    }
                    if (SUCCEEDED(hr) && stream)
                    {
                        hr = wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &outDecoder);
                    }
                }
                else
                {
                    hr = wicFactory->CreateDecoderFromFilename(
                        path.c_str(),
                        nullptr,
                        GENERIC_READ,
                        WICDecodeMetadataCacheOnLoad,
                        &outDecoder);
                }

                return hr;
            }

            static Microsoft::WRL::ComPtr<IWICImagingFactory> GetWicFactory()
            {
                // Per-thread WIC factory (DecodeScheduler worker threads already CoInitializeEx'd).
                thread_local Microsoft::WRL::ComPtr<IWICImagingFactory> s_factory {};
                if (!s_factory)
                {
                    (void)CoCreateInstance(
                        CLSID_WICImagingFactory,
                        nullptr,
                        CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&s_factory));
                }
                return s_factory;
            }

            static bool TryEmbeddedThumbnail(
                const std::wstring& path,
                std::span<const uint8_t> bytes,
                Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap,
                Size& outSize)
            {
                Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory = GetWicFactory();
                if (!wicFactory)
                {
                    return false;
                }

                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                HRESULT hr = CreateDecoder(path, bytes, decoder);
                if (FAILED(hr))
                {
                    return false;
                }

                Microsoft::WRL::ComPtr<IWICBitmapSource> thumbnail;
                hr = decoder->GetThumbnail(&thumbnail);
                if (FAILED(hr) || !thumbnail)
                {
                    return false;
                }

                Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                hr = wicFactory->CreateFormatConverter(&converter);
                if (FAILED(hr))
                {
                    return false;
                }

                hr = converter->Initialize(
                    thumbnail.Get(),
                    GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0f,
                    WICBitmapPaletteTypeMedianCut);
                if (FAILED(hr))
                {
                    return false;
                }

                UINT width = 0;
                UINT height = 0;
                hr = converter->GetSize(&width, &height);
                if (FAILED(hr))
                {
                    return false;
                }

                outBitmap = converter;
                outSize = { static_cast<float>(width), static_cast<float>(height) };
                return true;
            }

            static HRESULT DecodeAndScale(
                const std::wstring& path,
                std::span<const uint8_t> bytes,
                const Size& targetSize,
                Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap,
                Size& outSize)
            {
                Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory = GetWicFactory();
                if (!wicFactory)
                {
                    return E_POINTER;
                }

                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                HRESULT hr = CreateDecoder(path, bytes, decoder);
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

                UINT width = 0;
                UINT height = 0;
                hr = frame->GetSize(&width, &height);
                if (FAILED(hr))
                {
                    return hr;
                }

                const bool needScale = (targetSize.w > 0.0f && targetSize.h > 0.0f) &&
                    (static_cast<float>(width) > targetSize.w || static_cast<float>(height) > targetSize.h);

                Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;

                if (needScale)
                {
                    const float scaleX = targetSize.w / static_cast<float>(width);
                    const float scaleY = targetSize.h / static_cast<float>(height);
                    const float scale = (std::min)(scaleX, scaleY);

                    const UINT scaledWidth = static_cast<UINT>(std::round(width * scale));
                    const UINT scaledHeight = static_cast<UINT>(std::round(height * scale));

                    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
                    hr = wicFactory->CreateBitmapScaler(&scaler);
                    if (SUCCEEDED(hr))
                    {
                        hr = scaler->Initialize(frame.Get(), scaledWidth, scaledHeight, WICBitmapInterpolationModeCubic);
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

                UINT finalWidth = 0;
                UINT finalHeight = 0;
                hr = converter->GetSize(&finalWidth, &finalHeight);
                if (SUCCEEDED(hr))
                {
                    outBitmap = converter;
                    outSize = { static_cast<float>(finalWidth), static_cast<float>(finalHeight) };
                }
                return hr;
            }

            static HRESULT LoadWithWIC(
                const std::wstring& path,
                std::span<const uint8_t> bytes,
                Microsoft::WRL::ComPtr<IWICBitmapSource>& outBitmap,
                Size& outSize)
            {
                Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory = GetWicFactory();
                if (!wicFactory)
                {
                    return E_POINTER;
                }

                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                HRESULT hr = CreateDecoder(path, bytes, decoder);
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

                UINT width = 0;
                UINT height = 0;
                hr = converter->GetSize(&width, &height);
                if (SUCCEEDED(hr))
                {
                    outBitmap = converter;
                    outSize = { static_cast<float>(width), static_cast<float>(height) };
                }
                return hr;
            }
        };

        // ---------- DXTex common ----------
        class DXTexDecoderCommon
        {
        protected:
            static HRESULT LoadWithDirectXTex(
                const ImageRequest& request,
                std::span<const uint8_t> bytes,
                DirectX::ScratchImage& scratchImage,
                DirectX::TexMetadata& metadata)
            {
                const std::wstring& path = request.source;
                const std::wstring ext = GetLowerExtension(path);
                if (ext == L".dds")
                {
                    // NOTE:
                    // - FullResolution(main image): top mip is enough for viewing; avoid loading mip chains.
                    // - Thumbnail/Preview: prefer loading mip chains so we can decode from a small mip
                    //   (huge speedup for 8K BCn textures).
                    DirectX::DDS_FLAGS flags = DirectX::DDS_FLAGS_NONE;
                    if (request.purpose == ImagePurpose::FullResolution)
                    {
                        flags = static_cast<DirectX::DDS_FLAGS>(DirectX::DDS_FLAGS_NONE | DirectX::DDS_FLAGS_IGNORE_MIPS);
                    }
                    if (!bytes.empty())
                    {
                        return DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), flags, &metadata, scratchImage);
                    }
                    return DirectX::LoadFromDDSFile(path.c_str(), flags, &metadata, scratchImage);
                }
                if (ext == L".hdr")
                {
                    if (!bytes.empty())
                    {
                        return DirectX::LoadFromHDRMemory(bytes.data(), bytes.size(), &metadata, scratchImage);
                    }
                    return DirectX::LoadFromHDRFile(path.c_str(), &metadata, scratchImage);
                }
                if (ext == L".tga")
                {
                    if (!bytes.empty())
                    {
                        return DirectX::LoadFromTGAMemory(bytes.data(), bytes.size(), DirectX::TGA_FLAGS_NONE, &metadata, scratchImage);
                    }
                    return DirectX::LoadFromTGAFile(path.c_str(), DirectX::TGA_FLAGS_NONE, &metadata, scratchImage);
                }
                if (ext == L".exr")
                {
#ifdef USE_OPENEXR
                    return DirectX::LoadFromEXRFile(path.c_str(), &metadata, scratchImage);
#else
                    return E_NOTIMPL;
#endif
                }

                return E_NOTIMPL;
            }

            static HRESULT ConvertImageToBGRA8(const DirectX::Image& image, DirectX::ScratchImage& outScratchImage)
            {
                // Fast-path: already in the target format and not compressed.
                if (!DirectX::IsCompressed(image.format) && image.format == DXGI_FORMAT_B8G8R8A8_UNORM)
                {
                    outScratchImage = DirectX::ScratchImage();
                    return outScratchImage.InitializeFromImage(image, false);
                }

                DirectX::ScratchImage decompressed;
                const DirectX::Image* sourceImage = &image;

                if (DirectX::IsCompressed(image.format))
                {
                    HRESULT hr = DirectX::Decompress(image, DXGI_FORMAT_UNKNOWN, decompressed);
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    sourceImage = decompressed.GetImage(0, 0, 0);
                    if (!sourceImage)
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

            static HRESULT ConvertScratchImageToBGRA8(const DirectX::ScratchImage& scratchImage, DirectX::ScratchImage& outScratchImage)
            {
                const DirectX::Image* image = scratchImage.GetImage(0, 0, 0);
                if (!image)
                {
                    return E_FAIL;
                }

                return ConvertImageToBGRA8(*image, outScratchImage);
            }
        };

        // ---------- Built-in decoders ----------
        class WICThumbnailDecoder final : public IImageDecoder, private WICDecoderCommon
        {
        public:
            PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input) override
            {
                UNREFERENCED_PARAMETER(wicFactory);
                if (request.source.empty())
                {
                    return PipelineResult(E_INVALIDARG);
                }

                Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap;
                Size imageSize;

                if (TryEmbeddedThumbnail(request.source, input.bytes, wicBitmap, imageSize))
                {
                    PipelineResult result;
                    result.hr = S_OK;
                    result.wicBitmap = wicBitmap;
                    result.imageSize = imageSize;
                    return result;
                }

                HRESULT hr = DecodeAndScale(request.source, input.bytes, request.targetSize, wicBitmap, imageSize);
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
        };

        class WICFullImageDecoder final : public IImageDecoder, private WICDecoderCommon
        {
        public:
            PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input) override
            {
                UNREFERENCED_PARAMETER(wicFactory);
                if (request.source.empty())
                {
                    return PipelineResult(E_INVALIDARG);
                }

                Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap;
                Size imageSize;
                HRESULT hr = LoadWithWIC(request.source, input.bytes, wicBitmap, imageSize);
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
        };

        class DXTexThumbnailDecoder final : public IImageDecoder, private DXTexDecoderCommon
        {
        public:
            PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input) override
            {
                UNREFERENCED_PARAMETER(wicFactory);
                if (request.source.empty())
                {
                    return PipelineResult(E_INVALIDARG);
                }

                DirectX::ScratchImage scratchImage;
                DirectX::TexMetadata metadata;
                HRESULT hr = LoadWithDirectXTex(request, input.bytes, scratchImage, metadata);
                if (FAILED(hr))
                {
                    return PipelineResult(hr);
                }

                // Prefer decoding from a smaller mip for thumbnails (huge CPU win for 8K BCn DDS).
                size_t mipIndex = 0;
                if (GetLowerExtension(request.source) == L".dds" && metadata.mipLevels > 1 &&
                    request.targetSize.w > 0.0f && request.targetSize.h > 0.0f)
                {
                    const float targetMax = (std::max)(request.targetSize.w, request.targetSize.h);
                    size_t bestMip = 0;
                    size_t bestMaxDim = static_cast<size_t>(-1);

                    for (size_t m = 0; m < metadata.mipLevels; ++m)
                    {
                        const DirectX::Image* img = scratchImage.GetImage(m, 0, 0);
                        if (!img)
                        {
                            continue;
                        }

                        const size_t maxDim = (std::max)(img->width, img->height);
                        // Pick the smallest mip that is still >= target (avoids upscaling).
                        if (static_cast<float>(maxDim) >= targetMax)
                        {
                            if (maxDim < bestMaxDim)
                            {
                                bestMaxDim = maxDim;
                                bestMip = m;
                            }
                        }
                    }

                    if (bestMaxDim != static_cast<size_t>(-1))
                    {
                        mipIndex = bestMip;
                    }
                    else
                    {
                        // If all mips are smaller than target, use the smallest available.
                        mipIndex = static_cast<size_t>((metadata.mipLevels > 0) ? (metadata.mipLevels - 1) : 0);
                    }
                }

                auto convertedScratch = std::make_unique<DirectX::ScratchImage>();
                const DirectX::Image* loadedImage = scratchImage.GetImage(mipIndex, 0, 0);
                if (loadedImage && !DirectX::IsCompressed(loadedImage->format) && loadedImage->format == DXGI_FORMAT_B8G8R8A8_UNORM)
                {
                    // Avoid unnecessary Convert/copy when DDS is already BGRA8.
                    convertedScratch->InitializeFromImage(*loadedImage, false);
                }
                else
                {
                    if (!loadedImage)
                    {
                        return PipelineResult(E_FAIL);
                    }
                    hr = ConvertImageToBGRA8(*loadedImage, *convertedScratch);
                    if (FAILED(hr))
                    {
                        return PipelineResult(hr);
                    }
                }

                const DirectX::Image* convertedImage = convertedScratch->GetImage(0, 0, 0);
                if (!convertedImage)
                {
                    return PipelineResult(E_FAIL);
                }

                if (request.targetSize.w > 0.0f && request.targetSize.h > 0.0f &&
                    (static_cast<float>(convertedImage->width) > request.targetSize.w || static_cast<float>(convertedImage->height) > request.targetSize.h))
                {
                    const float scaleX = request.targetSize.w / static_cast<float>(convertedImage->width);
                    const float scaleY = request.targetSize.h / static_cast<float>(convertedImage->height);
                    const float scale = (std::min)(scaleX, scaleY);

                    const size_t scaledWidth = static_cast<size_t>(std::round(convertedImage->width * scale));
                    const size_t scaledHeight = static_cast<size_t>(std::round(convertedImage->height * scale));

                    auto scaledScratch = std::make_unique<DirectX::ScratchImage>();
                    // Thumbnails: prioritize speed (FANT is a fast downsampling filter).
                    hr = DirectX::Resize(*convertedImage, scaledWidth, scaledHeight, DirectX::TEX_FILTER_FANT, *scaledScratch);
                    if (SUCCEEDED(hr))
                    {
                        convertedScratch = std::move(scaledScratch);
                        convertedImage = convertedScratch->GetImage(0, 0, 0);
                    }
                }

                if (!convertedImage)
                {
                    return PipelineResult(E_FAIL);
                }

                PipelineResult result;
                result.hr = S_OK;
                result.scratchImage = std::move(convertedScratch);
                result.imageSize = { static_cast<float>(convertedImage->width), static_cast<float>(convertedImage->height) };
                return result;
            }
        };

        class DXTexFullImageDecoder final : public IImageDecoder, private DXTexDecoderCommon
        {
        public:
            PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input) override
            {
                UNREFERENCED_PARAMETER(wicFactory);
                if (request.source.empty())
                {
                    return PipelineResult(E_INVALIDARG);
                }

                DirectX::ScratchImage scratchImage;
                DirectX::TexMetadata metadata;
                HRESULT hr = LoadWithDirectXTex(request, input.bytes, scratchImage, metadata);
                if (FAILED(hr))
                {
                    return PipelineResult(hr);
                }

                // Performance / GPU path: if the DDS is GPU-compressed, return the original ScratchImage
                // so the renderer can upload it as a GPU texture without CPU Decompress.
                const std::wstring ext = GetLowerExtension(request.source);
                const DirectX::Image* loadedImage = scratchImage.GetImage(0, 0, 0);
                if (request.allowGpuCompressedDDS && ext == L".dds" && loadedImage && DirectX::IsCompressed(loadedImage->format))
                {
                    auto gpuScratch = std::make_unique<DirectX::ScratchImage>();
                    *gpuScratch = std::move(scratchImage);

                    PipelineResult result;
                    result.hr = S_OK;
                    result.scratchImage = std::move(gpuScratch);
                    result.imageSize = { static_cast<float>(metadata.width), static_cast<float>(metadata.height) };
                    return result;
                }

                auto convertedScratch = std::make_unique<DirectX::ScratchImage>();
                if (loadedImage && !DirectX::IsCompressed(loadedImage->format) && loadedImage->format == DXGI_FORMAT_B8G8R8A8_UNORM)
                {
                    // Avoid unnecessary Convert/copy when DDS is already BGRA8.
                    *convertedScratch = std::move(scratchImage);
                }
                else
                {
                    hr = ConvertScratchImageToBGRA8(scratchImage, *convertedScratch);
                    if (FAILED(hr))
                    {
                        return PipelineResult(hr);
                    }
                }

                const DirectX::Image* image = convertedScratch->GetImage(0, 0, 0);
                if (!image)
                {
                    return PipelineResult(E_FAIL);
                }

                PipelineResult result;
                result.hr = S_OK;
                result.scratchImage = std::move(convertedScratch);
                result.imageSize = { static_cast<float>(image->width), static_cast<float>(image->height) };
                return result;
            }
        };

        // ---------- Built-in factories ----------
        class WICDecoderFactory final : public IImageDecoderFactory
        {
        public:
            std::wstring_view Id() const override { return L"builtin.wic"; }
            std::wstring_view DisplayName() const override { return L"WIC Decoder"; }
            int Priority() const override { return 0; }

            std::span<const std::wstring_view> SupportedExtensions() const override
            {
                static constexpr std::wstring_view s_exts[] =
                {
                    L".jpg", L".jpeg", L".jfif",
                    L".png",
                    L".bmp",
                    L".gif",
                    L".tif", L".tiff",
                    L".ico",
                    L".heif", L".heic",
                    L".webp",
                };
                return std::span<const std::wstring_view>(s_exts, std::size(s_exts));
            }

            bool Probe(std::span<const uint8_t> header, std::wstring_view extensionLower) const override
            {
                UNREFERENCED_PARAMETER(extensionLower);
                const ImageFormatInfo info = ImageFormatDetector::DetectByMagic(header);
                switch (info.format)
                {
                case ImageFormat::PNG:
                case ImageFormat::JPEG:
                case ImageFormat::BMP:
                case ImageFormat::GIF:
                case ImageFormat::TIFF:
                case ImageFormat::WEBP:
                    return true;
                default:
                    // Unknown이면 확장자 기반 후보로는 남겨둔다 (Decode 실패 시 다른 디코더 시도)
                    return true;
                }
            }

            bool SupportsPurpose(ImagePurpose purpose) const override
            {
                switch (purpose)
                {
                case ImagePurpose::Thumbnail:
                case ImagePurpose::Preview:
                case ImagePurpose::FullResolution:
                    return true;
                default:
                    return false;
                }
            }

            std::unique_ptr<IImageDecoder> Create(ImagePurpose purpose) const override
            {
                switch (purpose)
                {
                case ImagePurpose::Thumbnail:
                case ImagePurpose::Preview:
                    return std::make_unique<WICThumbnailDecoder>();
                case ImagePurpose::FullResolution:
                    return std::make_unique<WICFullImageDecoder>();
                default:
                    return nullptr;
                }
            }
        };

        class DXTexDecoderFactory final : public IImageDecoderFactory
        {
        public:
            std::wstring_view Id() const override { return L"builtin.dxtex"; }
            std::wstring_view DisplayName() const override { return L"DirectXTex Decoder"; }
            int Priority() const override { return 0; }

            std::span<const std::wstring_view> SupportedExtensions() const override
            {
                static constexpr std::wstring_view s_exts[] =
                {
                    L".dds",
                    L".hdr",
                    L".tga",
                    L".exr",
                };
                return std::span<const std::wstring_view>(s_exts, std::size(s_exts));
            }

            bool Probe(std::span<const uint8_t> header, std::wstring_view extensionLower) const override
            {
                // Magic number가 강하게 DXTex 포맷을 가리키면 우선 후보로 남긴다.
                const ImageFormatInfo info = ImageFormatDetector::DetectByMagic(header);
                switch (info.format)
                {
                case ImageFormat::DDS:
                case ImageFormat::KTX2:
                case ImageFormat::EXR:
                case ImageFormat::HDR:
                    return true;
                default:
                    break;
                }

                // Magic이 Unknown이면 확장자 기반으로만 후보에 남긴다.
                // (TGA/HDR 같은 케이스)
                UNREFERENCED_PARAMETER(extensionLower);
                return true;
            }

            bool SupportsPurpose(ImagePurpose purpose) const override
            {
                switch (purpose)
                {
                case ImagePurpose::Thumbnail:
                case ImagePurpose::Preview:
                case ImagePurpose::FullResolution:
                    return true;
                default:
                    return false;
                }
            }

            std::unique_ptr<IImageDecoder> Create(ImagePurpose purpose) const override
            {
                switch (purpose)
                {
                case ImagePurpose::Thumbnail:
                case ImagePurpose::Preview:
                    return std::make_unique<DXTexThumbnailDecoder>();
                case ImagePurpose::FullResolution:
                    return std::make_unique<DXTexFullImageDecoder>();
                default:
                    return nullptr;
                }
            }
        };
    }

    void RegisterBuiltInDecoders()
    {
        static std::once_flag once;
        std::call_once(once, []()
        {
            auto& reg = DecoderRegistry::Instance();
            reg.RegisterFactory(std::make_shared<WICDecoderFactory>());
            reg.RegisterFactory(std::make_shared<DXTexDecoderFactory>());
        });
    }

    PipelineResult ImageDecodeDispatcher::Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input)
    {
        if (request.source.empty())
        {
            return PipelineResult(E_INVALIDARG);
        }

        // safe even if app already called it
        RegisterBuiltInDecoders();

        // Prefer explicit header bytes if available (avoids extra disk I/O in the dispatcher).
        // Otherwise fall back to ProbeFile(path).
        static ImageFormatDetector s_detector;
        FormatProbe probe {};
        if (!input.header.empty())
        {
            probe.header.assign(input.header.begin(), input.header.end());
            probe.extensionLower = GetLowerExtension(request.source);
            probe.info = ImageFormatDetector::DetectByMagic(probe.header);
            if (probe.info.format == ImageFormat::Unknown && !probe.extensionLower.empty())
            {
                probe.info = ImageFormatDetector::DetectByExtension(probe.extensionLower);
            }
        }
        else
        {
            probe = s_detector.ProbeFile(request.source);
        }

        HRESULT lastHr = E_FAIL;
        auto factories = DecoderRegistry::Instance().GetCandidateFactories(request, probe.header);

        for (const auto& factory : factories)
        {
            if (!factory)
            {
                continue;
            }

            auto decoder = factory->Create(request.purpose);
            if (!decoder)
            {
                continue;
            }

            PipelineResult result = decoder->Decode(request, wicFactory, input);
            lastHr = result.hr;
            if (SUCCEEDED(result.hr))
            {
                return result;
            }
        }

        return PipelineResult(lastHr);
    }
}


