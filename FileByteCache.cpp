#include "FileByteCache.h"

namespace ImageCore
{
    FileByteCache& FileByteCache::Instance()
    {
        static FileByteCache s_instance;
        return s_instance;
    }

    std::wstring FileByteCache::GetVolumeForPath(const std::wstring& path)
    {
        if (path.size() >= 2 && path[1] == L':')
        {
            std::wstring vol;
            vol.push_back(path[0]);
            vol.push_back(L':');
            return vol;
        }
        return L"";
    }

    void FileByteCache::Put(const std::wstring& path, const std::shared_ptr<std::vector<uint8_t>>& bytes)
    {
        if (path.empty() || !bytes)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // If at capacity, evict arbitrary entries (this is a transient helper cache).
        while (m_map.size() >= m_maxEntries && !m_map.empty())
        {
            const auto it = m_map.begin();
            const std::wstring vol = it->second.volume;
            if (!vol.empty())
            {
                auto cit = m_countByVolume.find(vol);
                if (cit != m_countByVolume.end() && cit->second > 0)
                {
                    cit->second--;
                    if (cit->second == 0)
                    {
                        m_countByVolume.erase(cit);
                    }
                }
            }
            m_map.erase(it);
        }

        // Replace existing entry (update per-volume counts).
        auto existing = m_map.find(path);
        if (existing != m_map.end())
        {
            const std::wstring oldVol = existing->second.volume;
            if (!oldVol.empty())
            {
                auto cit = m_countByVolume.find(oldVol);
                if (cit != m_countByVolume.end() && cit->second > 0)
                {
                    cit->second--;
                    if (cit->second == 0)
                    {
                        m_countByVolume.erase(cit);
                    }
                }
            }
            m_map.erase(existing);
        }

        Entry e {};
        e.bytes = bytes;
        e.volume = GetVolumeForPath(path);
        m_map[path] = e;

        if (!e.volume.empty())
        {
            m_countByVolume[e.volume] += 1;
        }
    }

    std::shared_ptr<const std::vector<uint8_t>> FileByteCache::Get(const std::wstring& path) const
    {
        if (path.empty())
        {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(path);
        if (it == m_map.end())
        {
            return nullptr;
        }

        return it->second.bytes;
    }

    void FileByteCache::EraseIfUnshared(const std::wstring& path)
    {
        if (path.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(path);
        if (it == m_map.end())
        {
            return;
        }

        // If someone else is still holding the bytes, don't remove the cache entry yet.
        if (it->second.bytes && it->second.bytes.use_count() > 1)
        {
            return;
        }

        const std::wstring vol = it->second.volume;
        if (!vol.empty())
        {
            auto cit = m_countByVolume.find(vol);
            if (cit != m_countByVolume.end() && cit->second > 0)
            {
                cit->second--;
                if (cit->second == 0)
                {
                    m_countByVolume.erase(cit);
                }
            }
        }

        m_map.erase(it);
    }

    size_t FileByteCache::Count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_map.size();
    }

    size_t FileByteCache::CountForVolume(const std::wstring& volume) const
    {
        if (volume.empty())
        {
            return 0;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_countByVolume.find(volume);
        if (it == m_countByVolume.end())
        {
            return 0;
        }
        return it->second;
    }

    void FileByteCache::SetMaxEntries(size_t maxEntries)
    {
        if (maxEntries == 0)
        {
            maxEntries = 1;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_maxEntries = maxEntries;
        while (m_map.size() > m_maxEntries && !m_map.empty())
        {
            const auto it = m_map.begin();
            const std::wstring vol = it->second.volume;
            if (!vol.empty())
            {
                auto cit = m_countByVolume.find(vol);
                if (cit != m_countByVolume.end() && cit->second > 0)
                {
                    cit->second--;
                    if (cit->second == 0)
                    {
                        m_countByVolume.erase(cit);
                    }
                }
            }
            m_map.erase(it);
        }
    }

    size_t FileByteCache::MaxEntries() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_maxEntries;
    }

}


