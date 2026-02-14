#pragma once

#include "DecodeTypes.h"
#include <wincodec.h>
#include <string>
#include <vector>

namespace ImageCore
{
    // Dispatcher: registry에게 "이 요청은 어떤 decoder?"를 묻고 실행한다.
    class ImageDecodeDispatcher final
    {
    public:
        static std::vector<std::wstring> GetSupportedExtensions();
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input);
    };
}


