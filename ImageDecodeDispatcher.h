#pragma once

#include "DecodeTypes.h"
#include <wincodec.h>
#include <string>
#include <vector>

namespace ImageCore
{
    // Dispatcher: asks the registry "which decoder handles this request?" and executes it.
    class ImageDecodeDispatcher final
    {
    public:
        static std::vector<std::wstring> GetSupportedExtensions();
        PipelineResult Decode(const ImageRequest& request, IWICImagingFactory* wicFactory, const DecodeInput& input);
    };
}


