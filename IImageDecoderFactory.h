#pragma once

#include "IImageDecoder.h"
#include <span>
#include <string>
#include <string_view>
#include <memory>
#include <cstdint>
#include <windows.h>

namespace ImageCore
{
    // Decoder factory/descriptor
    // - id is recommended to be a GUID string (e.g., L"{...}"), but plain strings are also allowed
    // - Extensible from the app via RegisterFactory without modifying ImageCore source
    class IImageDecoderFactory
    {
    public:
        virtual ~IImageDecoderFactory() = default;

        virtual std::wstring_view Id() const = 0;
        virtual std::wstring_view DisplayName() const = 0;

        // Priority: higher values are tried first
        virtual int Priority() const = 0;

        // List of supported extensions (in ".dds" format, lowercase recommended)
        virtual std::span<const std::wstring_view> SupportedExtensions() const = 0;

        // Determines support more precisely using magic/header-based hints.
        // Default implementation returns true (= not excluded from candidates). Custom decoders may override if needed.
        virtual bool Probe(std::span<const uint8_t> header, std::wstring_view extensionLower) const
        {
            UNREFERENCED_PARAMETER(header);
            UNREFERENCED_PARAMETER(extensionLower);
            return true;
        }

        // Whether Thumbnail/Preview/FullResolution is supported
        virtual bool SupportsPurpose(ImagePurpose purpose) const = 0;

        // Create a decoder suited to the requested purpose
        virtual std::unique_ptr<IImageDecoder> Create(ImagePurpose purpose) const = 0;
    };
}


