#include "TestHarness.h"

#include "DecodeScheduler.h"
#include "ImageCore.h"
#include "ImageCoreLog.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

    // Completion state shared between a test and the scheduler callback. The
    // callback owns a reference, so the state stays valid even when the test's
    // bounded wait times out and the completion arrives later.
    struct Completion
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        HRESULT hr = S_FALSE;
    };

    // Holds a callback closed until the test opens it, so a "late completion"
    // is ordered explicitly rather than by timing. Shared with the callback.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool open = false;
        bool callbackWaiting = false;

        void Open()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                open = true;
            }
            cv.notify_all();
        }
    };

    // Opens the gate on every exit path. Declare it after the scheduler so it
    // runs before the scheduler destructor joins the worker; otherwise a failed
    // IC_REQUIRE would leave the worker blocked in the callback and the join
    // would never return.
    struct GateOpener
    {
        std::shared_ptr<Gate> gate;
        ~GateOpener()
        {
            gate->Open();
        }
    };

    // Enqueues a full-resolution decode of `path`. With a `gate`, the callback
    // blocks until the gate is opened before it records the result.
    std::shared_ptr<Completion> StartDecode(
        DecodeScheduler& scheduler,
        const std::wstring& path,
        uint64_t handle,
        std::shared_ptr<Gate> gate = nullptr)
    {
        auto completion = std::make_shared<Completion>();

        DecodeTask task;
        task.request = ImageRequest(path, ImagePurpose::FullResolution);
        task.request.allowGpuCompressedDDS = false;
        task.handle = handle;
        task.callback = [completion, gate](PipelineResult&& result)
        {
            if (gate)
            {
                std::unique_lock<std::mutex> lock(gate->mutex);
                gate->callbackWaiting = true;
                gate->cv.notify_all();
                gate->cv.wait(lock, [&]() { return gate->open; });
            }
            std::lock_guard<std::mutex> lock(completion->mutex);
            completion->hr = result.hr;
            completion->done = true;
            completion->cv.notify_all();
        };
        scheduler.Enqueue(std::move(task));
        return completion;
    }

    // Waits (bounded) for the callback; returns its HRESULT, or S_FALSE on timeout.
    HRESULT WaitDecode(const std::shared_ptr<Completion>& completion, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait_for(lock, timeout, [&]() { return completion->done; });
        return completion->done ? completion->hr : S_FALSE;
    }

    HRESULT DecodeOnce(DecodeScheduler& scheduler, const std::wstring& path, uint64_t handle)
    {
        return WaitDecode(StartDecode(scheduler, path, handle), std::chrono::seconds(10));
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

IC_TEST(Log_TimedOutWaitKeepsCompletionStateAlive)
{
    // A completion that arrives after the test stopped waiting must land in
    // state that is still alive (shared with the callback), not in a destroyed
    // stack frame. The callback is held at a gate, so the wait is guaranteed to
    // time out first; the gate is opened only after the timeout assertions.
    RegisterBuiltInDecoders();
    auto gate = std::make_shared<Gate>();
    DecodeScheduler scheduler;
    scheduler.SetThreadCount(1);
    GateOpener openOnExit { gate };

    auto late = StartDecode(scheduler, MissingNonAcpPath(), 1, gate);

    // The callback has started and is parked at the gate.
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        IC_REQUIRE(gate->cv.wait_for(lock, std::chrono::seconds(10), [&]() { return gate->callbackWaiting; }));
    }
    IC_CHECK(WaitDecode(late, std::chrono::milliseconds(20)) == S_FALSE);   // timed out
    // The caller has given up waiting; the parked callback still holds the state.
    IC_CHECK(late.use_count() >= 2);

    // The late completion arrives and is recorded in the shared state.
    gate->Open();
    IC_CHECK(FAILED(WaitDecode(late, std::chrono::seconds(10))));

    // The worker is unaffected and the callback released its reference.
    IC_CHECK(FAILED(DecodeOnce(scheduler, MissingNonAcpPath(), 2)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (late.use_count() != 1 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    IC_CHECK(late.use_count() == 1);
}
