#include "TestHarness.h"

#include "DecodeScheduler.h"
#include "FileByteCache.h"
#include "IPathByteSource.h"
#include "ImageCore.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// DecodeScheduler worker-count and single-worker progress tests. They decode
// tiny BMP files from a temporary directory through the real dispatcher.
// Completion waits are bounded; ordering assertions only depend on queue
// state that is established before the worker is released.

namespace
{
    using namespace ImageCore;

    constexpr auto kWaitTimeout = std::chrono::seconds(10);

    // Writes a top-down 32bpp BI_RGB bitmap.
    void WriteBmp(const std::filesystem::path& path, uint32_t width, uint32_t height)
    {
        const uint32_t pixelBytes = width * height * 4u;

        BITMAPFILEHEADER file {};
        file.bfType = 0x4D42; // 'BM'
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + pixelBytes;

        BITMAPINFOHEADER info {};
        info.biSize = sizeof(BITMAPINFOHEADER);
        info.biWidth = static_cast<LONG>(width);
        info.biHeight = -static_cast<LONG>(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        info.biSizeImage = pixelBytes;

        std::vector<uint8_t> pixels(pixelBytes, 0x80);

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    }

    struct Fixture
    {
        std::filesystem::path dir;
        std::vector<std::wstring> thumbs;
        std::wstring fullRes;

        Fixture()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) /
                (L"ImageCoreTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);

            // Enough distinct files to exceed the prefetch cap (at most 8) several
            // times over; same-path tasks share one FileByteCache entry and would
            // not exercise backpressure.
            for (int i = 0; i < 40; ++i)
            {
                const auto path = dir / (L"thumb" + std::to_wstring(i) + L".bmp");
                WriteBmp(path, 32, 32);
                thumbs.push_back(path.wstring());
            }
            WriteBmp(dir / L"full.bmp", 16, 8);
            fullRes = (dir / L"full.bmp").wstring();

            RegisterBuiltInDecoders();
            ImageCoreTest::RegisterTeardown([this]()
            {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            });
        }
    };

    Fixture& Files()
    {
        static Fixture fixture;
        return fixture;
    }

    // Collects completions in arrival order.
    struct Completions
    {
        struct Entry
        {
            uint64_t handle { 0 };
            HRESULT hr { E_FAIL };
            int threadPriority { 0 };
        };

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<Entry> entries;

        std::function<void(PipelineResult&&)> Callback(uint64_t handle)
        {
            return [this, handle](PipelineResult&& result)
            {
                const int priority = GetThreadPriority(GetCurrentThread());
                std::lock_guard<std::mutex> lock(mutex);
                entries.push_back({ handle, result.hr, priority });
                cv.notify_all();
            };
        }

        bool WaitFor(size_t count)
        {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, kWaitTimeout, [&]() { return entries.size() >= count; });
        }
    };

    // Fails the process with a message if a scope outlives `limit`. Scheduler
    // destruction joins its threads, which cannot be bounded from the test; a
    // regression of the shutdown wakeup would otherwise hang until the CTest
    // timeout with no diagnostic.
    class Watchdog
    {
    public:
        Watchdog(const char* what, std::chrono::seconds limit)
            : m_done(CreateEventW(nullptr, TRUE, FALSE, nullptr))
            , m_thread([this, what, limit]()
            {
                if (WaitForSingleObject(m_done, static_cast<DWORD>(limit.count() * 1000)) != WAIT_OBJECT_0)
                {
                    std::fprintf(stderr, "  watchdog: %s did not finish within %lld s; terminating\n",
                        what, static_cast<long long>(limit.count()));
                    std::fflush(stderr);
                    TerminateProcess(GetCurrentProcess(), 3);
                }
            })
        {
        }

        ~Watchdog()
        {
            SetEvent(m_done);
            m_thread.join();
            CloseHandle(m_done);
        }

    private:
        HANDLE m_done;
        std::thread m_thread;
    };

    // Opens a worker gate on every exit path. Declare it after the scheduler so
    // it runs before the scheduler destructor joins the worker; otherwise a
    // failed IC_REQUIRE would leave the worker blocked in the gated callback and
    // the join would never return.
    struct GateRelease
    {
        std::mutex& mutex;
        std::condition_variable& cv;
        bool& flag;
        ~GateRelease()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                flag = true;
            }
            cv.notify_all();
        }
    };

    // Filesystem byte source for the tests. Without one, the prefetch stage reads
    // nothing (ReadWholeFileBytes returns null), FileByteCache stays empty and the
    // capacity wait never blocks. Declare it before the scheduler so it outlives
    // every scheduler thread; the previous source is restored on destruction.
    class ScopedFileByteSource : public IPathByteSource
    {
    public:
        ScopedFileByteSource()
            : m_previous(GetPathByteSource())
        {
            SetPathByteSource(this);
        }

        ~ScopedFileByteSource() override
        {
            SetPathByteSource(m_previous);
        }

        std::wstring ResolveHostPath(const std::wstring& path) const override
        {
            return path;
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

    private:
        IPathByteSource* m_previous;
    };

    DecodeTask MakeTask(const std::wstring& path, ImagePurpose purpose, uint64_t handle,
        std::function<void(PipelineResult&&)> callback)
    {
        DecodeTask task;
        task.request = ImageRequest(path, purpose);
        task.request.allowGpuCompressedDDS = false;
        if (purpose != ImagePurpose::FullResolution)
        {
            task.request.targetSize = { 16.0f, 16.0f };
        }
        task.handle = handle;
        task.callback = std::move(callback);
        return task;
    }
}

IC_TEST(DecodeScheduler_DefaultWorkerCountNeverZero)
{
    IC_CHECK(DecodeScheduler::DefaultWorkerCount(0) == 1);
    IC_CHECK(DecodeScheduler::DefaultWorkerCount(1) == 1);
    IC_CHECK(DecodeScheduler::DefaultWorkerCount(2) == 1);
    IC_CHECK(DecodeScheduler::DefaultWorkerCount(16) == 15);
}

IC_TEST(DecodeScheduler_WorkerCountMatchesConfiguration)
{
    DecodeScheduler scheduler;

    scheduler.SetThreadCount(1);
    IC_CHECK(scheduler.WorkerCount() == 1);

    scheduler.SetThreadCount(3);
    IC_CHECK(scheduler.WorkerCount() == 3);

    // Requests above the cap are clamped (one dedicated high-priority worker plus
    // at most four background workers).
    scheduler.SetThreadCount(DecodeScheduler::kMaxDecodeWorkers);
    IC_CHECK(scheduler.WorkerCount() == DecodeScheduler::kMaxDecodeWorkers);
    scheduler.SetThreadCount(DecodeScheduler::kMaxDecodeWorkers + 5);
    IC_CHECK(scheduler.WorkerCount() == DecodeScheduler::kMaxDecodeWorkers);

    scheduler.SetThreadCount(0);
    IC_CHECK(scheduler.WorkerCount() == 1);
}

IC_TEST(DecodeScheduler_SingleWorkerCompletesThumbnails)
{
    // Regression: with one configured worker (the default on a two-hardware-thread
    // machine) thumbnail work used to wait forever because the only worker served
    // the high-priority queue exclusively.
    const auto& files = Files();
    Completions done;
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(DecodeScheduler::DefaultWorkerCount(2));
        IC_REQUIRE(scheduler.WorkerCount() == 1);

        uint64_t handle = 0;
        for (const auto& path : files.thumbs)
        {
            ++handle;
            scheduler.Enqueue(MakeTask(path, ImagePurpose::Thumbnail, handle, done.Callback(handle)));
        }

        IC_REQUIRE(done.WaitFor(files.thumbs.size()));
    }

    for (const auto& entry : done.entries)
    {
        IC_CHECK(SUCCEEDED(entry.hr));
    }
}

IC_TEST(DecodeScheduler_SingleWorkerPrioritiesPerTask)
{
    const auto& files = Files();
    Completions done;
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);

        scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 1, done.Callback(1)));
        IC_REQUIRE(done.WaitFor(1));
        scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2, done.Callback(2)));
        IC_REQUIRE(done.WaitFor(2));
        scheduler.Enqueue(MakeTask(files.thumbs[1], ImagePurpose::Thumbnail, 3, done.Callback(3)));
        IC_REQUIRE(done.WaitFor(3));
    }

    // Background work runs below normal priority; full-resolution work keeps the
    // worker's above-normal priority, including after a background task.
    IC_CHECK(done.entries[0].handle == 1);
    IC_CHECK(done.entries[0].threadPriority < THREAD_PRIORITY_NORMAL);
    IC_CHECK(done.entries[1].handle == 2);
    IC_CHECK(done.entries[1].threadPriority == THREAD_PRIORITY_ABOVE_NORMAL);
    IC_CHECK(done.entries[2].handle == 3);
    IC_CHECK(done.entries[2].threadPriority < THREAD_PRIORITY_NORMAL);
}

IC_TEST(DecodeScheduler_SingleWorkerPrefersQueuedFullResolution)
{
    const auto& files = Files();
    Completions done;

    // Hold the only worker inside the first thumbnail callback.
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;

    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        GateRelease releaseOnExit { gateMutex, gateCv, released };

        auto gatedCallback = [&, inner = done.Callback(1)](PipelineResult&& result) mutable
        {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                entered = true;
                gateCv.notify_all();
                gateCv.wait(lock, [&]() { return released; });
            }
            inner(std::move(result));
        };
        scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 1, gatedCallback));

        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }

        // Queue a thumbnail backlog, then a full-resolution request, while the
        // worker is busy. The full-resolution request is in the high queue before
        // the worker is released, so it must run before any queued thumbnail.
        for (uint64_t handle = 2; handle <= 5; ++handle)
        {
            scheduler.Enqueue(MakeTask(files.thumbs[handle], ImagePurpose::Thumbnail, handle, done.Callback(handle)));
        }
        scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 100, done.Callback(100)));

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();

        IC_REQUIRE(done.WaitFor(6));
    }

    IC_REQUIRE(done.entries.size() == 6);
    IC_CHECK(done.entries[0].handle == 1);
    IC_CHECK(done.entries[1].handle == 100);
    for (const auto& entry : done.entries)
    {
        IC_CHECK(SUCCEEDED(entry.hr));
    }
}

IC_TEST(DecodeScheduler_BackpressureBlocksAtCapAndResumes)
{
    // The prefetch thread fills FileByteCache up to the effective cap and then
    // blocks until a decode frees space. With the only decode worker held inside
    // a callback, the cache must fill to the cap and stay there (blocked); once
    // the worker is released, every decode must wake the prefetch thread. A lost
    // cache-space wakeup leaves the remaining thumbnails queued forever, which
    // shows up here as a bounded-wait failure.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;

    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;
    size_t plateau = 0;
    size_t maxEntries = 0;

    {
        Watchdog watchdog("backpressure test", std::chrono::seconds(60));
        ScopedFileByteSource byteSource;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        GateRelease releaseOnExit { gateMutex, gateCv, released };

        auto gatedCallback = [&, inner = done.Callback(1)](PipelineResult&& result) mutable
        {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                entered = true;
                gateCv.notify_all();
                gateCv.wait(lock, [&]() { return released; });
            }
            inner(std::move(result));
        };
        scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 1, gatedCallback));
        for (size_t i = 1; i < files.thumbs.size(); ++i)
        {
            const uint64_t handle = static_cast<uint64_t>(i) + 1;
            scheduler.Enqueue(MakeTask(files.thumbs[i], ImagePurpose::Thumbnail, handle, done.Callback(handle)));
        }
        maxEntries = cache.MaxEntries();

        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }

        // Fill phase: the count must rise to at least one entry (the byte source
        // is in effect) and then stay flat for 500 ms while 38 thumbnails are
        // still queued, i.e. the prefetch thread is blocked on capacity.
        {
            const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            size_t last = 0;
            auto lastChange = std::chrono::steady_clock::now();
            for (;;)
            {
                const size_t count = cache.Count();
                const auto now = std::chrono::steady_clock::now();
                if (count != last)
                {
                    last = count;
                    lastChange = now;
                }
                if (last >= 1 && now - lastChange >= std::chrono::milliseconds(500))
                {
                    break;
                }
                IC_REQUIRE(now < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            plateau = last;
        }
        IC_CHECK(plateau >= 1);
        IC_CHECK(plateau <= maxEntries);              // held by the capacity check,
        IC_CHECK(plateau < files.thumbs.size());      // not by running out of work
        {
            std::lock_guard<std::mutex> lock(done.mutex);
            IC_CHECK(done.entries.empty());           // nothing decoded while held
        }

        // Resume phase: release the worker; each completed decode erases its
        // bytes and must wake the blocked prefetch thread.
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        IC_REQUIRE(done.WaitFor(files.thumbs.size()));

        // Every prefetched entry is released once its decode finished.
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (cache.Count() != 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        IC_CHECK(cache.Count() == 0);
    }

    std::printf("  backpressure: cache plateaued at %zu entries (cap %zu) with %zu thumbnails queued\n",
        plateau, maxEntries, files.thumbs.size() - 1);
    IC_CHECK(done.entries.size() == files.thumbs.size());
    for (const auto& entry : done.entries)
    {
        IC_CHECK(SUCCEEDED(entry.hr));
    }
}

// Kept last: it creates and destroys many schedulers.
IC_TEST(DecodeScheduler_ShutdownCyclesNeverHang)
{
    // Shutdown with idle workers (the case that hung: a waiter that evaluated
    // its predicate just before the flag flipped) and with queued and in-flight
    // full-resolution work. Full-resolution tasks bypass the FileByteCache, so
    // dropped tasks leave no stale cache entries behind for later cycles.
    const auto& files = Files();
    constexpr int kCycles = 1000;
    Watchdog watchdog("shutdown cycles", std::chrono::seconds(90));

    int completed = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        Completions done;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1 + (cycle % 2));

        const int mode = cycle % 3;
        if (mode != 0)
        {
            // Queued work at shutdown (mode 1) or work allowed to complete (mode 2).
            scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, done.Callback(1)));
            scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2, done.Callback(2)));
            if (mode == 2)
            {
                IC_REQUIRE(done.WaitFor(2));
                completed += 2;
            }
        }
        // mode 0: idle shutdown, every thread blocked in its wait.
    }

    IC_CHECK(completed == 2 * (kCycles / 3));
}
