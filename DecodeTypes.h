#pragma once

#include "ImageRequest.h"
#include "DecodedImage.h"
#include <memory>
#include <span>
#include <cstdint>
#include <windows.h>

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

    // Decode result (unified payload).
    struct PipelineResult
    {
        HRESULT hr { E_FAIL };
        DecodedImage image {};
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



