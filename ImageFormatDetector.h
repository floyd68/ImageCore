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
        std::wstring extensionLower {};         // ".dds" 같은 소문자 확장자(없으면 "")
        std::vector<uint8_t> header {};         // 최대 kProbeSize bytes
    };

    class ImageFormatDetector final
    {
    public:
        static constexpr size_t kProbeSize = 64;

        // path에서 헤더를 읽고, magic->extension 순으로 판별
        // (캐시 포함: last_write_time 기반)
        FormatProbe ProbeFile(const std::wstring& path);

        static ImageFormatInfo DetectByMagic(std::span<const uint8_t> header);
        static ImageFormatInfo DetectByExtension(const std::wstring& extensionLower);

    private:
        struct CacheEntry
        {
            std::filesystem::file_time_type writeTime {};
            FormatProbe probe {};
        };

        std::mutex m_mutex {};
        std::unordered_map<std::wstring, CacheEntry> m_cache {};

        static std::wstring GetLowerExtension(const std::wstring& path);
        static std::vector<uint8_t> ReadHeader(const std::wstring& path, size_t maxBytes);
        static bool StartsWith(std::span<const uint8_t> header, std::span<const uint8_t> magic);
    };
}


