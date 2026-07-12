#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ImageCore
{
    // Host-provided byte source for filesystem and archive-backed paths.
    // ImageCore stays free of Floar/VFS dependencies; the app installs an adapter.
    class IPathByteSource
    {
    public:
        virtual ~IPathByteSource() = default;

        // Resolve a display path to the host/archive file path used for volume queries.
        // When the source cannot parse the path, return path unchanged.
        virtual std::wstring ResolveHostPath(const std::wstring& path) const = 0;

        // Read the entire file (disk or archive entry). Empty vector on failure.
        virtual std::vector<uint8_t> ReadAll(const std::wstring& path) const = 0;
    };

    void SetPathByteSource(IPathByteSource* source);
    IPathByteSource* GetPathByteSource();
}
