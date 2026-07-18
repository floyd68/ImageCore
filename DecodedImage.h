#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <dxgiformat.h>

namespace ImageCore
{
    // Alpha interpretation of the decoded pixels/blocks - the DECODER decides
    // it, not the format (a BGRA8 blob can be either), so consumers must not
    // infer it from dxgiFormat. Drives correct compositing (premultiplied vs
    // straight) and accurate channel isolation.
    enum class AlphaMode
    {
        Unknown,        // not determined - treat as Straight
        Straight,       // color is independent of alpha (non-premultiplied)
        Premultiplied,  // color already multiplied by alpha
        Opaque,         // alpha is 1 everywhere (premult == straight)
        Custom,         // app-defined (e.g. alpha carries data, not coverage)
    };

    // Unified decoded payload for FD2D:
    // - CPU path: BGRA8 pixels (DXGI_FORMAT_B8G8R8A8_UNORM)
    // - GPU path: BCn blocks (DXGI_FORMAT_BC*)
    //
    // Policy:
    // - CPU BGRA8 output is normalized to premultiplied (alphaMode == Premultiplied);
    //   compressed BCn passthrough keeps its source DDS alpha mode.
    // - Mipmaps are not included (mip0 only).
    struct DecodedImage final
    {
        uint32_t width { 0 };
        uint32_t height { 0 };
        DXGI_FORMAT dxgiFormat { DXGI_FORMAT_UNKNOWN };
        AlphaMode alphaMode { AlphaMode::Unknown };

        // Bytes per row for mip0.
        // - BGRA8: width * 4
        // - BCn: block-row pitch in bytes
        uint32_t rowPitchBytes { 0 };

        // Total bytes in `blocks` (mip0 only).
        uint32_t blockBytes { 0 };

        // One contiguous blob holding pixels/blocks.
        std::shared_ptr<std::vector<uint8_t>> blocks {};
    };
}

