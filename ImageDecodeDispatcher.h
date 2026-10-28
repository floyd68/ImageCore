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
        PipelineResult Decode(const ImageRequest& request, const DecodeInput& input);

        // Releases the calling thread's cached COM objects (the per-thread WIC
        // factory). A decode worker must call this before CoUninitialize(); a
        // thread_local ComPtr would otherwise release the factory at thread exit,
        // after the apartment is gone, which is undefined and crashes intermittently.
        static void ReleaseThreadResources();

        // Whether the calling thread holds ImageCore's cached WIC factory (set
        // by a decode that went through WIC, cleared by ReleaseThreadResources()).
        // No side effects.
        static bool HasThreadResources();
    };
}


