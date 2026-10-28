#include "IPathByteSource.h"

#include <atomic>

namespace ImageCore
{
    namespace
    {
        std::atomic<IPathByteSource*> s_source { nullptr };
    }

    IoLane IoLaneFor(PathSourceKind kind)
    {
        switch (kind)
        {
        case PathSourceKind::Archive:
            return IoLane::Archive;
        case PathSourceKind::Remote:
            return IoLane::Remote;
        case PathSourceKind::Unknown:
        case PathSourceKind::Local:
            break;
        }
        return IoLane::Local;
    }

    std::vector<uint8_t> IPathByteSource::ReadAll(const std::wstring& path, std::stop_token stop) const
    {
        (void)stop;
        return ReadAll(path);
    }

    PathSourceKind IPathByteSource::ClassifyPath(const std::wstring& path) const
    {
        if (path.empty())
        {
            return PathSourceKind::Unknown;
        }
        const std::wstring host = ResolveHostPath(path);
        return (!host.empty() && host != path) ? PathSourceKind::Archive : PathSourceKind::Unknown;
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
