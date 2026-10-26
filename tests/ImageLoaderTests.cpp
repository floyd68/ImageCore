#include "TestHarness.h"

#include "ImageCore.h"
#include "ImageLoader.h"
#include "IPathByteSource.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ImageLoader shutdown: requests after (and racing with) Shutdown fail safely.
// ImageLoader is a process singleton and Shutdown is final, so these tests run
// as their own CTest entry (ImageCore.ImageLoader).

namespace
{
    using namespace ImageCore;

    std::wstring WriteTinyBmp()
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const auto path = std::filesystem::path(tempPath) /
            (L"ImageCoreLoaderTests_" + std::to_wstring(GetCurrentProcessId()) + L".bmp");

        BITMAPFILEHEADER file {};
        BITMAPINFOHEADER info {};
        const uint32_t pixelBytes = 4u * 4u * 4u;
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + pixelBytes;
        info.biSize = sizeof(BITMAPINFOHEADER);
        info.biWidth = 4;
        info.biHeight = -4;
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = pixelBytes;
        std::vector<uint8_t> pixels(pixelBytes, 0x40);

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

        ImageCoreTest::RegisterTeardown([path]()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        });
        return path.wstring();
    }

    ImageRequest MakeRequest(const std::wstring& path)
    {
        ImageRequest request(path, ImagePurpose::FullResolution);
        request.allowGpuCompressedDDS = false;
        return request;
    }
}

IC_TEST(ImageLoader_RequestsAfterShutdownFailSafely)
{
    RegisterBuiltInDecoders();
    const std::wstring bmp = WriteTinyBmp();
    ImageLoader& loader = ImageLoader::Instance();

    // Works before shutdown.
    std::mutex mutex;
    std::condition_variable cv;
    bool delivered = false;
    HRESULT deliveredHr = E_PENDING;
    const ImageHandle first = loader.RequestDecoded(MakeRequest(bmp), [&](HRESULT hr, DecodedImage)
    {
        std::lock_guard<std::mutex> lock(mutex);
        delivered = true;
        deliveredHr = hr;
        cv.notify_all();
    });
    IC_REQUIRE(first != 0);
    {
        std::unique_lock<std::mutex> lock(mutex);
        IC_REQUIRE(cv.wait_for(lock, std::chrono::seconds(10), [&]() { return delivered; }));
    }
    IC_CHECK(SUCCEEDED(deliveredHr));
    IC_CHECK(loader.LiveRequestCount() == 0);

    // Concurrent requests while Shutdown runs: each is either accepted (and may
    // or may not complete) or rejected with 0; nothing crashes or hangs.
    std::atomic<bool> stop { false };
    std::atomic<int> rejected { 0 };
    std::thread requester([&]()
    {
        while (!stop.load())
        {
            const ImageHandle handle = loader.RequestDecoded(MakeRequest(bmp), [](HRESULT, DecodedImage) {});
            if (handle == 0)
            {
                rejected.fetch_add(1);
            }
            else
            {
                loader.Cancel(handle);
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loader.Shutdown();
    // Let the requester observe the shut-down loader before stopping it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rejected.load() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    requester.join();
    IC_CHECK(rejected.load() > 0);

    // After shutdown: rejected without a callback; Cancel and Shutdown are no-ops.
    std::atomic<bool> lateCallback { false };
    const ImageHandle late = loader.RequestDecoded(MakeRequest(bmp), [&](HRESULT, DecodedImage)
    {
        lateCallback.store(true);
    });
    IC_CHECK(late == 0);
    IC_CHECK(loader.LiveRequestCount() == 0);
    loader.Cancel(first);
    loader.Cancel(12345);
    loader.SetWorkerThreadCount(2);
    loader.Shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    IC_CHECK(!lateCallback.load());
}

// ---------------------------------------------------------------------------
// Bounded and non-constructing shutdown. Each case runs in a fresh child
// process: this process already created (and shut down) the loader above.

namespace
{
    // Child exit codes: 0 = pass; anything else names the failed step.
    constexpr unsigned long kChildTimeoutMs = 30000;

    // Threads started in this process since it began: counted by a TLS callback,
    // which runs for every new thread, so a thread that was started and already
    // joined is still seen (a thread snapshot would miss it).
    std::atomic<long> g_threadStarts { 0 };

    void NTAPI CountThreadStarts(PVOID, DWORD reason, PVOID)
    {
        if (reason == DLL_THREAD_ATTACH)
        {
            g_threadStarts.fetch_add(1);
        }
    }
}

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:g_imageCoreTestTlsCallback")
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK g_imageCoreTestTlsCallback = CountThreadStarts;
#pragma const_seg()

namespace
{

    std::unordered_set<DWORD> ProcessThreadIds()
    {
        std::unordered_set<DWORD> ids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return ids;
        }
        THREADENTRY32 entry { sizeof(entry) };
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
            {
                ids.insert(entry.th32ThreadID);
            }
        }
        CloseHandle(snapshot);
        return ids;
    }

    // Archive-like source for the children: `<bmp>.zip\entry.bmp` resolves to
    // a different host path (so a full-resolution request reads it through the
    // source) and reads the real BMP. `cooperative` reads block until stop is
    // requested; otherwise they block on an event, ignoring the token and OS
    // cancellation, until Release().
    class BlockingLoaderSource final : public IPathByteSource
    {
    public:
        BlockingLoaderSource(std::wstring bmp, bool cooperative)
            : m_bmp(std::move(bmp))
            , m_cooperative(cooperative)
            , m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
            , m_entered(CreateEventW(nullptr, TRUE, FALSE, nullptr))
        {
        }

        ~BlockingLoaderSource() override
        {
            CloseHandle(m_event);
            CloseHandle(m_entered);
        }

        std::wstring EntryPath() const { return m_bmp + L".zip\\entry.bmp"; }

        std::wstring ResolveHostPath(const std::wstring& path) const override
        {
            return path == EntryPath() ? m_bmp + L".zip" : path;
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            (void)path;
            std::ifstream in(std::filesystem::path(m_bmp), std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override
        {
            SetEvent(m_entered);
            if (m_cooperative)
            {
                std::mutex mutex;
                std::condition_variable cv;
                std::stop_callback wake(stop, [&]
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    cv.notify_all();
                });
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::seconds(10), [&] { return stop.stop_requested(); });
                return {};
            }
            (void)WaitForSingleObject(m_event, 10000);
            return ReadAll(path);
        }

        bool WaitEntered() const { return WaitForSingleObject(m_entered, 10000) == WAIT_OBJECT_0; }
        void Release() const { SetEvent(m_event); }

    private:
        std::wstring m_bmp;
        bool m_cooperative;
        HANDLE m_event;
        HANDLE m_entered;
    };

    // The tiny BMP, removed at exit (teardowns do not run in a child).
    struct ChildBmp
    {
        std::wstring path { WriteTinyBmp() };
        ~ChildBmp()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    };

    long long ElapsedMs(std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
    }

    // Full-resolution request through `source` that records any callback.
    ImageHandle RequestEntry(BlockingLoaderSource& source, std::atomic<int>& callbacks)
    {
        ImageRequest request(source.EntryPath(), ImagePurpose::FullResolution);
        request.allowGpuCompressedDDS = false;
        return ImageLoader::Instance().RequestDecoded(request, [&callbacks](HRESULT, DecodedImage)
        {
            callbacks.fetch_add(1);
        });
    }
}

// Child: ImageCore is never used. ShutdownIfCreated must not construct the
// loader (and so its scheduler) or start any thread.
IC_CHILD(LoaderNeverUsedChild)
{
    (void)argc;
    (void)argv;
    const auto before = ProcessThreadIds();
    const long startsBefore = g_threadStarts.load();
    if (ImageLoader::IsInstantiated())
    {
        return 10;
    }
    if (!ImageLoader::ShutdownIfCreated(std::chrono::milliseconds(1000)))
    {
        return 11;
    }
    if (!ImageLoader::ShutdownIfCreated(std::chrono::milliseconds(0)))  // repeated
    {
        return 12;
    }
    if (ImageLoader::IsInstantiated())
    {
        return 13;
    }
    for (DWORD id : ProcessThreadIds())
    {
        if (before.count(id) == 0)
        {
            return 14;  // a thread was started and is still running
        }
    }
    if (g_threadStarts.load() != startsBefore)
    {
        return 15;  // a thread was started (even if already joined)
    }
    std::thread([] {}).join();
    if (g_threadStarts.load() != startsBefore + 1)
    {
        return 16;  // the counter does not see thread starts: the check above proves nothing
    }
    return 0;
}

// Child: a read blocked on its token. ShutdownIfCreated behaves like
// Shutdown(bound): it releases the read, joins within the bound and invokes no
// callback.
IC_CHILD(LoaderCooperativeReadChild)
{
    (void)argc;
    (void)argv;
    RegisterBuiltInDecoders();
    ChildBmp bmp;
    BlockingLoaderSource source(bmp.path, true);
    SetPathByteSource(&source);
    std::atomic<int> callbacks { 0 };
    if (RequestEntry(source, callbacks) == 0)
    {
        return 20;
    }
    if (!source.WaitEntered() || !ImageLoader::IsInstantiated())
    {
        return 21;
    }
    const auto start = std::chrono::steady_clock::now();
    if (!ImageLoader::ShutdownIfCreated(std::chrono::milliseconds(5000)))
    {
        return 22;
    }
    if (ElapsedMs(start) >= 2000)
    {
        return 23;
    }
    if (RequestEntry(source, callbacks) != 0 || ImageLoader::Instance().LiveRequestCount() != 0)
    {
        return 24;
    }
    if (!ImageLoader::ShutdownIfCreated(std::chrono::milliseconds(100)))
    {
        return 25;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SetPathByteSource(nullptr);
    return callbacks.load() == 0 ? 0 : 26;
}

// Child: a read that ignores its token and OS cancellation. ShutdownIfCreated
// returns false after about the bound and keeps rejecting requests; once the
// read is released a retry joins, and the parameterless Shutdown stays harmless.
IC_CHILD(LoaderStuckReadChild)
{
    (void)argc;
    (void)argv;
    RegisterBuiltInDecoders();
    ChildBmp bmp;
    BlockingLoaderSource source(bmp.path, false);
    SetPathByteSource(&source);
    std::atomic<int> callbacks { 0 };
    if (RequestEntry(source, callbacks) == 0)
    {
        return 30;
    }
    if (!source.WaitEntered())
    {
        return 31;
    }
    const auto start = std::chrono::steady_clock::now();
    if (ImageLoader::ShutdownIfCreated(std::chrono::milliseconds(300)))
    {
        return 32;
    }
    const long long elapsed = ElapsedMs(start);
    if (elapsed < 290 || elapsed >= 3000)
    {
        return 33;
    }
    if (RequestEntry(source, callbacks) != 0)
    {
        return 34;  // must keep rejecting requests
    }
    if (ImageLoader::Instance().LiveRequestCount() != 1)
    {
        return 35;  // the scheduler (with the stuck request) is kept, not dropped
    }
    source.Release();
    if (!ImageLoader::ShutdownIfCreated(std::chrono::milliseconds(5000)))
    {
        return 36;
    }
    if (ImageLoader::Instance().LiveRequestCount() != 0 || !ImageLoader::Instance().Shutdown(std::chrono::milliseconds(0)))
    {
        return 37;
    }
    ImageLoader::Instance().Shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SetPathByteSource(nullptr);
    return callbacks.load() == 0 ? 0 : 38;
}

IC_TEST(ImageLoader_ShutdownIfCreatedNeverConstructsUnusedLoader)
{
    IC_CHECK(ImageCoreTest::RunChild("LoaderNeverUsedChild", kChildTimeoutMs) == 0);
}

IC_TEST(ImageLoader_ShutdownIfCreatedReleasesCooperativeRead)
{
    IC_CHECK(ImageCoreTest::RunChild("LoaderCooperativeReadChild", kChildTimeoutMs) == 0);
}

IC_TEST(ImageLoader_ShutdownIfCreatedTimesOutThenRetries)
{
    IC_CHECK(ImageCoreTest::RunChild("LoaderStuckReadChild", kChildTimeoutMs) == 0);
}
