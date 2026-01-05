#pragma once

#include "ImageRequest.h"
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>

namespace ImageCore
{
    class IImageDecoderFactory;

    // Decoder들이 지원하는 확장자들을 등록/조회하는 레지스트리
    class DecoderRegistry final
    {
    public:
        static DecoderRegistry& Instance();

        // 디코더 팩토리 등록 (앱이 커스텀 디코더를 ImageCore 소스 수정 없이 등록 가능)
        // 같은 id가 이미 등록되어 있으면 false.
        bool RegisterFactory(const std::shared_ptr<IImageDecoderFactory>& factory);

        // 등록 해제 (옵션)
        bool UnregisterFactory(std::wstring_view id);

        // path의 확장자를 보고 지원 여부 판단
        bool IsSupportedPath(const std::wstring& path) const;

        // 확장자(예: L".dds") 지원 여부 판단
        bool IsSupportedExtension(std::wstring_view ext) const;

        // 이 요청에 대해 시도할 factory 목록 (우선순위 순)
        std::vector<std::shared_ptr<IImageDecoderFactory>> GetCandidateFactories(const ImageRequest& request) const;
        std::vector<std::shared_ptr<IImageDecoderFactory>> GetCandidateFactories(const ImageRequest& request, std::span<const uint8_t> header) const;

    private:
        DecoderRegistry() = default;

        static std::wstring NormalizeExtension(std::wstring_view ext);
        static std::wstring GetLowerExtensionFromPath(const std::wstring& path);

        // id -> factory
        std::unordered_map<std::wstring, std::shared_ptr<IImageDecoderFactory>> m_factoriesById {};

        // ext -> factory ids (priority order)
        std::unordered_map<std::wstring, std::vector<std::wstring>> m_factoriesByExtension {};

        mutable std::mutex m_mutex {};
    };
}


