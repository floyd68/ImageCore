#include "TestHarness.h"

#include "DecoderRegistry.h"
#include "IImageDecoderFactory.h"
#include "ImageCore.h"
#include "ImageDecodeDispatcher.h"
#include "ImageFormatDetector.h"

#include <DirectXTex.h>
#include <objbase.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Decoder routing and the thread-resource query (ImageCore #3).
//
// Every decode runs on a fresh COM-initialized thread, so
// ImageDecodeDispatcher::HasThreadResources() tells whether the decode went
// through WIC (the only user of ImageCore's per-thread WIC factory) or not.
// In-memory requests carry bytes + header and name a source that does not
// exist (the ThumbnailProvider pattern) or a decoy file with other content;
// path-only requests read a temp file. Fixtures are generated in the test.

namespace
{
    using namespace ImageCore;

    constexpr size_t kWidth = 40;
    constexpr size_t kHeight = 20;
    constexpr float kThumbTarget = 16.0f;

    enum class Format
    {
        Jpeg,
        Png,
        Dds,
    };

    const wchar_t* Extension(Format format)
    {
        switch (format)
        {
        case Format::Jpeg: return L".jpg";
        case Format::Png: return L".png";
        default: return L".dds";
        }
    }

    const char* Name(Format format)
    {
        switch (format)
        {
        case Format::Jpeg: return "JPEG";
        case Format::Png: return "PNG";
        default: return "DDS";
        }
    }

    // DirectXTex (fixture encoding, BGRA8 resize) uses its process-global WIC
    // factory, so COM stays initialized on the main thread for the process.
    void EnsureProcessCom()
    {
        static const HRESULT s_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IC_REQUIRE(SUCCEEDED(s_hrCom));
    }

    // An opaque width x height BGRA8 gradient encoded as `format`.
    std::vector<uint8_t> Encode(Format format, size_t width, size_t height)
    {
        EnsureProcessCom();
        DirectX::ScratchImage scratch;
        IC_REQUIRE(SUCCEEDED(scratch.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1)));
        const DirectX::Image* image = scratch.GetImage(0, 0, 0);
        IC_REQUIRE(image != nullptr);
        for (size_t y = 0; y < height; ++y)
        {
            uint8_t* row = image->pixels + y * image->rowPitch;
            for (size_t x = 0; x < width; ++x)
            {
                row[x * 4 + 0] = static_cast<uint8_t>(x * 6);
                row[x * 4 + 1] = static_cast<uint8_t>(y * 12);
                row[x * 4 + 2] = 0x80;
                row[x * 4 + 3] = 0xFF;
            }
        }

        DirectX::Blob blob;
        if (format == Format::Dds)
        {
            IC_REQUIRE(SUCCEEDED(DirectX::SaveToDDSMemory(*image, DirectX::DDS_FLAGS_NONE, blob)));
        }
        else
        {
            const auto codec = (format == Format::Png) ? DirectX::WIC_CODEC_PNG : DirectX::WIC_CODEC_JPEG;
            IC_REQUIRE(SUCCEEDED(DirectX::SaveToWICMemory(*image, DirectX::WIC_FLAGS_NONE,
                DirectX::GetWICCodec(codec), blob)));
        }
        const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + blob.GetBufferSize());
    }

    // "DDS " magic followed by a header DirectXTex rejects (size field 0).
    std::vector<uint8_t> CorruptDds()
    {
        std::vector<uint8_t> bytes(4 + 124 + 256, 0);
        std::memcpy(bytes.data(), "DDS ", 4);
        return bytes;
    }

    std::filesystem::path TempDir()
    {
        static const std::filesystem::path s_dir = []()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            const auto dir = std::filesystem::path(tempPath) /
                (L"ImageCoreRoutingTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);
            ImageCoreTest::RegisterTeardown([dir]()
            {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            });
            return dir;
        }();
        return s_dir;
    }

    std::wstring WriteFile(const std::wstring& name, const std::vector<uint8_t>& bytes)
    {
        const auto path = TempDir() / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        IC_REQUIRE(out.good());
        return path.wstring();
    }

    struct Outcome
    {
        HRESULT hr { E_FAIL };
        uint32_t width { 0 };
        uint32_t height { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        bool wicFactoryBefore { true };
        bool wicFactoryAfter { false };
        std::vector<uint8_t> pixels {};
    };

    // Runs `body` on a new COM (STA) thread that releases ImageCore's thread
    // resources before CoUninitialize, and rethrows a failed IC_REQUIRE.
    void RunOnComThread(const std::function<void()>& body)
    {
        EnsureProcessCom();
        bool requireFailed = false;
        std::thread worker([&]()
        {
            const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try
            {
                IC_REQUIRE(SUCCEEDED(hrCom));
                body();
            }
            catch (const ImageCoreTest::RequireFailed&)
            {
                requireFailed = true;
            }
            catch (const std::exception& ex)
            {
                ImageCoreTest::ReportFailure(__FILE__, __LINE__, ex.what());
            }
            ImageDecodeDispatcher::ReleaseThreadResources();
            if (SUCCEEDED(hrCom))
            {
                CoUninitialize();
            }
        });
        worker.join();
        if (requireFailed)
        {
            throw ImageCoreTest::RequireFailed {};
        }
    }

    // Decodes on a fresh thread. `bytes` empty means a path-only request;
    // `withHeader` false leaves the optional header of an in-memory request
    // empty.
    Outcome Decode(const std::wstring& source, ImagePurpose purpose, const std::vector<uint8_t>& bytes,
        bool withHeader = true)
    {
        RegisterBuiltInDecoders();
        ImageRequest request(source, purpose);
        request.allowGpuCompressedDDS = false;
        if (purpose == ImagePurpose::Thumbnail)
        {
            request.targetSize = { kThumbTarget, kThumbTarget };
        }

        DecodeInput input {};
        if (!bytes.empty())
        {
            input.bytes = std::span<const uint8_t>(bytes.data(), bytes.size());
            if (withHeader)
            {
                input.header = std::span<const uint8_t>(bytes.data(), (std::min)(bytes.size(), ImageFormatDetector::kProbeSize));
            }
        }

        Outcome outcome {};
        RunOnComThread([&]()
        {
            outcome.wicFactoryBefore = ImageDecodeDispatcher::HasThreadResources();
            ImageDecodeDispatcher dispatcher;
            PipelineResult result = dispatcher.Decode(request, input);
            outcome.wicFactoryAfter = ImageDecodeDispatcher::HasThreadResources();
            outcome.hr = result.hr;
            if (SUCCEEDED(result.hr) && result.image.blocks)
            {
                outcome.width = result.image.width;
                outcome.height = result.image.height;
                outcome.format = result.image.dxgiFormat;
                outcome.pixels = *result.image.blocks;
            }
        });
        return outcome;
    }

    std::vector<std::wstring> CandidateIds(const std::wstring& source, std::span<const uint8_t> header)
    {
        RegisterBuiltInDecoders();
        std::vector<std::wstring> ids;
        for (const auto& factory : DecoderRegistry::Instance().GetCandidateFactories(
                 ImageRequest(source, ImagePurpose::Thumbnail), header))
        {
            ids.emplace_back(factory->Id());
        }
        return ids;
    }

    bool Contains(const std::vector<std::wstring>& ids, const wchar_t* id)
    {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
}

IC_TEST(DecodeRouting_ThreadResourceQueryTracksTheWicFactory)
{
    const std::vector<uint8_t> png = Encode(Format::Png, kWidth, kHeight);
    RunOnComThread([&]()
    {
        // No decode on this thread yet, and asking does not create anything.
        IC_CHECK(!ImageDecodeDispatcher::HasThreadResources());
        IC_CHECK(!ImageDecodeDispatcher::HasThreadResources());

        RegisterBuiltInDecoders();
        ImageRequest request(L"query.png", ImagePurpose::FullResolution);
        DecodeInput input {};
        input.bytes = std::span<const uint8_t>(png.data(), png.size());
        input.header = std::span<const uint8_t>(png.data(), (std::min)(png.size(), ImageFormatDetector::kProbeSize));
        ImageDecodeDispatcher dispatcher;
        IC_REQUIRE(SUCCEEDED(dispatcher.Decode(request, input).hr));
        IC_CHECK(ImageDecodeDispatcher::HasThreadResources());

        ImageDecodeDispatcher::ReleaseThreadResources();
        IC_CHECK(!ImageDecodeDispatcher::HasThreadResources());
    });
}

IC_TEST(DecodeRouting_WicProbeRejectsMagicOwnedByDirectXTex)
{
    const std::vector<uint8_t> dds = Encode(Format::Dds, kWidth, kHeight);
    const std::vector<uint8_t> corrupt = CorruptDds();
    static constexpr uint8_t kHdr[] = { '#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E', '\n' };
    static constexpr uint8_t kExr[] = { 0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00 };

    // DDS magic: DirectXTex only, whatever the extension says.
    const std::vector<std::wstring> ddsIds = CandidateIds(L"x.dds", dds);
    IC_CHECK(Contains(ddsIds, L"builtin.dxtex"));
    IC_CHECK(!Contains(ddsIds, L"builtin.wic"));
    IC_CHECK(!Contains(CandidateIds(L"x.dds", corrupt), L"builtin.wic"));
    IC_CHECK(!Contains(CandidateIds(L"x.png", dds), L"builtin.wic"));
    IC_CHECK(!Contains(CandidateIds(L"x.hdr", kHdr), L"builtin.wic"));
    IC_CHECK(!Contains(CandidateIds(L"x.exr", kExr), L"builtin.wic"));

    // WIC formats and unknown magic keep WIC as a candidate.
    const std::vector<uint8_t> png = Encode(Format::Png, kWidth, kHeight);
    IC_CHECK(Contains(CandidateIds(L"x.png", png), L"builtin.wic"));
    IC_CHECK(Contains(CandidateIds(L"x.dds", png), L"builtin.wic"));
    IC_CHECK(Contains(CandidateIds(L"x.jpg", {}), L"builtin.wic"));
}

IC_TEST(DecodeRouting_CorruptDdsFailsWithoutWic)
{
    const std::vector<uint8_t> corrupt = CorruptDds();
    const std::wstring missing = (TempDir() / L"missing_corrupt.dds").wstring();
    const std::wstring onDisk = WriteFile(L"corrupt.dds", corrupt);

    for (const ImagePurpose purpose : { ImagePurpose::Thumbnail, ImagePurpose::FullResolution })
    {
        const Outcome inMemory = Decode(missing, purpose, corrupt);
        IC_CHECK(FAILED(inMemory.hr));
        IC_CHECK(!inMemory.wicFactoryAfter);

        const Outcome pathOnly = Decode(onDisk, purpose, {});
        IC_CHECK(FAILED(pathOnly.hr));
        IC_CHECK(!pathOnly.wicFactoryAfter);
    }
}

IC_TEST(DecodeRouting_InMemoryAndPathOnlyRequests)
{
    for (const Format format : { Format::Jpeg, Format::Png, Format::Dds })
    {
        const std::wstring ext = Extension(format);
        const std::vector<uint8_t> bytes = Encode(format, kWidth, kHeight);
        // Portrait decoy: decoding it instead of the bytes changes the size.
        const std::wstring decoy = WriteFile(L"decoy" + ext, Encode(format, kHeight, kWidth));
        const std::wstring onDisk = WriteFile(L"image" + ext, bytes);
        const std::wstring missing = (TempDir() / (L"missing" + ext)).wstring();
        IC_REQUIRE(!std::filesystem::exists(missing));
        const bool viaWic = (format != Format::Dds);

        for (const ImagePurpose purpose : { ImagePurpose::Thumbnail, ImagePurpose::FullResolution })
        {
            const bool thumb = (purpose == ImagePurpose::Thumbnail);
            const uint32_t expectedWidth = thumb ? static_cast<uint32_t>(kThumbTarget) : static_cast<uint32_t>(kWidth);
            const uint32_t expectedHeight = thumb ? static_cast<uint32_t>(kThumbTarget / 2) : static_cast<uint32_t>(kHeight);
            std::printf("  %s %s\n", Name(format), thumb ? "Thumbnail" : "FullResolution");

            const Outcome inMemory = Decode(missing, purpose, bytes);
            const Outcome inMemoryDecoy = Decode(decoy, purpose, bytes);
            const Outcome pathOnly = Decode(onDisk, purpose, {});
            for (const Outcome* outcome : { &inMemory, &inMemoryDecoy, &pathOnly })
            {
                if (FAILED(outcome->hr))
                {
                    std::fprintf(stderr, "  decode failed: hr=0x%08lX\n", static_cast<unsigned long>(outcome->hr));
                }
                IC_CHECK(SUCCEEDED(outcome->hr));
                IC_CHECK(outcome->width == expectedWidth && outcome->height == expectedHeight);
                IC_CHECK(outcome->format == DXGI_FORMAT_B8G8R8A8_UNORM);
                IC_CHECK(!outcome->wicFactoryBefore);
                // JPEG/PNG go through WIC; DDS through DirectXTex only.
                IC_CHECK(outcome->wicFactoryAfter == viaWic);
            }

            // Same bytes, same decoder: identical pixels from memory and disk.
            IC_CHECK(!inMemory.pixels.empty() && inMemory.pixels == pathOnly.pixels);
            IC_CHECK(inMemoryDecoy.pixels == inMemory.pixels);
        }
    }
}

// A valid DDS saved under another supported extension still decodes: the
// DirectXTex loader follows the detected format, not the extension, for
// path-only and in-memory input, and WIC (excluded by the DDS magic) is not
// involved.
IC_TEST(DecodeRouting_ValidDdsWithAMismatchingExtensionDecodes)
{
    const std::vector<uint8_t> dds = Encode(Format::Dds, kWidth, kHeight);
    const Outcome reference = Decode(WriteFile(L"reference.dds", dds), ImagePurpose::FullResolution, {});
    IC_REQUIRE(SUCCEEDED(reference.hr));

    for (const wchar_t* ext : { L".png", L".jpg", L".tga" })
    {
        const std::wstring onDisk = WriteFile(std::wstring(L"renamed") + ext, dds);
        const std::wstring missing = (TempDir() / (std::wstring(L"missing_renamed") + ext)).wstring();
        IC_REQUIRE(!std::filesystem::exists(missing));
        for (const ImagePurpose purpose : { ImagePurpose::Thumbnail, ImagePurpose::FullResolution })
        {
            const bool thumb = (purpose == ImagePurpose::Thumbnail);
            const uint32_t expectedWidth = thumb ? static_cast<uint32_t>(kThumbTarget) : static_cast<uint32_t>(kWidth);
            const uint32_t expectedHeight = thumb ? static_cast<uint32_t>(kThumbTarget / 2) : static_cast<uint32_t>(kHeight);
            const Outcome pathOnly = Decode(onDisk, purpose, {});
            const Outcome inMemory = Decode(missing, purpose, dds);
            for (const Outcome* outcome : { &pathOnly, &inMemory })
            {
                if (FAILED(outcome->hr))
                {
                    std::fprintf(stderr, "  %ls decode failed: hr=0x%08lX\n", ext, static_cast<unsigned long>(outcome->hr));
                }
                IC_CHECK(SUCCEEDED(outcome->hr));
                IC_CHECK(outcome->width == expectedWidth && outcome->height == expectedHeight);
                IC_CHECK(!outcome->wicFactoryAfter);
            }
            IC_CHECK(pathOnly.pixels == inMemory.pixels);
            if (!thumb)
            {
                IC_CHECK(pathOnly.pixels == reference.pixels);
            }
        }
    }
}

// In-memory bytes without the optional header are routed by the bytes, never
// by the file at the source path: decoys of another format there (an HDR
// signature, a PNG, a DDS) change nothing.
IC_TEST(DecodeRouting_BytesWithoutHeaderAreRoutedByTheBytes)
{
    static constexpr uint8_t kHdr[] = { '#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E', '\n' };
    const std::vector<uint8_t> dds = Encode(Format::Dds, kWidth, kHeight);
    const std::vector<uint8_t> png = Encode(Format::Png, kWidth, kHeight);
    const std::wstring hdrDecoy = WriteFile(L"hdr_decoy.dds", std::vector<uint8_t>(std::begin(kHdr), std::end(kHdr)));
    const std::wstring pngDecoy = WriteFile(L"png_decoy.dds", Encode(Format::Png, kHeight, kWidth));
    const std::wstring ddsDecoy = WriteFile(L"dds_decoy.png", Encode(Format::Dds, kHeight, kWidth));

    for (const ImagePurpose purpose : { ImagePurpose::Thumbnail, ImagePurpose::FullResolution })
    {
        const bool thumb = (purpose == ImagePurpose::Thumbnail);
        const uint32_t expectedWidth = thumb ? static_cast<uint32_t>(kThumbTarget) : static_cast<uint32_t>(kWidth);
        const uint32_t expectedHeight = thumb ? static_cast<uint32_t>(kThumbTarget / 2) : static_cast<uint32_t>(kHeight);

        struct Case
        {
            const std::wstring* source;
            const std::vector<uint8_t>* bytes;
            bool viaWic;
        };
        for (const Case& c : { Case { &hdrDecoy, &dds, false }, Case { &pngDecoy, &dds, false },
                 Case { &ddsDecoy, &png, true } })
        {
            const Outcome withHeader = Decode(*c.source, purpose, *c.bytes);
            const Outcome bytesOnly = Decode(*c.source, purpose, *c.bytes, false);
            if (FAILED(bytesOnly.hr))
            {
                std::fprintf(stderr, "  bytes-only decode failed: hr=0x%08lX\n", static_cast<unsigned long>(bytesOnly.hr));
            }
            IC_CHECK(SUCCEEDED(withHeader.hr));
            IC_CHECK(SUCCEEDED(bytesOnly.hr));
            IC_CHECK(bytesOnly.width == expectedWidth && bytesOnly.height == expectedHeight);
            IC_CHECK(bytesOnly.wicFactoryAfter == c.viaWic);
            IC_CHECK(!bytesOnly.pixels.empty() && bytesOnly.pixels == withHeader.pixels);
        }
    }
}
