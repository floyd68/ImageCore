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
3) Callback returns either:
   - `ScratchImage` (DirectXTex), or
   - `IWICBitmapSource` (WIC)

### Thumbnail / Preview

1) Request goes through `ThumbPrefetchThread`
2) File bytes are prefetched (sequential disk reads), stored in `FileByteCache`
3) Task is pushed to background queue
4) Background worker decodes using the prefetched bytes when available

## DDS specifics

DirectXTex decoding supports:

- Mip-aware thumbnail/preview selection (avoid decoding full 8K unnecessarily)
- Faster resize filters for thumbnails/previews
- Full-resolution path can choose to ignore mips depending on quality needs

## Cancellation semantics

- `ImageLoader::Cancel(handle)` is **best-effort**:
  - queued tasks may be skipped
  - if decode already started, we cannot abort the decoder safely
  - but the callback is suppressed for canceled handles to avoid applying stale results

## Building

`ImageCore` is a static library (`ImageCore.vcxproj`) used by the app.

Typical build is via the top-level app project.

## Integration notes

UI layer (`FD2D::Image` in the app) is responsible for:

- maintaining “current selection” state
- token-gating results (drop stale callbacks)
- converting decoded output to a D2D bitmap (CPU path) or a D3D SRV (GPU path)


