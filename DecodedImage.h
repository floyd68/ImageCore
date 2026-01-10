#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <dxgiformat.h>

namespace ImageCore
{
    // Unified decoded payload for FD2D:
    // - CPU path: BGRA8 premultiplied pixels (DXGI_FORMAT_B8G8R8A8_UNORM)
    // - GPU path: BCn blocks (DXGI_FORMAT_BC*)
    //
    // Policy:
    // - Premultiply is always applied for CPU BGRA8 output (no flag needed).
    // - Mipmaps are not included (mip0 only).
    struct DecodedImage final
    {
        uint32_t width { 0 };
        uint32_t height { 0 };
        DXGI_FORMAT dxgiFormat { DXGI_FORMAT_UNKNOWN };

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

