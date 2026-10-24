#pragma once

// ImageCoreLog.h - self-contained logging macros for ImageCore internals.
// With no sink installed, every IC_LOG_* call is a silent no-op: the arguments
// are not evaluated and nothing is formatted.
// Host apps call ImageCore::Log::SetSink() once at startup.
// Format strings use C++20 std::format; messages are UTF-8. Pass paths and
// other wide strings through Log::ToUtf8 (never std::filesystem::path::string(),
// which uses the active code page and throws for characters outside it).
// A logging failure (formatting, conversion, or a throwing sink) is swallowed so
// it can never escape into a decode worker.

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ImageCore
{
namespace Log
{
    enum class Level { Trace, Debug, Info, Warn, Error };

    using Sink = void(*)(Level level, const std::string& message);

    void SetSink(Sink sink);

    // Non-throwing UTF-16 -> UTF-8 conversion. Unpaired surrogates become U+FFFD.
    std::string ToUtf8(std::wstring_view text) noexcept;
    std::string ToUtf8(const std::filesystem::path& path) noexcept;
    // Exact-match overloads: std::wstring and wide literals convert to both
    // std::wstring_view and std::filesystem::path, which would be ambiguous.
    inline std::string ToUtf8(const std::wstring& text) noexcept { return ToUtf8(std::wstring_view(text)); }
    inline std::string ToUtf8(const wchar_t* text) noexcept
    {
        return text ? ToUtf8(std::wstring_view(text)) : std::string();
    }

    namespace Detail
    {
        bool HasSink() noexcept;
        void Dispatch(Level level, std::string message);

        template <typename... Args>
        std::string Format(std::format_string<Args...> fmt, Args&&... args)
        {
            return std::format(fmt, std::forward<Args>(args)...);
        }
    }
}
}

// Arguments are evaluated and formatted only when a sink is installed.
#define IC_LOG_AT(level, ...) \
    do { \
        if (::ImageCore::Log::Detail::HasSink()) \
        { \
            try \
            { \
                ::ImageCore::Log::Detail::Dispatch((level), ::ImageCore::Log::Detail::Format(__VA_ARGS__)); \
            } \
            catch (...) \
            { \
            } \
        } \
    } while (0)

#define IC_LOG_INFO(...)   IC_LOG_AT(::ImageCore::Log::Level::Info,  __VA_ARGS__)
#define IC_LOG_WARN(...)   IC_LOG_AT(::ImageCore::Log::Level::Warn,  __VA_ARGS__)
#define IC_LOG_ERROR(...)  IC_LOG_AT(::ImageCore::Log::Level::Error, __VA_ARGS__)

#ifdef _DEBUG
#define IC_LOG_TRACE(...)  IC_LOG_AT(::ImageCore::Log::Level::Trace, __VA_ARGS__)
#define IC_LOG_DEBUG(...)  IC_LOG_AT(::ImageCore::Log::Level::Debug, __VA_ARGS__)
#else
#define IC_LOG_TRACE(...)  do {} while (0)
#define IC_LOG_DEBUG(...)  do {} while (0)
#endif

#define IC_TIMER_START(var) \
    auto var = std::chrono::steady_clock::now()

#define IC_ELAPSED_MS(var) \
    (std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::steady_clock::now() - (var)).count())
