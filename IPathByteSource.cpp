#include "IPathByteSource.h"

#include <atomic>

namespace ImageCore
{
    namespace
    {
        std::atomic<IPathByteSource*> s_source { nullptr };
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
