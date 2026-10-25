#include "IPathByteSource.h"

#include <atomic>

namespace ImageCore
{
    namespace
    {
        std::atomic<IPathByteSource*> s_source { nullptr };
    }

    std::vector<uint8_t> IPathByteSource::ReadAll(const std::wstring& path, std::stop_token stop) const
    {
        (void)stop;
        return ReadAll(path);
    }

    void SetPathByteSource(IPathByteSource* source)
    {
        s_source.store(source, std::memory_order_relaxed);
    }

    IPathByteSource* GetPathByteSource()
    {
        return s_source.load(std::memory_order_relaxed);
    }
}
