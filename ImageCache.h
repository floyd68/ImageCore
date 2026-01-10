#pragma once

#include "ImageRequest.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <unordered_map>
#include <list>
#include <mutex>
#include <memory>

namespace ImageCore
{
    // LRU 캐시 엔트리
    // Note: D2D bitmap을 저장하지만, 이는 UI 레이어에서만 사용
    // ImageCore는 decode 결과를 DecodedImage로 제공하고, D2D 변환은 UI 레이어에서 처리하는 것이 이상적.
    // (현재는 기존 구조 호환을 위해 D2D 의존성을 유지)
    struct ImageCacheEntry
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        size_t memorySize;  // 메모리 사용량 (바이트)
        std::list<ImageRequest>::iterator lruIterator;

        ImageCacheEntry()
            : memorySize(0)
        {
        }
    };

    class ImageCache
    {
    public:
        ImageCache();
        ~ImageCache();

        // 캐시에서 찾기
        bool Find(const ImageRequest& request, Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap);

        // 캐시에 저장
        void Store(const ImageRequest& request, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap);

        // 캐시 정리
        void Clear();

        // 캐시 크기 제한 설정
        void SetSizeLimit(size_t maxBytes);

        // 현재 사용 중인 메모리
        size_t CurrentMemoryUsage() const;

    private:
        void EvictLRU();

        std::unordered_map<ImageRequest, ImageCacheEntry, std::hash<ImageRequest>> m_cache;
        std::list<ImageRequest> m_lruList;  // LRU 순서 (front = 최근 사용)

        mutable std::mutex m_mutex;
        size_t m_maxMemoryBytes;
        size_t m_currentMemoryBytes;
    };
}

