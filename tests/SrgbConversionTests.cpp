#include "TestHarness.h"

#include "ImageCore.h"
#include "ImageDecodeDispatcher.h"

#include <DirectXTex.h>
#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// BGRA8 conversion of sRGB-encoded sources (ImageCore #17).
//
// Each case builds a DDS twin pair with identical texel bytes, one in an _SRGB
// format and one in its _UNORM twin, decodes both through the dispatcher to
// CPU BGRA8 (full resolution and thumbnail), and checks that the two outputs
// match each other and the stored (or block-decoded) texel values, alpha
// included. sRGB-encoded values must not be linearized on the way.

namespace
{
    using namespace ImageCore;

    constexpr size_t kSize = 8;  // two BC blocks per side
    constexpr int kTolerance = 1;

    // Straight RGBA8 texels: mid-tones (where sRGB-to-linear differs most,
    // e.g. 0x80 -> 0x37), a varying alpha unless `opaque`.
    std::vector<uint8_t> MakeRgbaTexels(bool opaque)
    {
        std::vector<uint8_t> texels(kSize * kSize * 4);
        for (size_t y = 0; y < kSize; ++y)
        {
            for (size_t x = 0; x < kSize; ++x)
            {
                uint8_t* p = &texels[(y * kSize + x) * 4];
                p[0] = static_cast<uint8_t>(0x20 + x * 0x18);
                p[1] = 0x80;
                p[2] = static_cast<uint8_t>(0xE0 - y * 0x18);
                p[3] = opaque ? 0xFF : static_cast<uint8_t>(0x30 + (x + y) * 0x10);
            }
        }
        return texels;
    }

    std::vector<uint8_t> SwapRedBlue(std::vector<uint8_t> texels)
    {
        for (size_t i = 0; i + 3 < texels.size(); i += 4)
        {
            std::swap(texels[i], texels[i + 2]);
        }
        return texels;
    }

    DirectX::Image ImageOver(std::vector<uint8_t>& texels, DXGI_FORMAT format)
    {
        DirectX::Image image {};
        image.width = kSize;
        image.height = kSize;
        image.format = format;
        IC_REQUIRE(SUCCEEDED(DirectX::ComputePitch(format, kSize, kSize, image.rowPitch, image.slicePitch)));
        IC_REQUIRE(texels.size() >= image.slicePitch);
        image.pixels = texels.data();
        return image;
    }

    std::vector<uint8_t> SaveDds(const DirectX::Image& image)
    {
        DirectX::Blob blob;
        IC_REQUIRE(SUCCEEDED(DirectX::SaveToDDSMemory(image, DirectX::DDS_FLAGS_NONE, blob)));
        const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + blob.GetBufferSize());
    }

    // Decodes the DDS bytes to CPU BGRA8 (no BCn passthrough) and returns the
    // tightly packed pixels.
    std::vector<uint8_t> DecodeToBgra8(const std::vector<uint8_t>& dds, ImagePurpose purpose)
    {
        RegisterBuiltInDecoders();
        ImageRequest request(L"srgb_twin.dds", purpose);
        request.allowGpuCompressedDDS = false;
        if (purpose != ImagePurpose::FullResolution)
        {
            // Larger than the image: no mip selection or resize.
            request.targetSize = { 64.0f, 64.0f };
        }

        DecodeInput input {};
        input.bytes = std::span<const uint8_t>(dds.data(), dds.size());
        input.header = std::span<const uint8_t>(dds.data(), (std::min)(dds.size(), size_t { 512 }));

        // DirectXTex conversions may go through WIC, as on the decode threads.
        // COM stays initialized for the rest of the process: DirectXTex caches
        // its WIC factory process-wide.
        static const HRESULT s_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IC_REQUIRE(SUCCEEDED(s_hrCom));
        ImageDecodeDispatcher dispatcher;
        PipelineResult result = dispatcher.Decode(request, nullptr, input);
        if (FAILED(result.hr))
        {
            std::fprintf(stderr, "  decode failed: hr=0x%08lX\n", static_cast<unsigned long>(result.hr));
        }
        IC_REQUIRE(SUCCEEDED(result.hr));
        const DecodedImage& decoded = result.image;
        IC_REQUIRE(decoded.dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
        IC_REQUIRE(decoded.width == kSize && decoded.height == kSize);
        IC_REQUIRE(decoded.blocks != nullptr);
        IC_REQUIRE(decoded.rowPitchBytes >= kSize * 4);
        IC_REQUIRE(decoded.blocks->size() >= decoded.rowPitchBytes * (kSize - 1) + kSize * 4);

        std::vector<uint8_t> pixels(kSize * kSize * 4);
        for (size_t y = 0; y < kSize; ++y)
        {
            std::memcpy(&pixels[y * kSize * 4], decoded.blocks->data() + y * decoded.rowPitchBytes, kSize * 4);
        }
        return pixels;
    }

    // True when every channel (alpha included) is within kTolerance; prints
    // the first mismatch.
    bool NearlyEqual(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected, const char* what)
    {
        if (actual.size() != expected.size())
        {
            std::fprintf(stderr, "  %s: size %zu, expected %zu\n", what, actual.size(), expected.size());
            return false;
        }
        for (size_t i = 0; i < actual.size(); ++i)
        {
            if (std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])) > kTolerance)
            {
                static const char* const kChannel[] = { "B", "G", "R", "A" };
                std::fprintf(stderr, "  %s: texel %zu channel %s is 0x%02X, expected 0x%02X\n",
                    what, i / 4, kChannel[i % 4], actual[i], expected[i]);
                return false;
            }
        }
        return true;
    }

    // Decodes the _UNORM and _SRGB twins of `texels` (stored in `unormFormat`
    // and MakeSRGB(unormFormat)) through both decode purposes and checks both
    // against `expectedBgra`.
    void CheckTwins(std::vector<uint8_t> texels, DXGI_FORMAT unormFormat, const std::vector<uint8_t>& expectedBgra)
    {
        const DXGI_FORMAT srgbFormat = DirectX::MakeSRGB(unormFormat);
        IC_REQUIRE(srgbFormat != unormFormat && DirectX::IsSRGB(srgbFormat));

        const std::vector<uint8_t> unormDds = SaveDds(ImageOver(texels, unormFormat));
        const std::vector<uint8_t> srgbDds = SaveDds(ImageOver(texels, srgbFormat));

        for (const ImagePurpose purpose : { ImagePurpose::FullResolution, ImagePurpose::Thumbnail })
        {
            const std::vector<uint8_t> unorm = DecodeToBgra8(unormDds, purpose);
            const std::vector<uint8_t> srgb = DecodeToBgra8(srgbDds, purpose);
            IC_CHECK(NearlyEqual(unorm, expectedBgra, "_UNORM output vs texels"));
            IC_CHECK(NearlyEqual(srgb, expectedBgra, "_SRGB output vs texels"));
            IC_CHECK(NearlyEqual(srgb, unorm, "_SRGB output vs _UNORM output"));
        }
    }

    // Compresses `rgba` (R8G8B8A8_UNORM) to `bcFormat` (a _UNORM BC format),
    // checks both twins, and expects the block-decoded texels.
    void CheckBlockCompressedTwins(DXGI_FORMAT bcFormat, bool opaque)
    {
        std::vector<uint8_t> rgba = MakeRgbaTexels(opaque);
        DirectX::ScratchImage compressed;
        IC_REQUIRE(SUCCEEDED(DirectX::Compress(ImageOver(rgba, DXGI_FORMAT_R8G8B8A8_UNORM), bcFormat,
            DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, compressed)));
        const DirectX::Image* blocks = compressed.GetImage(0, 0, 0);
        IC_REQUIRE(blocks != nullptr);
        std::vector<uint8_t> blockBytes(blocks->pixels, blocks->pixels + blocks->slicePitch);

        // The oracle: the block-decoded texels of the _UNORM blocks, as BGRA.
        DirectX::ScratchImage decoded;
        IC_REQUIRE(SUCCEEDED(DirectX::Decompress(*blocks, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)));
        const DirectX::Image* decodedImage = decoded.GetImage(0, 0, 0);
        IC_REQUIRE(decodedImage != nullptr && decodedImage->rowPitch == kSize * 4);
        std::vector<uint8_t> expected(decodedImage->pixels, decodedImage->pixels + kSize * kSize * 4);
        if (opaque)
        {
            for (size_t i = 3; i < expected.size(); i += 4)
            {
                IC_REQUIRE(expected[i] == 0xFF);
            }
        }

        CheckTwins(std::move(blockBytes), bcFormat, SwapRedBlue(std::move(expected)));
    }
}

IC_TEST(SrgbConversion_R8G8B8A8KeepsEncodedValues)
{
    std::vector<uint8_t> rgba = MakeRgbaTexels(false);
    CheckTwins(rgba, DXGI_FORMAT_R8G8B8A8_UNORM, SwapRedBlue(rgba));
}

IC_TEST(SrgbConversion_B8G8R8A8KeepsEncodedValues)
{
    // The bytes are stored as BGRA already, so the output is the bytes.
    std::vector<uint8_t> bgra = SwapRedBlue(MakeRgbaTexels(false));
    CheckTwins(bgra, DXGI_FORMAT_B8G8R8A8_UNORM, bgra);
}

IC_TEST(SrgbConversion_B8G8R8X8KeepsEncodedValuesOpaque)
{
    // The X channel holds varying bytes; the output alpha must be opaque.
    std::vector<uint8_t> bgrx = SwapRedBlue(MakeRgbaTexels(false));
    std::vector<uint8_t> expected = bgrx;
    for (size_t i = 3; i < expected.size(); i += 4)
    {
        expected[i] = 0xFF;
    }
    CheckTwins(bgrx, DXGI_FORMAT_B8G8R8X8_UNORM, expected);
}

IC_TEST(SrgbConversion_BC1KeepsBlockDecodedValues)
{
    CheckBlockCompressedTwins(DXGI_FORMAT_BC1_UNORM, /*opaque=*/true);
}

IC_TEST(SrgbConversion_BC3KeepsBlockDecodedValues)
{
    CheckBlockCompressedTwins(DXGI_FORMAT_BC3_UNORM, /*opaque=*/false);
}

IC_TEST(SrgbConversion_BC7KeepsBlockDecodedValues)
{
    CheckBlockCompressedTwins(DXGI_FORMAT_BC7_UNORM, /*opaque=*/false);
}
