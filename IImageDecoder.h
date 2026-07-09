#pragma once

#include "DecodeTypes.h"

struct IWICImagingFactory;

namespace ImageCore
{
    // Minimal interface implemented by user-defined extension decoders
    class IImageDecoder
    {
    public:
        virtual ~IImageDecoder() = default;
        virtual PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input) = 0;
    };
}



