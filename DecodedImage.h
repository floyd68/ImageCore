#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <dxgiformat.h>

namespace ImageCore
{
    // How the color relates to alpha - a STORAGE property, independent of what
    // the alpha MEANS (see AlphaUsage). The decoder reports this; a consumer must
    // not infer it from dxgiFormat. ImageCore preserves the ORIGINAL channels (no
    // premultiplying on decode), so a data-bearing alpha is never destroyed and a
    // consumer's usage override can always recover the straight color.
    enum class AlphaEncoding
    {
        Unknown,        // not stated - treat as Straight
        Straight,       // color independent of alpha (non-premultiplied)
        Premultiplied,  // color already multiplied by alpha
        Opaque,         // alpha is 1 everywhere (encoding is moot)
    };

    // What the alpha MEANS - independent of encoding. The decoder can only HINT
    // (e.g. a DDS declared CUSTOM); the consumer resolves Auto with its own policy
    // (see NIFDiff's AlphaUsage resolver) and may override per-image.
    enum class AlphaUsage
    {
        Auto,      // decide by policy (encoding + source provenance)
        Coverage,  // alpha is transparency (composite it)
        Data,      // alpha carries data (height/spec/mask) - show RGB opaque
    };

    // Unified decoded payload for FD2D:
    // - CPU path: BGRA8 straight pixels (DXGI_FORMAT_B8G8R8A8_UNORM)
    // - GPU path: BCn blocks (DXGI_FORMAT_BC*)
    //
    // Policy:
    // - Pixels are decoded to STRAIGHT alpha and left unmodified (premultiply, if
    //   needed, happens only at presentation). Only a genuinely premultiplied
    //   source reports AlphaEncoding::Premultiplied.
    // - Mipmaps are not included (mip0 only).
    struct DecodedImage final
    {
        uint32_t width { 0 };
        uint32_t height { 0 };
        DXGI_FORMAT dxgiFormat { DXGI_FORMAT_UNKNOWN };

        // How color relates to alpha (storage), and any decoder hint about what
        // the alpha means. Usage stays Auto unless the source declared it (CUSTOM).
        AlphaEncoding alphaEncoding { AlphaEncoding::Unknown };
        AlphaUsage alphaUsageHint { AlphaUsage::Auto };

        // Fact-based provenance: was the SOURCE block-compressed (BCn), even if it
        // was decompressed to BGRA8 (e.g. a thumbnail)? The final dxgiFormat can't
        // tell, but the Auto usage policy needs it (compressed straight alpha is
        // usually data in Bethesda assets).
        bool sourceWasBlockCompressed { false };

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
