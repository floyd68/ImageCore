#pragma once

#include "DecodeTypes.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
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

        // Tests/diagnostics: WIC decoders (opens of a decoded input) created on
        // the calling thread. A WIC decode creates one.
        static uint64_t ThreadWicDecoderCount();

        // Test seam: every WIC decoder ImageCore creates on the calling thread
        // is passed through `wrapper` (an empty function: none), so a test can
        // stand in for parts of a container, such as its thumbnail.
        using WicDecoderWrapper = std::function<Microsoft::WRL::ComPtr<IWICBitmapDecoder>(
            Microsoft::WRL::ComPtr<IWICBitmapDecoder>)>;
        static void SetThreadWicDecoderWrapperForTesting(WicDecoderWrapper wrapper);
    };
}


