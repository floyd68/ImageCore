#pragma once

#include "DecodeTypes.h"
#include <wincodec.h>

namespace ImageCore
{
    // Dispatcher: registry에게 "이 요청은 어떤 decoder?"를 묻고 실행한다.
    class ImageDecodeDispatcher final
    {
    public:
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory);
    };
}


