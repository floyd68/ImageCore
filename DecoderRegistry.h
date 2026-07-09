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

    // Registry for registering/querying extensions supported by decoders
    class DecoderRegistry final
    {
    public:
        static DecoderRegistry& Instance();

        // Register a decoder factory (apps can register custom decoders without modifying ImageCore source)
        // Returns false if a factory with the same id is already registered.
        bool RegisterFactory(const std::shared_ptr<IImageDecoderFactory>& factory);

        // Unregister (optional)
        bool UnregisterFactory(std::wstring_view id);

        // Determines support based on the extension of the given path
        bool IsSupportedPath(const std::wstring& path) const;

        // Determines whether the given extension (e.g., L".dds") is supported
        bool IsSupportedExtension(std::wstring_view ext) const;
        std::vector<std::wstring> GetSupportedExtensions() const;

        // List of factories to try for this request (in priority order)
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


