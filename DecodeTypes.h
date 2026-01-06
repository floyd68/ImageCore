#pragma once

#include "ImageRequest.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <memory>
#include <span>
#include <cstdint>

// DirectXTex types
#include "../external/DirectXTex/DirectXTex/DirectXTex.h"

namespace ImageCore
{
    // Explicit decode input (optional):
    // - bytes: full file bytes (prefetched by I/O stage)
    // - header: first kProbeSize bytes (used for magic/probing without extra disk I/O)
    struct DecodeInput
    {
        std::span<const uint8_t> bytes {};
        std::span<const uint8_t> header {};
    };

    // 디코딩 결과 (WIC 또는 ScratchImage 중 하나를 채움)
    struct PipelineResult
    {
        HRESULT hr { E_FAIL };
        Microsoft::WRL::ComPtr<IWICBitmapSource> wicBitmap {};
        std::unique_ptr<DirectX::ScratchImage> scratchImage {};
        Size imageSize { 0.0f, 0.0f };

        PipelineResult() = default;
        explicit PipelineResult(HRESULT h)
            : hr(h)
        {
        }

        PipelineResult(const PipelineResult&) = delete;
        PipelineResult& operator=(const PipelineResult&) = delete;
        PipelineResult(PipelineResult&&) noexcept = default;
        PipelineResult& operator=(PipelineResult&&) noexcept = default;
    };
}



