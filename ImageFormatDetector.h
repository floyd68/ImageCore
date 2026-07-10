#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace ImageCore
{
    enum class ImageFormat
    {
        Unknown,
        PNG,
        JPEG,
        BMP,
        GIF,
        TIFF,
        DDS,
        KTX2,
        WEBP,
        EXR,
        HDR,
        TGA,
        ZIP,
        SEVEN_Z,
    };

    struct ImageFormatInfo
    {
        ImageFormat format { ImageFormat::Unknown };
        bool isContainer { false };
        bool isGPUCompressed { false };
        bool supportsStreaming { false };
    };

    struct FormatProbe
    {
        ImageFormatInfo info {};
        std::wstring extensionLower {};         // Lowercase extension like ".dds" (empty string if none)
        std::vector<uint8_t> header {};         // Up to kProbeSize bytes
    };

    class ImageFormatDetector final
    {
    public:
        static constexpr size_t kProbeSize = 64;

        // Reads header from path, detects by magic -> extension order
        // (includes cache: based on last_write_time)
        FormatProbe ProbeFile(const std::wstring& path);

        static ImageFormatInfo DetectByMagic(std::span<const uint8_t> header);
        static ImageFormatInfo DetectByExtension(const std::wstring& extensionLower);

        // Shared helper: returns the lowercase extension of a path (e.g. L".dds"),
        // or an empty string when the path has no extension.
        static std::wstring GetLowerExtension(const std::wstring& path);

    private:
        struct CacheEntry
        {
            std::filesystem::file_time_type writeTime {};
            FormatProbe probe {};
        };

        std::mutex m_mutex {};
        std::unordered_map<std::wstring, CacheEntry> m_cache {};

        static std::vector<uint8_t> ReadHeader(const std::wstring& path, size_t maxBytes);
        static bool StartsWith(std::span<const uint8_t> header, std::span<const uint8_t> magic);
    };
}


