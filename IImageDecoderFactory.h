#pragma once

#include "IImageDecoder.h"
#include <span>
#include <string>
#include <string_view>
#include <memory>

namespace ImageCore
{
    // 디코더 팩토리/디스크립터
    // - id는 GUID 문자열 등(예: L"{...}")을 권장하지만, 단순 문자열도 허용
    // - ImageCore 소스 수정 없이 앱에서 RegisterFactory로 확장 가능
    class IImageDecoderFactory
    {
    public:
        virtual ~IImageDecoderFactory() = default;

        virtual std::wstring_view Id() const = 0;
        virtual std::wstring_view DisplayName() const = 0;

        // 우선순위: 높을수록 먼저 시도
        virtual int Priority() const = 0;

        // 지원 확장자 목록 (".dds" 형태, 소문자 권장)
        virtual std::span<const std::wstring_view> SupportedExtensions() const = 0;

        // Thumbnail/Preview/FullResolution 중 지원 여부
        virtual bool SupportsPurpose(ImagePurpose purpose) const = 0;

        // 요청 목적에 맞는 디코더 생성
        virtual std::unique_ptr<IImageDecoder> Create(ImagePurpose purpose) const = 0;
    };
}


