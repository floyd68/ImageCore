#pragma once

// ImageCoreLog.h - self-contained logging macros for ImageCore internals.
// With no sink installed, every IC_LOG_* call is a silent no-op.
// Host apps call ImageCore::Log::SetSink() once at startup.
// Format strings use C++20 std::format.

#include <chrono>
#include <format>
#include <string>
#include <utility>

namespace ImageCore
{
namespace Log
{
    enum class Level { Trace, Debug, Info, Warn, Error };

    using Sink = void(*)(Level level, const std::string& message);

    void SetSink(Sink sink);

    namespace Detail
    {
        void Dispatch(Level level, std::string message);

        template <typename... Args>
        std::string Format(std::format_string<Args...> fmt, Args&&... args)
        {
            return std::format(fmt, std::forward<Args>(args)...);
        }
    }
}
}

#define IC_LOG_INFO(...)   ::ImageCore::Log::Detail::Dispatch(::ImageCore::Log::Level::Info,  ::ImageCore::Log::Detail::Format(__VA_ARGS__))
#define IC_LOG_WARN(...)   ::ImageCore::Log::Detail::Dispatch(::ImageCore::Log::Level::Warn,  ::ImageCore::Log::Detail::Format(__VA_ARGS__))
#define IC_LOG_ERROR(...)  ::ImageCore::Log::Detail::Dispatch(::ImageCore::Log::Level::Error, ::ImageCore::Log::Detail::Format(__VA_ARGS__))

#ifdef _DEBUG
#define IC_LOG_TRACE(...)  ::ImageCore::Log::Detail::Dispatch(::ImageCore::Log::Level::Trace, ::ImageCore::Log::Detail::Format(__VA_ARGS__))
#define IC_LOG_DEBUG(...)  ::ImageCore::Log::Detail::Dispatch(::ImageCore::Log::Level::Debug, ::ImageCore::Log::Detail::Format(__VA_ARGS__))
#else
#define IC_LOG_TRACE(...)  do {} while (0)
#define IC_LOG_DEBUG(...)  do {} while (0)
#endif

#define IC_TIMER_START(var) \
    auto var = std::chrono::steady_clock::now()

#define IC_ELAPSED_MS(var) \
    (std::chrono::duration_cast<std::chrono::milliseconds>( \
        std::chrono::steady_clock::now() - (var)).count())
