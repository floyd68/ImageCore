#include "TestHarness.h"

#include "DecodeScheduler.h"
#include "ImageCore.h"
#include "ImageCoreLog.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// ImageCore logging: laziness without a sink, UTF-8 conversion, and that a
// decode failure for a path outside the active code page cannot terminate a
// worker, with a normal sink or with a sink that throws.

namespace
{
    using namespace ImageCore;

    // Sink state (sinks are plain function pointers).
    std::mutex g_sinkMutex;
    std::vector<std::string> g_messages;

    void RecordingSink(Log::Level, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        g_messages.push_back(message);
    }

    void ThrowingSink(Log::Level, const std::string&)
    {
        throw std::runtime_error("sink failure");
    }

    struct ScopedSink
    {
        explicit ScopedSink(Log::Sink sink)
        {
            {
                std::lock_guard<std::mutex> lock(g_sinkMutex);
                g_messages.clear();
            }
            Log::SetSink(sink);
        }
        ~ScopedSink()
        {
            Log::SetSink(nullptr);
        }
    };

    int g_evaluations = 0;

    int CountedArgument()
    {
        ++g_evaluations;
        return 42;
    }

    // Emoji (surrogate pair), Japanese, and Thai: outside every single-byte or
    // East Asian ANSI code page at once.
    const std::wstring kNonAcpName = L"\U0001F600_テスト_ไทย.png";
    const std::string kNonAcpNameUtf8 =
        "\xF0\x9F\x98\x80_\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88_\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2.png";

    std::wstring MissingNonAcpPath()
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        return (std::filesystem::path(tempPath) / kNonAcpName).wstring();
    }

    // Decodes `path` at full resolution with one worker and waits (bounded) for
    // the callback; returns the HRESULT, or S_FALSE if no callback arrived.
    HRESULT DecodeOnce(DecodeScheduler& scheduler, const std::wstring& path, uint64_t handle)
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        HRESULT hr = S_FALSE;

        DecodeTask task;
        task.request = ImageRequest(path, ImagePurpose::FullResolution);
        task.request.allowGpuCompressedDDS = false;
        task.handle = handle;
        task.callback = [&](PipelineResult&& result)
        {
            std::lock_guard<std::mutex> lock(mutex);
            hr = result.hr;
            done = true;
            cv.notify_all();
        };
        scheduler.Enqueue(std::move(task));

        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10), [&]() { return done; });
        return done ? hr : S_FALSE;
    }
}

IC_TEST(Log_ArgumentsNotEvaluatedWithoutSink)
{
    Log::SetSink(nullptr);
    g_evaluations = 0;
    IC_LOG_ERROR("value {}", CountedArgument());
    IC_LOG_WARN("value {}", CountedArgument());
    IC_LOG_INFO("value {}", CountedArgument());
    IC_CHECK(g_evaluations == 0);

    ScopedSink sink(&RecordingSink);
    IC_LOG_ERROR("value {}", CountedArgument());
    IC_CHECK(g_evaluations == 1);
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    IC_REQUIRE(g_messages.size() == 1);
    IC_CHECK(g_messages[0] == "value 42");
}

IC_TEST(Log_ToUtf8ConvertsNonAcpText)
{
    IC_CHECK(Log::ToUtf8(std::wstring_view(kNonAcpName)) == kNonAcpNameUtf8);
    IC_CHECK(Log::ToUtf8(std::filesystem::path(kNonAcpName)) == kNonAcpNameUtf8);
    IC_CHECK(Log::ToUtf8(std::wstring_view()).empty());

    // An unpaired surrogate does not throw; it becomes U+FFFD.
    const std::wstring lone = L"a\xD800" L"b";
    IC_CHECK(Log::ToUtf8(std::wstring_view(lone)) == "a\xEF\xBF\xBD" "b");
}

IC_TEST(Log_NonAcpDecodeFailureIsLoggedAsUtf8AndWorkerSurvives)
{
    ScopedSink sink(&RecordingSink);
    RegisterBuiltInDecoders();
    DecodeScheduler scheduler;
    scheduler.SetThreadCount(1);

    const std::wstring path = MissingNonAcpPath();
    IC_CHECK(FAILED(DecodeOnce(scheduler, path, 1)));

    // The single worker is still alive: a second request completes too.
    IC_CHECK(FAILED(DecodeOnce(scheduler, path, 2)));

    std::lock_guard<std::mutex> lock(g_sinkMutex);
    bool found = false;
    for (const auto& message : g_messages)
    {
        if (message.find("[DecodeScheduler] Decode failed (") != std::string::npos &&
            message.find(kNonAcpNameUtf8) != std::string::npos)
        {
            found = true;
        }
    }
    IC_CHECK(found);
}

IC_TEST(Log_ThrowingSinkCannotStopTheScheduler)
{
    ScopedSink sink(&ThrowingSink);
    RegisterBuiltInDecoders();
    DecodeScheduler scheduler;
    scheduler.SetThreadCount(1);

    const std::wstring path = MissingNonAcpPath();
    IC_CHECK(FAILED(DecodeOnce(scheduler, path, 1)));
    IC_CHECK(FAILED(DecodeOnce(scheduler, path, 2)));
}
