#pragma once

#include <string>

namespace ImageCore
{
    // Size 구조체 (FD2D Layout.h 의존성 제거)
    struct Size
    {
        float w, h;
    };

    enum class ImagePurpose
    {
        Thumbnail,      // 썸네일 (작은 크기)
        Preview,        // 미리보기 (중간 크기)
        FullResolution  // 원본 해상도
    };

    struct ImageRequest
    {
        std::wstring source;        // 파일 경로
        ImagePurpose purpose;       // 로딩 목적
        Size targetSize;            // 썸네일/미리보기용 목표 크기
        bool srgb;                  // sRGB 색공간 사용 여부
        // If true, allow returning GPU-compressed DDS ScratchImage for GPU-native rendering.
        // If false (e.g., D2D-only renderer), the decode pipeline should return CPU-displayable BGRA8 pixels.
        bool allowGpuCompressedDDS;

        ImageRequest()
            : purpose(ImagePurpose::FullResolution)
            , targetSize{ 0.0f, 0.0f }
            , srgb(true)
            , allowGpuCompressedDDS(true)
        {
        }

        ImageRequest(const std::wstring& path, ImagePurpose p = ImagePurpose::FullResolution)
            : source(path)
            , purpose(p)
            , targetSize{ 0.0f, 0.0f }
            , srgb(true)
            , allowGpuCompressedDDS(true)
        {
        }
    };

    // 캐시 키 생성용
    inline bool operator==(const ImageRequest& a, const ImageRequest& b)
    {
        return a.source == b.source &&
            a.purpose == b.purpose &&
            a.targetSize.w == b.targetSize.w &&
            a.targetSize.h == b.targetSize.h &&
            a.srgb == b.srgb &&
            a.allowGpuCompressedDDS == b.allowGpuCompressedDDS;
    }

    inline bool operator<(const ImageRequest& a, const ImageRequest& b)
    {
        if (a.source != b.source)
        {
            return a.source < b.source;
        }
        if (a.purpose != b.purpose)
        {
            return static_cast<int>(a.purpose) < static_cast<int>(b.purpose);
        }
        if (a.targetSize.w != b.targetSize.w)
        {
            return a.targetSize.w < b.targetSize.w;
        }
        if (a.targetSize.h != b.targetSize.h)
        {
            return a.targetSize.h < b.targetSize.h;
        }
        if (a.srgb != b.srgb)
        {
            return a.srgb < b.srgb;
        }
        return a.allowGpuCompressedDDS < b.allowGpuCompressedDDS;
    }
}

// ImageRequest 해시 함수 (std 네임스페이스에 특수화)
namespace std
{
    template<>
    struct hash<ImageCore::ImageRequest>
    {
        size_t operator()(const ImageCore::ImageRequest& req) const
        {
            size_t h1 = hash<wstring>{}(req.source);
            size_t h2 = static_cast<size_t>(req.purpose);
            size_t h3 = hash<float>{}(req.targetSize.w);
            size_t h4 = hash<float>{}(req.targetSize.h);
            size_t h5 = hash<bool>{}(req.srgb);
            size_t h6 = hash<bool>{}(req.allowGpuCompressedDDS);
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4) ^ (h6 << 5);
        }
    };
}

