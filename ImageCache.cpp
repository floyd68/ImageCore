#include "ImageCache.h"
#include <algorithm>

namespace ImageCore
{
    ImageCache::ImageCache()
        : m_maxMemoryBytes(256 * 1024 * 1024)  // Default 256MB
        , m_currentMemoryBytes(0)
    {
    }

    ImageCache::~ImageCache()
    {
        Clear();
    }

    bool ImageCache::Find(const ImageRequest& request, Microsoft::WRL::ComPtr<ID2D1Bitmap>& outBitmap)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_cache.find(request);
        if (it == m_cache.end())
        {
            return false;
        }

        // LRU update: move to end of list
        m_lruList.erase(it->second.lruIterator);
        m_lruList.push_back(request);
        it->second.lruIterator = std::prev(m_lruList.end());

        outBitmap = it->second.bitmap;
        return true;
    }

    void ImageCache::Store(const ImageRequest& request, Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap)
    {
        if (bitmap == nullptr)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Remove if already exists
        auto it = m_cache.find(request);
        if (it != m_cache.end())
        {
            m_currentMemoryBytes -= it->second.memorySize;
            m_lruList.erase(it->second.lruIterator);
            m_cache.erase(it);
        }

        // Calculate memory size
        D2D1_SIZE_F size = bitmap->GetSize();
        D2D1_SIZE_U pixelSize = bitmap->GetPixelSize();
        size_t memorySize = static_cast<size_t>(pixelSize.width * pixelSize.height * 4);  // BGRA = 4 bytes

        // Check memory limit and evict LRU
        while (m_currentMemoryBytes + memorySize > m_maxMemoryBytes && !m_cache.empty())
        {
            EvictLRU();
        }

        // Add new entry
        ImageCacheEntry entry;
        entry.bitmap = bitmap;
        entry.memorySize = memorySize;
        m_lruList.push_back(request);
        entry.lruIterator = std::prev(m_lruList.end());

        m_cache[request] = entry;
        m_currentMemoryBytes += memorySize;
    }

    void ImageCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
        m_lruList.clear();
        m_currentMemoryBytes = 0;
    }

    void ImageCache::SetSizeLimit(size_t maxBytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maxMemoryBytes = maxBytes;

        // If the limit was reduced, evict excess entries
        while (m_currentMemoryBytes > m_maxMemoryBytes && !m_cache.empty())
        {
            EvictLRU();
        }
    }

    size_t ImageCache::CurrentMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentMemoryBytes;
    }

    void ImageCache::EvictLRU()
    {
        if (m_lruList.empty())
        {
            return;
        }

        // Remove the oldest entry
        const ImageRequest& oldest = m_lruList.front();
        auto it = m_cache.find(oldest);
        if (it != m_cache.end())
        {
            m_currentMemoryBytes -= it->second.memorySize;
            m_cache.erase(it);
        }
        m_lruList.pop_front();
    }
}

