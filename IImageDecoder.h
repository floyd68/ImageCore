#pragma once

#include "DecodeTypes.h"

namespace ImageCore
{
    // Minimal interface implemented by user-defined extension decoders
    class IImageDecoder
    {
    public:
        virtual ~IImageDecoder() = default;
        virtual PipelineResult Decode(const ImageRequest& request, const DecodeInput& input) = 0;
    };
}



