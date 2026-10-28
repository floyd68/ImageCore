#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace ImageCore
{
    // What kind of source a path's bytes come from, as far as the host knows.
    // It selects the I/O lane of a request (IoLaneFor); see DecodeScheduler.h.
    enum class PathSourceKind : uint8_t
    {
        Unknown,  // no classification: scheduled as Local (the conservative default)
        Local,    // a plain file on a local drive
        Archive,  // an entry inside an archive (extraction can take seconds)
        Remote,   // a network path or a cloud placeholder (the read waits on a download)
    };

    // Logical I/O lanes. A slow read in one lane never holds up another lane;
    // within a lane, reads are additionally admitted per physical device (see
    // DecodeScheduler.h).
    enum class IoLane : uint8_t
    {
        Local,
        Archive,
        Remote,
    };

    // Unknown and Local -> Local, Archive -> Archive, Remote -> Remote.
    IoLane IoLaneFor(PathSourceKind kind);

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

        // Classifies `path` for lane selection. Called once per request from
        // DecodeScheduler::Enqueue, on the caller's thread and outside every
        // scheduler lock; ImageCore treats it as a fast synchronous call and
        // does not cache the result. It must use only cheap signals (lexical
        // checks, cached drive types, local metadata): never a network
        // metadata query or a file read. Return Unknown when unsure; an
        // exception is treated as Unknown.
        //
        // The default returns Archive when ResolveHostPath(path) is a
        // different, non-empty path (an archive entry, as ImageCore has always
        // inferred), otherwise Unknown.
        virtual PathSourceKind ClassifyPath(const std::wstring& path) const;
    };

    void SetPathByteSource(IPathByteSource* source);
    IPathByteSource* GetPathByteSource();
}
