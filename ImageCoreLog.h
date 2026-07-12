#pragma once

// ImageCoreLog.h - self-contained logging macros for ImageCore internals.
// With no sink installed, every IC_LOG_* call is a silent no-op.
// Host apps call ImageCore::Log::SetSink() once at startup.

#include <chrono>
#include <sstream>
#include <string>

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

        template <typename T>
        void FormatArg(std::ostringstream& out, const T& value)
        {
            out << value;
        }

        inline void FormatArg(std::ostringstream& out, bool value)
        {
            out << (value ? "true" : "false");
        }

        template <typename... Args>
        std::string Format(const char* fmt, const Args&... args)
        {
            std::ostringstream out;
            const char* p = fmt;
            auto emitOne = [&](const auto& value)
            {
                while (*p != '\0' && !(p[0] == '{' && p[1] == '}'))
                {
                    out << *p++;
                }
                if (*p != '\0')
                {
                    p += 2;
                }
                FormatArg(out, value);
            };
            (emitOne(args), ...);
            out << p;
            return out.str();
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
