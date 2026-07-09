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
    // LRU cache entry
    // Note: Stores a D2D bitmap, but this is only used in the UI layer.
    // Ideally, ImageCore provides decode results as DecodedImage and D2D conversion is handled in the UI layer.
    // (Currently maintaining D2D dependency for compatibility with existing structure)
    struct ImageCacheEntry
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        size_t memorySize;  // Memory usage (bytes)
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

        // Find in cache
        bool Find(const ImageRequest& request, Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap);

        // Store in cache
        void Store(const ImageRequest& request, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap);

        // Clear cache
        void Clear();

        // Set cache size limit
        void SetSizeLimit(size_t maxBytes);

        // Current memory in use
        size_t CurrentMemoryUsage() const;

    private:
        void EvictLRU();

        std::unordered_map<ImageRequest, ImageCacheEntry, std::hash<ImageRequest>> m_cache;
        std::list<ImageRequest> m_lruList;  // LRU order (front = most recently used)

        mutable std::mutex m_mutex;
        size_t m_maxMemoryBytes;
        size_t m_currentMemoryBytes;
    };
}

