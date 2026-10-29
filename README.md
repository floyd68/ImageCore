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
    - **I/O lane readers**: thumbnail/preview reads in Local, Archive and Remote lanes, serialized per lane and admitted per physical device (see [I/O lanes](#io-lanes))
    - **Main-image readers**: full-resolution reads of archive entries and remote files, so a decode worker never waits for slow I/O
  - Uses `notify_all()` intentionally because multiple worker types share a CV with different predicates
  - Best-effort cancel: canceled handles suppress callbacks (in-flight decode cannot be interrupted)

- **`ImageDecodeDispatcher`** (`ImageDecodeDispatcher.*`)
  - Routes requests to a decoder (WIC/DirectXTex) using a format probe. The WIC probe rejects magic that DirectXTex owns (DDS, HDR, EXR), so a file DirectXTex rejects is not retried through WIC
  - `Decode(request, input)` decodes from `input.bytes` when present (`request.source` then only supplies the extension and is never opened), otherwise from the path
  - WIC decodes cache a per-thread WIC factory: `HasThreadResources()` reports it, and `ReleaseThreadResources()` releases it; call it before `CoUninitialize()` on any thread that decoded

- **`FileByteCache`** (`FileByteCache.*`)
  - Caches prefetched file bytes for thumbnail/preview stages

## Pipeline overview

### FullResolution (main image)

1) Request goes to `DecodeScheduler` high queue (an archive entry or remote file is first read by a main-image reader, and queued with its bytes)
2) High worker decodes via `ImageDecodeDispatcher`
3) Callback returns a **`DecodedImage`** payload:
   - CPU path: BGRA8 premultiplied pixels (`DXGI_FORMAT_B8G8R8A8_UNORM`)
     (an `_SRGB` DDS keeps its sRGB-encoded values; nothing is linearized)
   - GPU path: BCn blocks (`DXGI_FORMAT_BC*`) when `allowGpuCompressedDDS == true`

### Thumbnail / Preview

1) Request goes to its I/O lane (from `IPathByteSource::ClassifyPath`)
2) File bytes are prefetched by a lane reader (sequential within the lane) and attached to the task, which owns them; `FileByteCache` only de-duplicates reads for same-path tasks
3) Task is pushed to background queue
4) Background worker decodes using the task's prefetched bytes when available

Backpressure counts tasks holding prefetched bytes plus reads in progress (a global cap, and a per-lane depth). Canceling a queued task removes it immediately and releases its bytes and slot.

## I/O lanes

A slow read that is still wanted (a deep entry of a solid 7z, a network or cloud file) only delays the work behind it in its own lane. The full design is in the comment above `DecodeScheduler` in `DecodeScheduler.h`; in short:

- **Classification**: the host reports `Local`, `Archive`, `Remote` or `Unknown` per path (`IPathByteSource::ClassifyPath`, a fast synchronous call made once per request). `Unknown` is scheduled as `Local`; the default classification is `Archive` when `ResolveHostPath` changes the path, else `Unknown`.
- **Lanes**: Local lanes per physical device; Archive lanes per device on an HDD and per archive on an SSD; Remote lanes per UNC server or drive letter. Each lane reads one thumbnail/preview at a time, in priority order.
- **Device admission**: devices come from `IOCTL_STORAGE_GET_DEVICE_NUMBER` (several volume letters of one disk are one device), cached per volume for 60 s. An HDD admits two reads at once, one per lane kind (Local, Archive): a blocked archive read does not stop local thumbnails on the same disk, at the cost of two interleaved read streams there. Strict one-reader-per-HDD serialization is not promised. Remote reads are not counted against a local device.
- **Bounds**: two reader threads per lane kind (a slow lane kind cannot take readers from another), two main-image readers, at most two main-image reads plus inputs held; thumbnail bytes held plus reads in progress stay within the global prefetch cap (at most 8). The memory bound is a count bound: each read is at most 1 GiB, and there is no aggregate byte budget.
- **Fairness**: at equal priority the lane served least recently goes first, so a newly active folder is not queued behind another lane's backlog; across lane kinds, the last free prefetch slot goes to the higher-priority request.
- **Main images**: an archive or remote main image is read beside its lane's thumbnail reads (never behind them) by a main-image reader and then decoded; local main images are decoded by the worker directly as before. An archive or remote read that fails (no bytes, not stopped) is delivered as a read error without decoding, so no decoder reopens the slow source on a decode worker; a failed local read still falls back to the path.
- **Cancellation**: a request waiting for lane, device or slot capacity holds nothing and is removed from its lane by `Cancel` at once; idle lanes are reclaimed.

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
- `ImageLoader::Shutdown(bound)` does the same but waits at most `bound` and returns whether every thread was joined. On `false` nothing is detached: the loader keeps its scheduler, keeps rejecting requests, and a later call retries the join; the host should take a forced-exit path instead of static destruction. `DecodeScheduler::SetThreadCount` does not restart until a retry succeeds.
- `ImageLoader::ShutdownIfCreated(bound)` is the bounded shutdown for host cleanup paths: it never constructs the loader (returns `true` when ImageCore was never used, starting no thread) and otherwise returns `Instance().Shutdown(bound)`.

## Priority

- Full-resolution (main image) work always runs at above-normal thread priority, including when a background worker picks it up; thumbnail/preview work runs in Windows background mode (lower CPU and I/O priority).
- A thread is in background mode only while it runs that work (a thumbnail/preview decode, the prefetch read); idle workers and the idle prefetch thread wait at normal priority or above, so they wake for new work or shutdown without queuing behind normal-priority load.
- `Shutdown` raises every scheduler thread above normal priority before waiting for it, overriding background mode: a thread in background mode runs below all normal-priority work, and on a CPU-saturated machine it would otherwise get the processor only every few seconds and miss a shutdown bound.
- The same starvation applies to the thumbnail work itself: under normal-priority load on every CPU, background-mode reads and decodes advance only every few seconds. `DecodeScheduler::SetBackgroundWorkPriority(Normal)` is a test seam that runs that work at normal priority instead; the scheduling-logic tests use it so their progress deadlines do not depend on other load on the machine, and the priority tests keep background mode. Production always uses background mode.
- I/O lanes keep this policy: a lane reader enters background mode only around a thumbnail/preview read and waits at normal priority; main-image readers run above normal. With lanes, a starved or scan-delayed background read holds up only its own lane. Raising the read priority alone would not make thumbnails progress under all-core normal-priority load, since their decode also runs in background mode; changing both stages is a separate decision.

## Building

`ImageCore` is a static library (`ImageCore.vcxproj`) used by the app.

Typical build is via the top-level app project.

## Integration notes

UI layer (`FD2D::Image` in the app) is responsible for:

- maintaining “current selection” state
- token-gating results (drop stale callbacks)
- converting decoded output to a D2D bitmap (CPU path) or a D3D SRV (GPU path)


