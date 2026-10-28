#include "TestHarness.h"

#include "ImageCore.h"
#include "ImageDecodeDispatcher.h"

#include <DirectXTex.h>
#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

// Mip selection for DDS thumbnail requests (ImageCore #18). A thumbnail
// request with a target size decodes the smallest mip that still reaches the
// target; when no mip reaches it (the source is smaller than the target), it
// decodes mip 0, never the tiny last mip. Generated in the test, no fixture.

namespace
{
    using namespace ImageCore;

    constexpr size_t kWidth = 64;
    constexpr size_t kHeight = 32;

    // A kWidth x kHeight BGRA8 DDS, with a full mip chain (64x32 down to 1x1,
    // 7 levels) unless `singleMip`.
    std::vector<uint8_t> MakeDds(bool singleMip)
    {
        DirectX::ScratchImage base;
        IC_REQUIRE(SUCCEEDED(base.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, 1, 1)));
        const DirectX::Image* image = base.GetImage(0, 0, 0);
        IC_REQUIRE(image != nullptr);
        for (size_t y = 0; y < kHeight; ++y)
        {
            uint8_t* row = image->pixels + y * image->rowPitch;
            for (size_t x = 0; x < kWidth; ++x)
            {
                row[x * 4 + 0] = static_cast<uint8_t>(x * 4);
                row[x * 4 + 1] = static_cast<uint8_t>(y * 8);
                row[x * 4 + 2] = 0x80;
                row[x * 4 + 3] = 0xFF;
            }
        }

        DirectX::ScratchImage mips;
        const DirectX::ScratchImage* source = &base;
        if (!singleMip)
        {
            IC_REQUIRE(SUCCEEDED(DirectX::GenerateMipMaps(*image,
                DirectX::TEX_FILTER_BOX | DirectX::TEX_FILTER_FORCE_NON_WIC, 0, mips)));
            IC_REQUIRE(mips.GetMetadata().mipLevels == 7);
            source = &mips;
        }

        DirectX::Blob blob;
        IC_REQUIRE(SUCCEEDED(DirectX::SaveToDDSMemory(source->GetImages(), source->GetImageCount(),
            source->GetMetadata(), DirectX::DDS_FLAGS_NONE, blob)));
        const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + blob.GetBufferSize());
    }

    struct Decoded
    {
        uint32_t mip { 0 };
        uint32_t width { 0 };
        uint32_t height { 0 };
    };

    // Decodes a thumbnail request for `target` x `target` (explicit `mipLevel`
    // when nonzero) to CPU BGRA8.
    Decoded DecodeThumbnail(const std::vector<uint8_t>& dds, float target, uint32_t mipLevel = 0)
    {
        RegisterBuiltInDecoders();
        ImageRequest request(L"mip_selection.dds", ImagePurpose::Thumbnail);
        request.allowGpuCompressedDDS = false;
        request.targetSize = { target, target };
        request.mipLevel = mipLevel;

        DecodeInput input {};
        input.bytes = std::span<const uint8_t>(dds.data(), dds.size());
        input.header = std::span<const uint8_t>(dds.data(), (std::min)(dds.size(), size_t { 512 }));

        // As in SrgbConversionTests: DirectXTex may use WIC, whose factory it
        // caches process-wide, so COM stays initialized.
        static const HRESULT s_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IC_REQUIRE(SUCCEEDED(s_hrCom));
        ImageDecodeDispatcher dispatcher;
        PipelineResult result = dispatcher.Decode(request, input);
        if (FAILED(result.hr))
        {
            std::fprintf(stderr, "  decode failed: hr=0x%08lX\n", static_cast<unsigned long>(result.hr));
        }
        IC_REQUIRE(SUCCEEDED(result.hr));
        const DecodedImage& image = result.image;
        std::printf("  target %.0f, mipLevel %u: mip %u, %ux%u\n", target, mipLevel, image.sourceMipIndex,
            image.width, image.height);
        return { image.sourceMipIndex, image.width, image.height };
    }
}

IC_TEST(DdsMip_TargetLargerThanSourceDecodesMip0)
{
    // No mip reaches 256: mip 0 at source size (the fit never upscales),
    // not the 1x1 last mip.
    const Decoded d = DecodeThumbnail(MakeDds(false), 256.0f);
    IC_CHECK(d.mip == 0);
    IC_CHECK(d.width == 64 && d.height == 32);
}

IC_TEST(DdsMip_SmallerTargetPicksTheSmallestMipThatReachesIt)
{
    const std::vector<uint8_t> dds = MakeDds(false);

    // 16 is reached by the 16x8 mip (mip 2) and not by the 8x4 one.
    const Decoded d16 = DecodeThumbnail(dds, 16.0f);
    IC_CHECK(d16.mip == 2);
    IC_CHECK(d16.width == 16 && d16.height == 8);

    // A target equal to a mip's max dimension picks that mip (32x16, mip 1).
    const Decoded d32 = DecodeThumbnail(dds, 32.0f);
    IC_CHECK(d32.mip == 1);
    IC_CHECK(d32.width == 32 && d32.height == 16);

    // Between two mips: the larger one (mip 1), fitted down to the target.
    const Decoded d20 = DecodeThumbnail(dds, 20.0f);
    IC_CHECK(d20.mip == 1);
    IC_CHECK(d20.width <= 20 && d20.height <= 20);

    // The full source size picks mip 0.
    const Decoded d64 = DecodeThumbnail(dds, 64.0f);
    IC_CHECK(d64.mip == 0);
    IC_CHECK(d64.width == 64 && d64.height == 32);
}

IC_TEST(DdsMip_SingleMipAndExplicitMipLevelAreUnaffected)
{
    const Decoded single = DecodeThumbnail(MakeDds(true), 256.0f);
    IC_CHECK(single.mip == 0);
    IC_CHECK(single.width == 64 && single.height == 32);

    // An explicit mip level is decoded as requested, whatever the target.
    const std::vector<uint8_t> dds = MakeDds(false);
    const Decoded explicitLarge = DecodeThumbnail(dds, 256.0f, 2);
    IC_CHECK(explicitLarge.mip == 2);
    IC_CHECK(explicitLarge.width == 16 && explicitLarge.height == 8);
    const Decoded explicitSmall = DecodeThumbnail(dds, 4.0f, 1);
    IC_CHECK(explicitSmall.mip == 1);
}
