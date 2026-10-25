#pragma once

#include <cstdint>
#include <stop_token>
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

        // Stop-aware read. ImageCore calls this one, with a token owned by the
        // request; Cancel and Shutdown have stop requested on it by a
        // scheduler-owned stop-dispatch thread, outside every scheduler lock (so
        // a stop callback never runs on the canceling thread and may call back
        // into ImageCore; one that blocks makes a bounded Shutdown time out
        // instead of hanging it). A source that honours the
        // token returns early with an empty or partial vector; once stop was
        // requested ImageCore discards whatever the read returns, never caches
        // it and never decodes the request. ImageCore also tries
        // CancelSynchronousIo on the reading thread (best effort), so a
        // synchronous OS call the source makes on that thread may fail with
        // ERROR_OPERATION_ABORTED. Work the source waits for on other threads
        // is not OS-cancelled by ImageCore: wait for it on the token instead.
        //
        // The default delegates to ReadAll(path), so legacy (ReadAll-only)
        // sources keep working; their reads are not interrupted by the token,
        // and a bounded scheduler shutdown may then report a timeout.
        virtual std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const;
    };

    void SetPathByteSource(IPathByteSource* source);
    IPathByteSource* GetPathByteSource();
}
