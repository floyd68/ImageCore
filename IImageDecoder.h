#pragma once

#include "DecodeTypes.h"

namespace ImageCore
{
    // 사용자 확장 디코더가 구현하는 최소 인터페이스
    class IImageDecoder
    {
    public:
        virtual ~IImageDecoder() = default;
        virtual PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory) = 0;
    };
}


