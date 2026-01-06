#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace ImageCore
{
    // Small transient cache for prefetched file bytes (used to split thumbnail I/O from decode/resize CPU work).
    // Keyed by full path. Intended for short-lived entries; decode workers should Erase() after consuming.
    class FileByteCache final
    {
    public:
        static FileByteCache& Instance();

        void Put(const std::wstring& path, const std::shared_ptr<std::vector<uint8_t>>& bytes);
        std::shared_ptr<const std::vector<uint8_t>> Get(const std::wstring& path) const;
        // Removes entry if the cache is the only owner (safe with concurrent/overlapping requests).
        void EraseIfUnshared(const std::wstring& path);

        size_t Count() const;
        size_t CountForVolume(const std::wstring& volume) const;

        // Soft limit to prevent unbounded memory growth.
        void SetMaxEntries(size_t maxEntries);
        size_t MaxEntries() const;

    private:
        FileByteCache() = default;
    public:
        static std::wstring GetVolumeForPath(const std::wstring& path);

    private:
        mutable std::mutex m_mutex {};
        struct Entry
        {
            std::shared_ptr<std::vector<uint8_t>> bytes {};
            std::wstring volume {};
        };

        std::unordered_map<std::wstring, Entry> m_map {};
        std::unordered_map<std::wstring, size_t> m_countByVolume {};
        size_t m_maxEntries { 4 };
    };
}


