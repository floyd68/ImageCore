#include "ImageFormatDetector.h"
#include <algorithm>
#include <fstream>

namespace ImageCore
{
    static ImageFormatInfo MakeInfo(ImageFormat f)
    {
        ImageFormatInfo info {};
        info.format = f;
        switch (f)
        {
        case ImageFormat::DDS:
        case ImageFormat::KTX2:
            info.isGPUCompressed = true;
            info.supportsStreaming = true;
            break;

        case ImageFormat::JPEG:
            info.supportsStreaming = true;
            break;

        case ImageFormat::ZIP:
        case ImageFormat::SEVEN_Z:
            info.isContainer = true;
            break;

        default:
            break;
        }
        return info;
    }

    bool ImageFormatDetector::StartsWith(std::span<const uint8_t> header, std::span<const uint8_t> magic)
    {
        if (header.size() < magic.size())
        {
            return false;
        }
        for (size_t i = 0; i < magic.size(); ++i)
        {
            if (header[i] != magic[i])
            {
                return false;
            }
        }
        return true;
    }

    std::wstring ImageFormatDetector::GetLowerExtension(const std::wstring& path)
    {
        std::filesystem::path p(path);
        std::wstring ext = p.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        return ext;
    }

    std::vector<uint8_t> ImageFormatDetector::ReadHeader(const std::wstring& path, size_t maxBytes)
    {
        std::vector<uint8_t> data;
        data.resize(maxBytes);

        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        if (!f)
        {
            data.clear();
            return data;
        }

        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(maxBytes));
        const auto readCount = static_cast<size_t>(f.gcount());
        data.resize(readCount);
        return data;
    }

    ImageFormatInfo ImageFormatDetector::DetectByMagic(std::span<const uint8_t> header)
    {
        // PNG: 89 50 4E 47 0D 0A 1A 0A
        static constexpr uint8_t kPng[] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
        if (StartsWith(header, kPng))
        {
            return MakeInfo(ImageFormat::PNG);
        }

        // JPEG: FF D8 FF
        static constexpr uint8_t kJpeg[] = { 0xFF,0xD8,0xFF };
        if (StartsWith(header, kJpeg))
        {
            return MakeInfo(ImageFormat::JPEG);
        }

        // BMP: "BM"
        static constexpr uint8_t kBmp[] = { 'B','M' };
        if (StartsWith(header, kBmp))
        {
            return MakeInfo(ImageFormat::BMP);
        }

        // GIF: "GIF8"
        static constexpr uint8_t kGif[] = { 'G','I','F','8' };
        if (StartsWith(header, kGif))
        {
            return MakeInfo(ImageFormat::GIF);
        }

        // TIFF: "II*\0" or "MM\0*"
        static constexpr uint8_t kTiffLe[] = { 'I','I',0x2A,0x00 };
        static constexpr uint8_t kTiffBe[] = { 'M','M',0x00,0x2A };
        if (StartsWith(header, kTiffLe) || StartsWith(header, kTiffBe))
        {
            return MakeInfo(ImageFormat::TIFF);
        }

        // DDS: "DDS "
        static constexpr uint8_t kDds[] = { 'D','D','S',' ' };
        if (StartsWith(header, kDds))
        {
            return MakeInfo(ImageFormat::DDS);
        }

        // KTX2: AB 4B 54 58 20 32 30 BB 0D 0A 1A 0A
        static constexpr uint8_t kKtx2[] = { 0xAB,'K','T','X',' ', '2','0', 0xBB, 0x0D,0x0A,0x1A,0x0A };
        if (StartsWith(header, kKtx2))
        {
            return MakeInfo(ImageFormat::KTX2);
        }

        // WebP: RIFF....WEBP (offset 0 "RIFF", offset 8 "WEBP")
        if (header.size() >= 12 &&
            header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
            header[8] == 'W' && header[9] == 'E' && header[10] == 'B' && header[11] == 'P')
        {
            return MakeInfo(ImageFormat::WEBP);
        }

        // EXR: 76 2F 31 01
        static constexpr uint8_t kExr[] = { 0x76,0x2F,0x31,0x01 };
        if (StartsWith(header, kExr))
        {
            return MakeInfo(ImageFormat::EXR);
        }

        // HDR (Radiance): "#?RADIANCE" or "#?RGBE"
        static constexpr uint8_t kHdr1[] = { '#','?','R','A','D','I','A','N','C','E' };
        static constexpr uint8_t kHdr2[] = { '#','?','R','G','B','E' };
        if (StartsWith(header, kHdr1) || StartsWith(header, kHdr2))
        {
            return MakeInfo(ImageFormat::HDR);
        }

        // ZIP: "PK\003\004" or "PK\005\006" or "PK\007\008"
        if (header.size() >= 4 && header[0] == 'P' && header[1] == 'K' &&
            ((header[2] == 0x03 && header[3] == 0x04) ||
             (header[2] == 0x05 && header[3] == 0x06) ||
             (header[2] == 0x07 && header[3] == 0x08)))
        {
            return MakeInfo(ImageFormat::ZIP);
        }

        // 7z: 37 7A BC AF 27 1C
        static constexpr uint8_t k7z[] = { '7','z',0xBC,0xAF,0x27,0x1C };
        if (StartsWith(header, k7z))
        {
            return MakeInfo(ImageFormat::SEVEN_Z);
        }

        return MakeInfo(ImageFormat::Unknown);
    }

    ImageFormatInfo ImageFormatDetector::DetectByExtension(const std::wstring& extensionLower)
    {
        if (extensionLower == L".dds") return MakeInfo(ImageFormat::DDS);
        if (extensionLower == L".ktx2") return MakeInfo(ImageFormat::KTX2);
        if (extensionLower == L".tga") return MakeInfo(ImageFormat::TGA);
        if (extensionLower == L".hdr") return MakeInfo(ImageFormat::HDR);
        if (extensionLower == L".exr") return MakeInfo(ImageFormat::EXR);
        return MakeInfo(ImageFormat::Unknown);
    }

    FormatProbe ImageFormatDetector::ProbeFile(const std::wstring& path)
    {
        FormatProbe probe {};
        probe.extensionLower = GetLowerExtension(path);

        std::error_code ec;
        const auto wt = std::filesystem::last_write_time(std::filesystem::path(path), ec);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cache.find(path);
            if (it != m_cache.end() && !ec && it->second.writeTime == wt)
            {
                return it->second.probe;
            }
        }

        probe.header = ReadHeader(path, kProbeSize);
        probe.info = DetectByMagic(probe.header);

        if (probe.info.format == ImageFormat::Unknown)
        {
            const ImageFormatInfo byExt = DetectByExtension(probe.extensionLower);
            if (byExt.format != ImageFormat::Unknown)
            {
                probe.info = byExt;
            }
        }

        if (!ec)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[path] = CacheEntry{ wt, probe };
        }

        return probe;
    }
}


