#include "ImageCoreLog.h"

#include <windows.h>

#include <atomic>

namespace ImageCore
{
namespace Log
{
    namespace
    {
        std::atomic<Sink> s_sink { nullptr };
    }

    void SetSink(Sink sink)
    {
        s_sink.store(sink, std::memory_order_relaxed);
    }

    std::string ToUtf8(std::wstring_view text) noexcept
    {
        if (text.empty())
        {
            return {};
        }

        try
        {
            // Without WC_ERR_INVALID_CHARS, unpaired surrogates are replaced with
            // U+FFFD instead of failing the whole conversion.
            const int length = static_cast<int>(text.size());
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
            if (bytes <= 0)
            {
                return "<invalid UTF-16>";
            }

            std::string out(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), bytes, nullptr, nullptr);
            return out;
        }
        catch (...)
        {
            return {};
        }
    }

    std::string ToUtf8(const std::filesystem::path& path) noexcept
    {
        return ToUtf8(std::wstring_view(path.native()));
    }

    namespace Detail
    {
        bool HasSink() noexcept
        {
            return s_sink.load(std::memory_order_relaxed) != nullptr;
        }

        void Dispatch(Level level, std::string message)
        {
            if (Sink sink = s_sink.load(std::memory_order_relaxed))
            {
                sink(level, message);
            }
        }
    }
}
}
