# ImageCore

ImageCore is the async image loading/decoding pipeline used by `FICture2`.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Copyright (c) 2024 EunSuk, Lee (이은석, floyd)

It focuses on high-throughput DDS workloads (thumbnails + full-resolution) while keeping UI interaction responsive.

## What it does

- Schedules decode work on background threads
- Prioritizes **FullResolution** requests (main image) over thumbnail/preview work
- Uses:
  - **DirectXTex** for DDS loading/conversion/resizing
  - **WIC** for common raster formats
- Provides explicit cancellation (best-effort) and avoids UI-thread decode work

## Main types

- **`ImageRequest`** (`ImageRequest.h`)
  - `source`: file path
  - `purpose`: `Thumbnail`, `Preview`, `FullResolution`
  - `targetSize`: requested output size for thumbnail/preview
  - `allowGpuCompressedDDS`: if true and applicable, keeps BCn blocks for a GPU upload path

- **`ImageLoader`** (`ImageLoader.h/.cpp`)
  - Public API: `Request(...) -> ImageHandle`, `Cancel(handle)`
  - Delegates work to `DecodeScheduler`

- **`DecodeScheduler`** (`DecodeScheduler.h/.cpp`)
  - Worker pools:
    - **HighPriority**: user-interactive full-resolution loads
    - **Background**: thumbnail/preview decode/resize work
    - **ThumbPrefetchThread**: serialized thumbnail I/O to reduce random-seek thrashing
  - Uses `notify_all()` intentionally because multiple worker types share a CV with different predicates
  - Best-effort cancel: canceled handles suppress callbacks (in-flight decode cannot be interrupted)

- **`ImageDecodeDispatcher`** (`ImageDecodeDispatcher.*`)
  - Routes requests to a decoder (WIC/DirectXTex) using a format probe

- **`FileByteCache`** (`FileByteCache.*`)
  - Caches prefetched file bytes for thumbnail/preview stages

## Pipeline overview

### FullResolution (main image)

1) Request goes to `DecodeScheduler` high queue
2) High worker decodes via `ImageDecodeDispatcher`
3) Callback returns a **`DecodedImage`** payload:
   - CPU path: BGRA8 premultiplied pixels (`DXGI_FORMAT_B8G8R8A8_UNORM`)
   - GPU path: BCn blocks (`DXGI_FORMAT_BC*`) when `allowGpuCompressedDDS == true`

### Thumbnail / Preview

1) Request goes through `ThumbPrefetchThread`
2) File bytes are prefetched (sequential disk reads) and attached to the task, which owns them; `FileByteCache` only de-duplicates reads for same-path tasks
3) Task is pushed to background queue
4) Background worker decodes using the task's prefetched bytes when available

Backpressure counts tasks holding prefetched bytes (global and per-volume caps). Canceling a queued task removes it immediately and releases its bytes and slot.

## DDS specifics

DirectXTex decoding supports:

- Mip-aware thumbnail/preview selection (avoid decoding full 8K unnecessarily)
- Faster resize filters for thumbnails/previews
- Full-resolution path can choose to ignore mips depending on quality needs

## Cancellation semantics

- `ImageLoader::Cancel(handle)` is **best-effort**:
  - queued tasks are skipped without a callback
  - if decode already started, we cannot abort the decoder safely, but the callback is suppressed when the cancel is observed before delivery starts
  - a callback whose delivery has already started can still run concurrently with, or after, `Cancel` returns, so callers must drop stale results themselves (for example with a generation)
  - a byte-source read in progress (thumbnail prefetch, full-resolution archive read) is asked to stop through the request's `std::stop_token`, passed to `IPathByteSource::ReadAll(path, stop)`; ImageCore also tries `CancelSynchronousIo` on the reading thread (best effort, not relied upon)
  - a stopped read's bytes (empty or partial) are discarded: never put into `FileByteCache` (a same-path request reads its own bytes) and never decoded, so the decoders cannot fall back to re-reading the path; a request canceled before its decode starts is not decoded
- Stop is signaled on a scheduler-owned stop-dispatch thread, outside the scheduler locks: `Cancel` and `Shutdown` never run a host stop callback on the caller's thread, a stop callback may call back into ImageCore, and one that blocks makes a bounded `Shutdown` time out instead of hanging it. OS cancellation goes through a per-thread read lease, so a late cancel never reaches the thread's next read.
- Byte sources that implement only `ReadAll(path)` keep working (the stop-aware overload delegates to it); their reads are not interrupted.
- Cancellation state is owned by each request and released when the request finishes, is canceled, or the scheduler shuts down; canceling an unknown or finished handle is a no-op and leaves nothing behind.
- `ImageLoader::Shutdown()` is idempotent and final: it cancels every live request (as above), queued requests are dropped without callbacks, and later `RequestDecoded` calls return 0 without invoking the callback. It joins the decode threads without a bound, so a read that honours neither its token nor OS cancellation keeps it waiting.
- `ImageLoader::Shutdown(bound)` does the same but waits at most `bound` and returns whether every thread was joined. On `false` nothing is detached: the loader keeps its scheduler and cache, keeps rejecting requests, and a later call retries the join; the host should take a forced-exit path instead of static destruction. `DecodeScheduler::SetThreadCount` does not restart until a retry succeeds.
- `ImageLoader::ShutdownIfCreated(bound)` is the bounded shutdown for host cleanup paths: it never constructs the loader (returns `true` when ImageCore was never used, starting no thread) and otherwise returns `Instance().Shutdown(bound)`.

## Priority

- Full-resolution (main image) work always runs at above-normal thread priority, including when a background worker picks it up; thumbnail/preview work runs in Windows background mode (lower CPU and I/O priority).

## Building

`ImageCore` is a static library (`ImageCore.vcxproj`) used by the app.

Typical build is via the top-level app project.

## Integration notes

UI layer (`FD2D::Image` in the app) is responsible for:

- maintaining “current selection” state
- token-gating results (drop stale callbacks)
- converting decoded output to a D2D bitmap (CPU path) or a D3D SRV (GPU path)


