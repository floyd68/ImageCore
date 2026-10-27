#include "TestHarness.h"

#include "DecodeScheduler.h"
#include "FileByteCache.h"
#include "IPathByteSource.h"
#include "ImageCore.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <cstdio>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

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
            {
                std::lock_guard<std::mutex> lock(m_readMutex);
                m_reads.push_back(path);
            }
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

        // Read-side history: the prefetch stage is the only caller for
        // thumbnail requests, so this observes exactly when prefetch stops and
        // resumes, independently of FileByteCache (which evicts at its limit and
        // so cannot show whether prefetch waited).
        size_t ReadCount() const
        {
            std::lock_guard<std::mutex> lock(m_readMutex);
            return m_reads.size();
        }

        std::vector<std::wstring> Reads() const
        {
            std::lock_guard<std::mutex> lock(m_readMutex);
            return m_reads;
        }

    private:
        IPathByteSource* m_previous;
        mutable std::mutex m_readMutex;
        mutable std::vector<std::wstring> m_reads;
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
    // The prefetch thread reads thumbnail bytes into FileByteCache and must stop
    // reading when the cache holds the effective cap, until a decode frees space.
    // The test observes the read side directly (ScopedFileByteSource history):
    // FileByteCache::Put evicts at its limit, so a flat cache count alone would
    // also appear if prefetch never waited. With the only decode worker held
    // inside a callback:
    //   fill   - reads stop at exactly the effective cap and do not continue;
    //            nothing was evicted (cache count == reads), nothing decoded;
    //   resume - after the worker is released, reads continue until every file
    //            has been read exactly once, in enqueue order, and all decodes
    //            complete. A lost cache-space wakeup leaves reads stuck, which
    //            fails the bounded wait.
    // With all inputs on one volume, the effective cap is FileByteCache's limit:
    // the global cap is min(8, that volume's prefetch depth) and the per-volume
    // cap is that depth, so the smaller of the two is the global value.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;

    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;
    size_t readsWhileHeld = 0;
    size_t cacheWhileHeld = 0;
    size_t effectiveCap = 0;
    std::vector<std::wstring> reads;

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
        // Enqueue retunes the cap synchronously; read it after the last enqueue.
        effectiveCap = cache.MaxEntries();
        IC_REQUIRE(effectiveCap >= 1);
        IC_REQUIRE(effectiveCap < files.thumbs.size());

        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }

        // Fill phase: wait until the read count stops changing for 500 ms.
        {
            const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            size_t last = byteSource.ReadCount();
            auto lastChange = std::chrono::steady_clock::now();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const size_t count = byteSource.ReadCount();
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
            }
            readsWhileHeld = last;
        }
        cacheWhileHeld = cache.Count();

        // Prefetch stopped at the cap: exactly `effectiveCap` files read, all of
        // them still cached (no eviction happened), 39 thumbnails still queued,
        // and nothing decoded while the worker was held.
        IC_CHECK(readsWhileHeld == effectiveCap);
        IC_CHECK(cacheWhileHeld == readsWhileHeld);
        {
            std::lock_guard<std::mutex> lock(done.mutex);
            IC_CHECK(done.entries.empty());
        }
        // ...and it stays stopped: no further read starts before the release.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        IC_CHECK(byteSource.ReadCount() == readsWhileHeld);

        // Resume phase: release the worker; each completed decode frees cache
        // space and must wake the prefetch thread for the next read.
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        IC_REQUIRE(done.WaitFor(files.thumbs.size()));

        {
            const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (cache.Count() != 0 && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        IC_CHECK(cache.Count() == 0);
        reads = byteSource.Reads();
    }

    std::printf("  backpressure: reads stopped at %zu (effective cap %zu, cached %zu) with %zu thumbnails queued; %zu reads after resume\n",
        readsWhileHeld, effectiveCap, cacheWhileHeld, files.thumbs.size() - readsWhileHeld,
        reads.size() - readsWhileHeld);

    // Reads resumed after release and covered every file exactly once, in
    // enqueue order (the prefetch stage is FIFO).
    IC_REQUIRE(reads.size() == files.thumbs.size());
    for (size_t i = 0; i < reads.size(); ++i)
    {
        IC_CHECK(reads[i] == files.thumbs[i]);
    }
    IC_CHECK(done.entries.size() == files.thumbs.size());
    for (const auto& entry : done.entries)
    {
        IC_CHECK(SUCCEEDED(entry.hr));
    }
}

namespace
{
    // Byte source with controllable timing. `archivePath` resolves to a
    // different host path, so the scheduler treats it as archive-backed and a
    // full-resolution decode reads it through ReadAll (inside the decode step).
    // `gatedRead` blocks ReadAll and `gatedResolve` blocks ResolveHostPath for one
    // path until Release(); every wait is bounded.
    class GatedByteSource : public IPathByteSource
    {
    public:
        GatedByteSource()
            : m_previous(GetPathByteSource())
        {
            SetPathByteSource(this);
        }

        ~GatedByteSource() override
        {
            Release();
            SetPathByteSource(m_previous);
        }

        std::wstring archivePath;
        std::wstring gatedRead;
        std::wstring gatedResolve;

        std::wstring ResolveHostPath(const std::wstring& path) const override
        {
            if (!gatedResolve.empty() && path == gatedResolve)
            {
                Block();
            }
            return (!archivePath.empty() && path == archivePath) ? path + L".host" : path;
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            if (!gatedRead.empty() && path == gatedRead)
            {
                Block();
            }
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

        bool WaitEntered() const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, kWaitTimeout, [&]() { return m_entered; });
        }

        void Release() const
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_released = true;
            }
            m_cv.notify_all();
        }

    private:
        void Block() const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_entered = true;
            m_cv.notify_all();
            m_cv.wait_for(lock, kWaitTimeout, [&]() { return m_released; });
        }

        IPathByteSource* m_previous;
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        mutable bool m_entered = false;
        mutable bool m_released = false;
    };

    // Waits (bounded) until `predicate` holds.
    template <typename Predicate>
    bool WaitUntil(Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    bool Delivered(Completions& done, uint64_t handle)
    {
        std::lock_guard<std::mutex> lock(done.mutex);
        for (const auto& entry : done.entries)
        {
            if (entry.handle == handle)
            {
                return true;
            }
        }
        return false;
    }
}

IC_TEST(DecodeScheduler_LateCancelLeavesNoState)
{
    // Cancel after completion and of unknown handles is a no-op; finished
    // requests leave nothing in the live-request table.
    const auto& files = Files();
    Completions done;
    DecodeScheduler scheduler;
    scheduler.SetThreadCount(2);

    constexpr uint64_t kCount = 20;
    for (uint64_t handle = 1; handle <= kCount; ++handle)
    {
        const bool full = (handle % 2) == 0;
        const std::wstring& path = full ? files.fullRes : files.thumbs[handle % files.thumbs.size()];
        IC_REQUIRE(scheduler.Enqueue(MakeTask(path, full ? ImagePurpose::FullResolution : ImagePurpose::Thumbnail,
            handle, done.Callback(handle))));
    }
    IC_REQUIRE(done.WaitFor(kCount));
    IC_REQUIRE(WaitUntil([&]() { return scheduler.LiveRequestCount() == 0; }));

    for (uint64_t handle = 1; handle <= kCount; ++handle)
    {
        scheduler.Cancel(handle);
    }
    for (uint64_t handle = 1000; handle < 3000; ++handle)
    {
        scheduler.Cancel(handle);
    }
    IC_CHECK(scheduler.LiveRequestCount() == 0);
}

IC_TEST(DecodeScheduler_CancelReloadStressStaysBounded)
{
    // Rapid request/cancel/re-request, like fast main-image navigation with
    // thumbnails in flight. Cancellation state must drain to zero, and every
    // delivered callback must belong to a request that was issued.
    const auto& files = Files();
    Watchdog watchdog("cancel/reload stress", std::chrono::seconds(60));
    Completions done;
    DecodeScheduler scheduler;
    scheduler.SetThreadCount(3);

    constexpr uint64_t kRounds = 300;
    size_t peakLive = 0;
    for (uint64_t round = 0; round < kRounds; ++round)
    {
        const uint64_t full = round * 2 + 1;
        const uint64_t thumb = round * 2 + 2;
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, full, done.Callback(full))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[round % files.thumbs.size()], ImagePurpose::Thumbnail,
            thumb, done.Callback(thumb))));
        // Supersede the previous main image (sometimes this one immediately) and
        // some thumbnails; an occasional pause lets cancels land on queued, decoding,
        // and already-delivered requests alike.
        if (round > 0)
        {
            scheduler.Cancel(full - 2);
            if (round % 2 == 0)
            {
                scheduler.Cancel(thumb - 2);
            }
        }
        if (round % 3 == 0)
        {
            scheduler.Cancel(full);
        }
        peakLive = (std::max)(peakLive, scheduler.LiveRequestCount());
        if (round % 4 == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    IC_REQUIRE(WaitUntil([&]() { return scheduler.LiveRequestCount() == 0; }));
    // The final round's thumbnail was never canceled and must be delivered.
    IC_REQUIRE(WaitUntil([&]() { return Delivered(done, kRounds * 2); }));

    std::lock_guard<std::mutex> lock(done.mutex);
    IC_CHECK(done.entries.size() <= kRounds * 2);
    for (const auto& entry : done.entries)
    {
        IC_CHECK(entry.handle >= 1 && entry.handle <= kRounds * 2);
    }
    std::printf("  cancel/reload: %zu callbacks for %llu requests, peak live %zu\n",
        done.entries.size(), static_cast<unsigned long long>(kRounds * 2), peakLive);
}

IC_TEST(DecodeScheduler_QueuedCancelSuppressesCallback)
{
    const auto& files = Files();
    Completions done;
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
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, gatedCallback)));
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }

        // Task 1 is being delivered (it already left the live table); 2 and 3 queue.
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2, done.Callback(2))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 3, done.Callback(3))));
        IC_CHECK(scheduler.LiveRequestCount() == 2);
        scheduler.Cancel(2);
        scheduler.Cancel(3);
        scheduler.Cancel(1);  // late: delivery already started, so it is a no-op
        IC_CHECK(scheduler.LiveRequestCount() == 0);
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 4, done.Callback(4))));

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        IC_REQUIRE(done.WaitFor(2));
        IC_REQUIRE(WaitUntil([&]() { return scheduler.LiveRequestCount() == 0; }));
    }

    // Only the in-delivery task 1 (a callback after its Cancel returned, which
    // the contract allows) and the uncanceled task 4 were delivered.
    IC_REQUIRE(done.entries.size() == 2);
    IC_CHECK(done.entries[0].handle == 1);
    IC_CHECK(done.entries[1].handle == 4);
}

IC_TEST(DecodeScheduler_CancelDuringDecodeSuppressesCallback)
{
    // The decode of task 1 is held inside the archive read (after dequeue,
    // before delivery). A cancel issued then must suppress its callback.
    const auto& files = Files();
    Completions done;
    {
        Watchdog watchdog("cancel during decode", std::chrono::seconds(60));
        GatedByteSource source;
        source.archivePath = files.thumbs[30];
        source.gatedRead = files.thumbs[30];
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[30], ImagePurpose::FullResolution, 1, done.Callback(1))));
        IC_REQUIRE(source.WaitEntered());
        scheduler.Cancel(1);
        IC_CHECK(scheduler.LiveRequestCount() == 0);
        source.Release();

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2, done.Callback(2))));
        IC_REQUIRE(done.WaitFor(1));
    }

    IC_REQUIRE(done.entries.size() == 1);
    IC_CHECK(done.entries[0].handle == 2);
}

IC_TEST(DecodeScheduler_StolenFullResolutionRunsAboveNormal)
{
    // With one worker held, the other worker takes the next full-resolution
    // task; with two workers one of them is the background worker. Full-
    // resolution work must never run in background mode.
    const auto& files = Files();
    Completions done;
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;
    DWORD gatedThread = 0;
    DWORD otherThread = 0;
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(2);  // one high-priority worker + one background worker
        GateRelease releaseOnExit { gateMutex, gateCv, released };

        auto gatedCallback = [&, inner = done.Callback(1)](PipelineResult&& result) mutable
        {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                gatedThread = GetCurrentThreadId();
                entered = true;
                gateCv.notify_all();
                gateCv.wait(lock, [&]() { return released; });
            }
            inner(std::move(result));
        };
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, gatedCallback)));
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }

        auto inner2 = done.Callback(2);
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2,
            [&, inner2](PipelineResult&& result) mutable
            {
                otherThread = GetCurrentThreadId();
                inner2(std::move(result));
            })));
        IC_REQUIRE(done.WaitFor(1));

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        IC_REQUIRE(done.WaitFor(2));

        // Background work afterwards still runs in background mode.
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[2], ImagePurpose::Thumbnail, 3, done.Callback(3))));
        IC_REQUIRE(done.WaitFor(3));
    }

    IC_CHECK(gatedThread != 0 && otherThread != 0 && gatedThread != otherThread);
    IC_REQUIRE(done.entries.size() == 3);
    for (const auto& entry : done.entries)
    {
        if (entry.handle == 3)
        {
            IC_CHECK(entry.threadPriority < THREAD_PRIORITY_NORMAL);
        }
        else
        {
            IC_CHECK(entry.threadPriority == THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }
}

IC_TEST(DecodeScheduler_EnqueueAfterShutdownIsRejected)
{
    const auto& files = Files();
    Completions done;
    DecodeScheduler scheduler;
    scheduler.SetThreadCount(2);
    scheduler.Shutdown();
    scheduler.Shutdown();  // idempotent

    IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, done.Callback(1))));
    IC_CHECK(!scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 2, done.Callback(2))));
    scheduler.Cancel(1);
    IC_CHECK(scheduler.LiveRequestCount() == 0);
    IC_CHECK(scheduler.WorkerCount() == 0);
    std::lock_guard<std::mutex> lock(done.mutex);
    IC_CHECK(done.entries.empty());
}

IC_TEST(DecodeScheduler_RestartClearsTuningAndPrefetchedBytes)
{
    // Restart (SetThreadCount on a running scheduler) drops queued work and must
    // not inherit volume tuning or prefetched bytes from the previous run.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;
    {
        Watchdog watchdog("restart test", std::chrono::seconds(60));
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
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 1, gatedCallback)));
        for (uint64_t handle = 2; handle <= 12; ++handle)
        {
            IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[handle], ImagePurpose::Thumbnail, handle,
                done.Callback(handle))));
        }
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }
        // Prefetch fills the cache while the only worker is held.
        IC_REQUIRE(WaitUntil([&]() { return cache.Count() >= 1; }));
        IC_CHECK(scheduler.VolumeProfileCount() == 1);
        IC_CHECK(scheduler.LiveRequestCount() >= 1);

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        scheduler.SetThreadCount(2);

        IC_CHECK(cache.Count() == 0);
        IC_CHECK(cache.MaxEntries() == 4);
        IC_CHECK(scheduler.VolumeProfileCount() == 0);
        IC_CHECK(scheduler.LiveRequestCount() == 0);

        // The restarted scheduler works.
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[20], ImagePurpose::Thumbnail, 100, done.Callback(100))));
        IC_REQUIRE(WaitUntil([&]() { return Delivered(done, 100); }));
        IC_CHECK(scheduler.VolumeProfileCount() == 1);
    }
}

IC_TEST(DecodeScheduler_TuningQueryRunsOutsideQueueMutex)
{
    // Enqueue resolves storage tuning (host path, volume query) before taking
    // the queue mutex. With that step blocked, Cancel and other queue users must
    // still make progress.
    const auto& files = Files();
    Completions done;
    {
        Watchdog watchdog("tuning outside mutex", std::chrono::seconds(60));
        GatedByteSource source;
        source.gatedResolve = files.thumbs[25];
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);

        std::thread enqueuer([&]()
        {
            scheduler.Enqueue(MakeTask(files.thumbs[25], ImagePurpose::Thumbnail, 1, done.Callback(1)));
        });
        const bool entered = source.WaitEntered();

        std::mutex m;
        std::condition_variable cv;
        bool progressed = false;
        std::thread other([&]()
        {
            scheduler.Cancel(999);
            (void)scheduler.LiveRequestCount();
            std::lock_guard<std::mutex> lock(m);
            progressed = true;
            cv.notify_all();
        });
        bool progressedWhileBlocked = false;
        {
            std::unique_lock<std::mutex> lock(m);
            progressedWhileBlocked = cv.wait_for(lock, std::chrono::seconds(2), [&]() { return progressed; });
        }
        source.Release();
        other.join();
        enqueuer.join();

        IC_REQUIRE(entered);
        IC_CHECK(progressedWhileBlocked);
        IC_REQUIRE(done.WaitFor(1));
    }
}

namespace
{
    // Archive-like byte source: paths under `<dir>\pack.zip\` do not exist on
    // disk and resolve to the host `<dir>\pack.zip` (which does not exist
    // either), so a thumbnail decode can only succeed with the bytes the
    // prefetch stage attached to its task. ReadAll maps `pack.zip\name` to the
    // real `<dir>\name` and records every read.
    class VirtualArchiveSource : public IPathByteSource
    {
    public:
        explicit VirtualArchiveSource(const std::filesystem::path& dir)
            : m_previous(GetPathByteSource())
            , m_dir(dir)
            , m_host((dir / L"pack.zip").wstring())
        {
            SetPathByteSource(this);
        }

        ~VirtualArchiveSource() override
        {
            SetPathByteSource(m_previous);
        }

        std::wstring Entry(const std::wstring& realPath) const
        {
            return m_host + L"\\" + std::filesystem::path(realPath).filename().wstring();
        }

        std::wstring ResolveHostPath(const std::wstring& path) const override
        {
            return IsVirtual(path) ? m_host : path;
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_reads.push_back(path);
            }
            const std::filesystem::path real = IsVirtual(path)
                ? m_dir / std::filesystem::path(path).filename()
                : std::filesystem::path(path);
            std::ifstream in(real, std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

        size_t ReadCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_reads.size();
        }

    private:
        bool IsVirtual(const std::wstring& path) const
        {
            return path.size() > m_host.size() && path.compare(0, m_host.size(), m_host) == 0;
        }

        IPathByteSource* m_previous;
        std::filesystem::path m_dir;
        std::wstring m_host;
        mutable std::mutex m_mutex;
        mutable std::vector<std::wstring> m_reads;
    };

    // Holds the only worker inside a full-resolution callback (no prefetch slot).
    struct WorkerGate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool released = false;

        std::function<void(PipelineResult&&)> Callback(Completions& done, uint64_t handle)
        {
            return [this, inner = done.Callback(handle)](PipelineResult&& result) mutable
            {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    entered = true;
                    cv.notify_all();
                    cv.wait(lock, [&]() { return released; });
                }
                inner(std::move(result));
            };
        }

        bool WaitEntered()
        {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, kWaitTimeout, [&]() { return entered; });
        }

        void Release()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                released = true;
            }
            cv.notify_all();
        }
    };

    HRESULT DeliveredHr(Completions& done, uint64_t handle)
    {
        std::lock_guard<std::mutex> lock(done.mutex);
        for (const auto& entry : done.entries)
        {
            if (entry.handle == handle)
            {
                return entry.hr;
            }
        }
        return E_PENDING;
    }
}

IC_TEST(DecodeScheduler_CanceledSamePathTaskKeepsNewerArchiveBytes)
{
    // Regression: two queued thumbnail tasks for the same archive entry shared
    // one path-keyed cache entry. Dropping the canceled older task erased it,
    // so the newer task's decode found no bytes and failed on the virtual path.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;
    WorkerGate gate;
    {
        Watchdog watchdog("same-path archive bytes", std::chrono::seconds(60));
        VirtualArchiveSource source(files.dir);
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        struct ReleaseOnExit { WorkerGate& g; ~ReleaseOnExit() { g.Release(); } } releaseOnExit { gate };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, gate.Callback(done, 1))));
        IC_REQUIRE(gate.WaitEntered());

        const std::wstring entry = source.Entry(files.thumbs[5]);
        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 3, done.Callback(3))));
        // Both tasks have been prefetched (the second reused the first one's bytes).
        IC_REQUIRE(WaitUntil([&]() { return scheduler.PrefetchedTaskCount() == 2; }));
        IC_CHECK(source.ReadCount() == 1);

        scheduler.Cancel(2);
        IC_CHECK(scheduler.PrefetchedTaskCount() == 1);
        gate.Release();

        IC_REQUIRE(WaitUntil([&]() { return DeliveredHr(done, 3) != E_PENDING; }));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 3)));
        IC_REQUIRE(WaitUntil([&]() { return scheduler.LiveRequestCount() == 0 && scheduler.PrefetchedTaskCount() == 0; }));
        IC_REQUIRE(WaitUntil([&]() { return cache.Count() == 0; }));
    }
    IC_CHECK(DeliveredHr(done, 2) == E_PENDING);
}

IC_TEST(DecodeScheduler_CancelReleasesPrefetchSlotsImmediately)
{
    // With the only worker busy, prefetch fills its slots and stops. Canceling
    // the prefetched tasks must free their slots at once, so prefetch continues
    // without waiting for a worker to reach and drop the canceled tasks.
    const auto& files = Files();
    Completions done;
    WorkerGate gate;
    size_t held = 0;
    {
        Watchdog watchdog("cancel releases slots", std::chrono::seconds(60));
        ScopedFileByteSource byteSource;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        struct ReleaseOnExit { WorkerGate& g; ~ReleaseOnExit() { g.Release(); } } releaseOnExit { gate };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, gate.Callback(done, 1))));
        IC_REQUIRE(gate.WaitEntered());

        constexpr uint64_t kFirst = 10;
        constexpr uint64_t kCount = 20;
        for (uint64_t i = 0; i < kCount; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[i], ImagePurpose::Thumbnail, kFirst + i,
                done.Callback(kFirst + i))));
        }

        // Wait until prefetch stops at its cap (no read for 300 ms).
        {
            size_t last = byteSource.ReadCount();
            auto lastChange = std::chrono::steady_clock::now();
            IC_REQUIRE(WaitUntil([&]()
            {
                const size_t count = byteSource.ReadCount();
                const auto now = std::chrono::steady_clock::now();
                if (count != last)
                {
                    last = count;
                    lastChange = now;
                }
                return last >= 1 && now - lastChange >= std::chrono::milliseconds(300);
            }));
            held = last;
        }
        IC_REQUIRE(held < kCount);
        IC_CHECK(scheduler.PrefetchedTaskCount() == held);

        // Cancel the prefetched tasks; the worker is still held.
        for (uint64_t i = 0; i < held; ++i)
        {
            scheduler.Cancel(kFirst + i);
        }
        // (No slot-count check here: the freed slots wake prefetch, which may
        // legitimately refill them before any count could be observed.)
        IC_CHECK(WaitUntil([&]() { return byteSource.ReadCount() >= (std::min)(2 * held, static_cast<size_t>(kCount)); }));

        gate.Release();
        IC_REQUIRE(done.WaitFor(1 + kCount - held));
        IC_REQUIRE(WaitUntil([&]() { return scheduler.LiveRequestCount() == 0 && scheduler.PrefetchedTaskCount() == 0; }));
    }

    std::lock_guard<std::mutex> lock(done.mutex);
    IC_CHECK(done.entries.size() == 1 + 20 - held);
    for (const auto& entry : done.entries)
    {
        IC_CHECK(entry.handle == 1 || entry.handle >= 10 + held);
        IC_CHECK(SUCCEEDED(entry.hr));
    }
}

IC_TEST(DecodeScheduler_SamePathCancelRequeueStressKeepsArchiveBytes)
{
    // Rapid cancel/re-request of a few archive entries on several workers, like
    // thumbnail tiles being reused while scrolling. Every delivered result must
    // succeed (a lost-bytes decode fails on the virtual path), and all request,
    // prefetch and cache state must drain.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;
    {
        Watchdog watchdog("same-path stress", std::chrono::seconds(90));
        VirtualArchiveSource source(files.dir);
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(3);

        constexpr uint64_t kRounds = 400;
        std::vector<uint64_t> lastFor(4, 0);
        for (uint64_t round = 0; round < kRounds; ++round)
        {
            const size_t slot = static_cast<size_t>(round % 4);
            const uint64_t handle = round + 1;
            IC_REQUIRE(scheduler.Enqueue(MakeTask(source.Entry(files.thumbs[slot]), ImagePurpose::Thumbnail, handle,
                done.Callback(handle))));
            // Tile reuse: usually cancel the previous request for this entry.
            if (lastFor[slot] != 0 && round % 3 != 0)
            {
                scheduler.Cancel(lastFor[slot]);
            }
            lastFor[slot] = handle;
            if (round % 8 == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // The last request per entry was never canceled and must arrive.
        for (const uint64_t handle : lastFor)
        {
            IC_REQUIRE(WaitUntil([&]() { return DeliveredHr(done, handle) != E_PENDING; }));
        }
        IC_REQUIRE(WaitUntil([&]() { return scheduler.LiveRequestCount() == 0 && scheduler.PrefetchedTaskCount() == 0; }));
        IC_REQUIRE(WaitUntil([&]() { return cache.Count() == 0; }));
    }

    std::lock_guard<std::mutex> lock(done.mutex);
    size_t failures = 0;
    for (const auto& entry : done.entries)
    {
        if (FAILED(entry.hr))
        {
            ++failures;
        }
    }
    std::printf("  same-path stress: %zu results delivered, %zu failed\n", done.entries.size(), failures);
    IC_CHECK(failures == 0);
}

IC_TEST(DecodeScheduler_BulkCancelOfLargeBacklogIsLinear)
{
    // A consumer that queues a request per folder entry (thousands) and cancels
    // them on navigation must not pay a queue scan per Cancel. With the only
    // worker held and prefetch stopped at its cap, the backlog stays queued while
    // it is canceled; 8x more requests must cost about 8x more time, far from the
    // 64x of a per-Cancel scan.
    const auto& files = Files();
    Completions done;
    WorkerGate gate;
    double smallMs = 0.0;
    double largeMs = 0.0;
    {
        Watchdog watchdog("bulk cancel", std::chrono::seconds(120));
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        struct ReleaseOnExit { WorkerGate& g; ~ReleaseOnExit() { g.Release(); } } releaseOnExit { gate };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, gate.Callback(done, 1))));
        IC_REQUIRE(gate.WaitEntered());

        auto cancelBacklog = [&](uint64_t first, uint64_t count) -> double
        {
            for (uint64_t i = 0; i < count; ++i)
            {
                scheduler.Enqueue(MakeTask(files.thumbs[i % files.thumbs.size()], ImagePurpose::Thumbnail,
                    first + i, done.Callback(first + i)));
            }
            const auto start = std::chrono::steady_clock::now();
            for (uint64_t i = 0; i < count; ++i)
            {
                scheduler.Cancel(first + i);
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        };

        smallMs = cancelBacklog(100, 4000);
        largeMs = cancelBacklog(100000, 32000);
        IC_CHECK(scheduler.LiveRequestCount() == 0);

        gate.Release();
        IC_REQUIRE(done.WaitFor(1));
        IC_REQUIRE(WaitUntil([&]() { return scheduler.PrefetchedTaskCount() == 0; }));
    }

    std::printf("  bulk cancel: 4000 queued in %.2f ms, 32000 queued in %.2f ms\n", smallMs, largeMs);
    // Generous linear bound with a floor for timer noise on tiny durations.
    IC_CHECK(largeMs < (std::max)(24.0 * smallMs, 50.0));
    std::lock_guard<std::mutex> lock(done.mutex);
    IC_CHECK(done.entries.size() == 1);
}

namespace
{
    std::unordered_set<DWORD> ProcessThreadIds();  // defined with the read-cancel tests

    // Lowest priority among the process threads not in `excluded`.
    int LowestThreadPriority(const std::unordered_set<DWORD>& excluded)
    {
        int lowest = THREAD_PRIORITY_TIME_CRITICAL;
        for (DWORD id : ProcessThreadIds())
        {
            if (excluded.count(id) != 0)
            {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
            if (!thread)
            {
                continue;  // already gone
            }
            const int priority = GetThreadPriority(thread);
            CloseHandle(thread);
            if (priority != THREAD_PRIORITY_ERROR_RETURN)
            {
                lowest = (std::min)(lowest, priority);
            }
        }
        return lowest;
    }
}

IC_TEST(DecodeScheduler_IdleThreadsWaitOutsideBackgroundMode)
{
    // A thread waiting in background mode would wake (for a stolen full-
    // resolution task, or for shutdown) at background priority, which normal-
    // priority load starves for seconds (floyd68/ImageCore#21). Background work
    // still runs in background mode; the idle workers and the idle prefetch
    // thread wait at normal priority or above.
    const auto& files = Files();
    Completions done;
    DecodeScheduler scheduler;
    const auto before = ProcessThreadIds();
    scheduler.SetThreadCount(3);  // high-priority worker, two background workers

    // Through the prefetch read and a background decode, then idle again.
    IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[2], ImagePurpose::Thumbnail, 1, done.Callback(1))));
    IC_REQUIRE(done.WaitFor(1));
    IC_CHECK(done.entries[0].threadPriority < THREAD_PRIORITY_NORMAL);

    // The decoding worker leaves background mode just after its callback.
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    int lowest = LowestThreadPriority(before);
    while (lowest < THREAD_PRIORITY_NORMAL && std::chrono::steady_clock::now() < deadline)
    {
        Sleep(1);
        lowest = LowestThreadPriority(before);
    }
    IC_CHECK(lowest >= THREAD_PRIORITY_NORMAL);
}

namespace
{
    // Fails the process with a message if Advance() is not called for `limit`.
    // It catches one stuck step of a long loop without bounding the sum of
    // many short steps, which a loaded machine stretches.
    class StallWatchdog
    {
    public:
        StallWatchdog(const char* what, std::chrono::seconds limit)
            : m_done(CreateEventW(nullptr, TRUE, FALSE, nullptr))
            , m_thread([this, what, limit]()
            {
                int seen = m_progress.load();
                auto lastChange = std::chrono::steady_clock::now();
                while (WaitForSingleObject(m_done, 250) != WAIT_OBJECT_0)
                {
                    const int progress = m_progress.load();
                    const auto now = std::chrono::steady_clock::now();
                    if (progress != seen)
                    {
                        seen = progress;
                        lastChange = now;
                    }
                    else if (now - lastChange > limit)
                    {
                        std::fprintf(stderr, "  watchdog: %s: step %d did not finish within %lld s; terminating\n",
                            what, progress, static_cast<long long>(limit.count()));
                        std::fflush(stderr);
                        TerminateProcess(GetCurrentProcess(), 3);
                    }
                }
            })
        {
        }

        ~StallWatchdog()
        {
            SetEvent(m_done);
            m_thread.join();
            CloseHandle(m_done);
        }

        void Advance()
        {
            ++m_progress;
        }

    private:
        HANDLE m_done;
        std::atomic<int> m_progress { 0 };
        std::thread m_thread;
    };
}

// Kept last: it creates and destroys many schedulers.
IC_TEST(DecodeScheduler_ShutdownCyclesNeverHang)
{
    // Shutdown with idle workers (the case that hung: a waiter that evaluated
    // its predicate just before the flag flipped) and with queued and in-flight
    // full-resolution work. Full-resolution tasks bypass the FileByteCache, so
    // dropped tasks leave no stale cache entries behind for later cycles.
    //
    // A hang is one cycle that never ends, so the watchdog bounds each cycle,
    // not the run: under CPU saturation every cycle waits for its share of the
    // processors (more busy threads than CPUs: 50-190 ms per cycle on average,
    // the longest measured cycle 150 ms in an instrumented Debug run), and
    // 1,000 such cycles took 51-190 s, past a 90 s budget for the whole run
    // (floyd68/ImageCore#21).
    const auto& files = Files();
    constexpr int kCycles = 1000;
    StallWatchdog watchdog("shutdown cycles", std::chrono::seconds(30));

    int completed = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle, watchdog.Advance())
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

// ---------------------------------------------------------------------------
// Request priority and promotion (both stages). The prefetch stage is held in
// a gated read or the only decode worker in a gated callback, so the queue
// contents are fixed before the order is observed.

namespace
{
    // Filesystem byte source whose read of one path blocks until Open().
    class GatedFileByteSource final : public ScopedFileByteSource
    {
    public:
        explicit GatedFileByteSource(std::wstring gatedPath)
            : m_gatedPath(std::move(gatedPath))
        {
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            if (path == m_gatedPath)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_entered = true;
                m_cv.notify_all();
                m_cv.wait(lock, [&] { return m_open; });
            }
            return ScopedFileByteSource::ReadAll(path);
        }

        bool WaitEntered()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, kWaitTimeout, [&] { return m_entered; });
        }

        void Open()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_open = true;
            }
            m_cv.notify_all();
        }

    private:
        std::wstring m_gatedPath;
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        mutable bool m_entered { false };
        bool m_open { false };
    };

    // Opens the gate on every exit path (declare after the scheduler).
    struct ByteGateRelease
    {
        GatedFileByteSource& source;
        ~ByteGateRelease() { source.Open(); }
    };

    DecodeTask MakeThumb(const std::wstring& path, uint64_t handle, RequestPriority priority,
        std::function<void(PipelineResult&&)> callback)
    {
        DecodeTask task = MakeTask(path, ImagePurpose::Thumbnail, handle, std::move(callback));
        task.priority = priority;
        return task;
    }

    size_t IndexOf(const std::vector<std::wstring>& items, const std::wstring& value)
    {
        return static_cast<size_t>(std::find(items.begin(), items.end(), value) - items.begin());
    }

    bool WaitUntil(const std::function<bool()>& condition)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
}

IC_TEST(DecodeScheduler_PriorityOrdersAndPromotesInTheIoStage)
{
    // The prefetch stage is held inside the read of thumbs[0]. Queued meanwhile:
    // three prefetch-priority thumbnails, then one visible; then the third
    // prefetch one is promoted. Reads after the gate must follow priority, then
    // age: the promoted one (older) and the visible one before the other two.
    const auto& files = Files();
    Completions done;
    std::vector<std::wstring> reads;
    {
        Watchdog watchdog("I/O priority test", std::chrono::seconds(60));
        GatedFileByteSource byteSource(files.thumbs[0]);
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(2);
        ByteGateRelease release { byteSource };

        scheduler.Enqueue(MakeThumb(files.thumbs[0], 1, RequestPriority::Prefetch, done.Callback(1)));
        IC_REQUIRE(byteSource.WaitEntered());
        scheduler.Enqueue(MakeThumb(files.thumbs[1], 2, RequestPriority::Prefetch, done.Callback(2)));
        scheduler.Enqueue(MakeThumb(files.thumbs[2], 3, RequestPriority::Prefetch, done.Callback(3)));
        scheduler.Enqueue(MakeThumb(files.thumbs[3], 4, RequestPriority::Prefetch, done.Callback(4)));
        scheduler.Enqueue(MakeThumb(files.thumbs[4], 5, RequestPriority::Visible, done.Callback(5)));
        scheduler.SetPriority(4, RequestPriority::Visible);   // promote thumbs[3]
        scheduler.SetPriority(99, RequestPriority::Visible);  // unknown: no-op

        byteSource.Open();
        IC_REQUIRE(done.WaitFor(5));
        reads = byteSource.Reads();
    }

    IC_REQUIRE(reads.size() == 5);
    IC_CHECK(reads[0] == files.thumbs[0]);
    IC_CHECK(reads[1] == files.thumbs[3]);   // promoted, older than thumbs[4]
    IC_CHECK(reads[2] == files.thumbs[4]);   // visible
    IC_CHECK(reads[3] == files.thumbs[1]);   // prefetch, by age
    IC_CHECK(reads[4] == files.thumbs[2]);
    for (const auto& entry : done.entries)
    {
        IC_CHECK(SUCCEEDED(entry.hr));
    }
}

IC_TEST(DecodeScheduler_HigherPriorityTakesTheNextPrefetchSlot)
{
    // The only decode worker is held, so prefetched bytes reach the cap and the
    // prefetch stage waits for a slot holding the next prefetch-priority task.
    // A visible request queued now must get that slot first.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;

    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;
    std::vector<std::wstring> reads;
    size_t readsAtCap = 0;
    const size_t backlog = 12;
    {
        Watchdog watchdog("prefetch slot test", std::chrono::seconds(60));
        ScopedFileByteSource byteSource;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        GateRelease releaseOnExit { gateMutex, gateCv, released };

        auto gated = [&, inner = done.Callback(1)](PipelineResult&& result) mutable
        {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                entered = true;
                gateCv.notify_all();
                gateCv.wait(lock, [&]() { return released; });
            }
            inner(std::move(result));
        };
        scheduler.Enqueue(MakeThumb(files.thumbs[0], 1, RequestPriority::Prefetch, gated));
        for (size_t i = 1; i < backlog; ++i)
        {
            const uint64_t handle = static_cast<uint64_t>(i) + 1;
            scheduler.Enqueue(MakeThumb(files.thumbs[i], handle, RequestPriority::Prefetch, done.Callback(handle)));
        }
        const size_t cap = cache.MaxEntries();
        IC_REQUIRE(cap + 2 < backlog);
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }
        IC_REQUIRE(WaitUntil([&] { return scheduler.PrefetchedTaskCount() == cap; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let the stage block on the slot
        readsAtCap = byteSource.ReadCount();

        const std::wstring& visible = files.thumbs[backlog];
        const uint64_t visibleHandle = backlog + 1;
        scheduler.Enqueue(MakeThumb(visible, visibleHandle, RequestPriority::Visible, done.Callback(visibleHandle)));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        IC_CHECK(byteSource.ReadCount() == readsAtCap);   // still at the cap

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        IC_REQUIRE(done.WaitFor(backlog + 1));
        reads = byteSource.Reads();
    }

    IC_REQUIRE(reads.size() == backlog + 1);
    IC_CHECK(readsAtCap >= 1);
    // The first read after the cap was the visible request, not the waiting one.
    IC_CHECK(reads[readsAtCap] == files.thumbs[backlog]);
}

IC_TEST(DecodeScheduler_PromotionReordersTheDecodeStage)
{
    // With the only decode worker held, several thumbnails are read and wait in
    // the decode stage. Promoting the oldest-but-lowest one to MainImage priority
    // makes it decode first once the worker is free.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;

    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool entered = false;
    bool released = false;
    bool skipped = false;
    {
        Watchdog watchdog("decode priority test", std::chrono::seconds(60));
        ScopedFileByteSource byteSource;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        GateRelease releaseOnExit { gateMutex, gateCv, released };

        auto gated = [&, inner = done.Callback(1)](PipelineResult&& result) mutable
        {
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                entered = true;
                gateCv.notify_all();
                gateCv.wait(lock, [&]() { return released; });
            }
            inner(std::move(result));
        };
        scheduler.Enqueue(MakeThumb(files.thumbs[0], 1, RequestPriority::Visible, gated));
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            IC_REQUIRE(gateCv.wait_for(lock, kWaitTimeout, [&]() { return entered; }));
        }
        scheduler.Enqueue(MakeThumb(files.thumbs[1], 2, RequestPriority::Prefetch, done.Callback(2)));
        scheduler.Enqueue(MakeThumb(files.thumbs[2], 3, RequestPriority::Prefetch, done.Callback(3)));
        scheduler.Enqueue(MakeThumb(files.thumbs[3], 4, RequestPriority::Visible, done.Callback(4)));

        // All three must wait in the decode stage (they hold prefetch slots next
        // to the held task); a seek-penalty volume caps prefetch below that.
        if (cache.MaxEntries() < 4)
        {
            skipped = true;
        }
        else
        {
            IC_REQUIRE(WaitUntil([&] { return scheduler.PrefetchedTaskCount() == 4; }));
            scheduler.SetPriority(2, RequestPriority::MainImage);
        }

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            released = true;
        }
        gateCv.notify_all();
        IC_REQUIRE(done.WaitFor(4));
    }

    if (skipped)
    {
        std::printf("  (prefetch cap below 4 on this volume: decode-stage ordering not checked)\n");
        return;
    }
    IC_REQUIRE(done.entries.size() == 4);
    IC_CHECK(done.entries[0].handle == 1);
    IC_CHECK(done.entries[1].handle == 2);   // promoted to MainImage
    IC_CHECK(done.entries[2].handle == 4);   // visible
    IC_CHECK(done.entries[3].handle == 3);   // prefetch
}

IC_TEST(DecodeScheduler_PromotionAndCancelStressUnderTileReuse)
{
    // Rapid tile reuse: each "tile" repeatedly cancels its request and issues a
    // new one for another path, while scrolling promotes and demotes recent
    // handles (including stale ones). Every request that was not canceled must
    // complete exactly once; nothing may remain live or hold prefetched bytes.
    const auto& files = Files();
    constexpr int kTiles = 16;
    constexpr int kRounds = 60;

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<uint64_t, int> deliveries;
    std::unordered_set<uint64_t> canceled;
    std::vector<uint64_t> issued;
    {
        Watchdog watchdog("priority stress", std::chrono::seconds(90));
        ScopedFileByteSource byteSource;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(3);

        std::vector<uint64_t> tileHandle(kTiles, 0);
        uint64_t nextHandle = 1;
        uint32_t rng = 12345;
        const auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        for (int round = 0; round < kRounds; ++round)
        {
            for (int tile = 0; tile < kTiles; ++tile)
            {
                if (tileHandle[tile] != 0)
                {
                    scheduler.Cancel(tileHandle[tile]);
                    std::lock_guard<std::mutex> lock(mutex);
                    canceled.insert(tileHandle[tile]);
                }
                const uint64_t handle = nextHandle++;
                tileHandle[tile] = handle;
                issued.push_back(handle);
                const auto priority = static_cast<RequestPriority>(next() % 3);
                const auto& path = files.thumbs[next() % files.thumbs.size()];
                scheduler.Enqueue(MakeThumb(path, handle, priority, [&, handle](PipelineResult&&)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++deliveries[handle];
                    cv.notify_all();
                }));
            }
            // Scrolling: re-prioritize random recent handles, some long finished.
            for (int k = 0; k < kTiles; ++k)
            {
                const uint64_t handle = 1 + next() % (nextHandle - 1);
                scheduler.SetPriority(handle, static_cast<RequestPriority>(next() % 3));
            }
        }

        // The last request of every tile stays live: all of those must complete.
        {
            std::unique_lock<std::mutex> lock(mutex);
            IC_REQUIRE(cv.wait_for(lock, std::chrono::seconds(30), [&]
            {
                for (uint64_t handle : tileHandle)
                {
                    if (deliveries.find(handle) == deliveries.end())
                    {
                        return false;
                    }
                }
                return true;
            }));
        }
        IC_REQUIRE(WaitUntil([&] { return scheduler.LiveRequestCount() == 0; }));
        IC_CHECK(WaitUntil([&] { return scheduler.PrefetchedTaskCount() == 0; }));
        // Promoting finished handles leaves no state behind.
        for (uint64_t handle : tileHandle)
        {
            scheduler.SetPriority(handle, RequestPriority::MainImage);
        }
        IC_CHECK(scheduler.LiveRequestCount() == 0);
    }

    std::lock_guard<std::mutex> lock(mutex);
    for (uint64_t handle : issued)
    {
        const auto it = deliveries.find(handle);
        const int count = (it == deliveries.end()) ? 0 : it->second;
        if (canceled.count(handle) != 0)
        {
            IC_CHECK(count <= 1);   // a late cancel may still deliver once
        }
        else
        {
            IC_CHECK(count == 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Read cancellation (request stop tokens, read leases, OS cancellation). The
// byte source below blocks chosen reads in a controlled way; the scheduler's
// test hook observes (and can hold) a request at its read and decode points.
// Every wait is bounded.

namespace
{
    // Archive-like byte source with per-path read modes. Paths under
    // `<dir>\pack.zip\` are virtual (as in VirtualArchiveSource): they resolve
    // to a host archive that does not exist, so a decode of one succeeds only
    // with the bytes the read returned. Other paths are read from disk.
    //
    // Modes (each applies to the next `times` stop-aware reads of a path):
    //   Cooperative - blocks until stop is requested (then returns a partial
    //                 prefix, or nothing) or Release();
    //   IgnoreToken - blocks on an event until Release(), ignoring the token and
    //                 OS cancellation (WaitForSingleObject is not I/O);
    //   Pipe        - blocks in ReadFile on an anonymous pipe until WritePipe()
    //                 (or CancelSynchronousIo), ignoring the token;
    //   OtherThread - hands a pipe ReadFile to a helper thread and waits for it
    //                 on the token (like a host waiting for a shared open).
    class ControlledSource final : public IPathByteSource
    {
    public:
        enum class Mode { Normal, Cooperative, IgnoreToken, Pipe, OtherThread };

        explicit ControlledSource(const std::filesystem::path& dir)
            : m_previous(GetPathByteSource())
            , m_dir(dir)
            , m_host((dir / L"pack.zip").wstring())
            , m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
        {
            (void)CreatePipe(&m_pipeRead, &m_pipeWrite, nullptr, 0);
            SetPathByteSource(this);
        }

        ~ControlledSource() override
        {
            SetPathByteSource(m_previous);
            Release();
            // A read still blocked on the pipe fails with ERROR_BROKEN_PIPE.
            CloseHandle(m_pipeWrite);
            for (auto& helper : m_helpers)
            {
                helper.join();
            }
            CloseHandle(m_pipeRead);
            CloseHandle(m_event);
        }

        std::wstring Entry(const std::wstring& realPath) const
        {
            return m_host + L"\\" + std::filesystem::path(realPath).filename().wstring();
        }

        void SetMode(const std::wstring& path, Mode mode, int times = 1, bool partialOnStop = true)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_modes[path] = { mode, times, partialOnStop };
        }

        // Runs on the thread that requests stop, inside the stop callback.
        void SetOnStop(std::function<void()> onStop)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_onStop = std::move(onStop);
        }

        std::wstring ResolveHostPath(const std::wstring& path) const override
        {
            return IsVirtual(path) ? m_host : path;
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            return Load(path);
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override
        {
            ModeEntry mode {};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                PathStats& stats = m_stats[path];
                ++stats.reads;
                stats.lastThread = GetCurrentThreadId();
                auto it = m_modes.find(path);
                if (it != m_modes.end() && it->second.times > 0)
                {
                    mode = it->second;
                    --it->second.times;
                }
            }

            switch (mode.mode)
            {
            case Mode::Normal:
                return ReadNormal(path, stop);
            case Mode::Cooperative:
                return ReadCooperative(path, stop, mode.partialOnStop);
            case Mode::IgnoreToken:
                Enter(path);
                (void)WaitForSingleObject(m_event, static_cast<DWORD>(kWaitTimeout.count() * 1000));
                return Load(path);
            case Mode::Pipe:
            {
                Enter(path);
                const bool ok = ReadPipeByte();
                const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
                std::lock_guard<std::mutex> lock(m_mutex);
                PathStats& stats = m_stats[path];
                stats.aborted += (error == ERROR_OPERATION_ABORTED) ? 1 : 0;
                stats.returned += 1;
                m_cv.notify_all();
                return ok ? LoadUnlocked(path) : std::vector<uint8_t> {};
            }
            case Mode::OtherThread:
                return ReadOnOtherThread(path, stop);
            }
            return {};
        }

        // Normal reads take a random 0..`maxMicroseconds` and return a partial
        // prefix when stop is requested meanwhile.
        void SetJitter(int maxMicroseconds)
        {
            m_jitterUs = maxMicroseconds;
        }

        // Unblocks Cooperative (with full bytes) and IgnoreToken reads.
        void Release()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_released = true;
            }
            m_cv.notify_all();
            SetEvent(m_event);
        }

        // Lets one pending pipe read complete.
        void WritePipe()
        {
            const uint8_t byte = 1;
            DWORD written = 0;
            (void)WriteFile(m_pipeWrite, &byte, 1, &written, nullptr);
        }

        bool WaitEntered(const std::wstring& path, size_t count = 1) const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, kWaitTimeout, [&] { return StatsOf(path).entered >= count; });
        }

        // Waits until `count` blocked reads of `path` have returned.
        bool WaitReturned(const std::wstring& path, size_t count = 1,
            std::chrono::milliseconds timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kWaitTimeout)) const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [&] { return StatsOf(path).returned >= count; });
        }

        size_t Reads(const std::wstring& path) const { std::lock_guard<std::mutex> l(m_mutex); return StatsOf(path).reads; }
        size_t Aborted(const std::wstring& path) const { std::lock_guard<std::mutex> l(m_mutex); return StatsOf(path).aborted; }
        size_t StoppedReturns(const std::wstring& path) const { std::lock_guard<std::mutex> l(m_mutex); return StatsOf(path).stoppedReturns; }
        DWORD LastReadThread(const std::wstring& path) const { std::lock_guard<std::mutex> l(m_mutex); return StatsOf(path).lastThread; }
        // Thread on which the last stop callback ran (0 if none ran).
        DWORD StopCallbackThread() const { std::lock_guard<std::mutex> l(m_mutex); return m_stopThread; }

        // OtherThread mode: helper reads that completed with data / were aborted.
        size_t HelperCompleted() const { std::lock_guard<std::mutex> l(m_mutex); return m_helperCompleted; }
        size_t HelperAborted() const { std::lock_guard<std::mutex> l(m_mutex); return m_helperAborted; }
        bool WaitHelperCompleted(size_t count) const
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, kWaitTimeout, [&] { return m_helperCompleted + m_helperAborted >= count; });
        }

    private:
        struct ModeEntry
        {
            Mode mode { Mode::Normal };
            int times { 0 };
            bool partialOnStop { true };
        };

        struct PathStats
        {
            size_t reads { 0 };
            size_t entered { 0 };
            size_t returned { 0 };
            size_t aborted { 0 };
            size_t stoppedReturns { 0 };
            DWORD lastThread { 0 };
        };

        bool IsVirtual(const std::wstring& path) const
        {
            return path.size() > m_host.size() && path.compare(0, m_host.size(), m_host) == 0;
        }

        std::vector<uint8_t> Load(const std::wstring& path) const
        {
            return LoadUnlocked(path);
        }

        std::vector<uint8_t> LoadUnlocked(const std::wstring& path) const
        {
            const std::filesystem::path real = IsVirtual(path)
                ? m_dir / std::filesystem::path(path).filename()
                : std::filesystem::path(path);
            std::ifstream in(real, std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

        const PathStats& StatsOf(const std::wstring& path) const
        {
            static const PathStats kEmpty {};
            auto it = m_stats.find(path);
            return it == m_stats.end() ? kEmpty : it->second;
        }

        void Enter(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats[path].entered;
            m_cv.notify_all();
        }

        bool ReadPipeByte() const
        {
            uint8_t byte = 0;
            DWORD read = 0;
            return ReadFile(m_pipeRead, &byte, 1, &read, nullptr) && read == 1;
        }

        std::stop_callback<std::function<void()>> WakeOnStop(std::stop_token stop) const
        {
            // Registered before m_mutex is taken: it runs at once (on this
            // thread) when stop was already requested.
            return std::stop_callback<std::function<void()>>(stop, std::function<void()>([this]
            {
                std::function<void()> onStop;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    onStop = m_onStop;
                    m_stopThread = GetCurrentThreadId();
                }
                if (onStop)
                {
                    onStop();
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                }
                m_cv.notify_all();
            }));
        }

        std::vector<uint8_t> ReadNormal(const std::wstring& path, std::stop_token stop) const
        {
            const int jitter = m_jitterUs.load();
            if (jitter > 0)
            {
                thread_local uint32_t rng = GetCurrentThreadId();
                rng = rng * 1664525u + 1013904223u;
                const auto until = std::chrono::steady_clock::now() +
                    std::chrono::microseconds((rng >> 8) % static_cast<uint32_t>(jitter));
                while (std::chrono::steady_clock::now() < until && !stop.stop_requested())
                {
                    std::this_thread::yield();
                }
                if (stop.stop_requested())
                {
                    std::vector<uint8_t> bytes = Load(path);
                    bytes.resize((std::min)(bytes.size(), static_cast<size_t>(40)));
                    return bytes;
                }
            }
            return Load(path);
        }

        std::vector<uint8_t> ReadCooperative(const std::wstring& path, std::stop_token stop, bool partialOnStop) const
        {
            // Entered is signaled only once the stop callback is registered, so
            // a Cancel or Shutdown issued after WaitEntered always runs it.
            const auto wake = WakeOnStop(stop);
            Enter(path);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, kWaitTimeout, [&] { return stop.stop_requested() || m_released; });
            PathStats& stats = m_stats[path];
            stats.returned += 1;
            m_cv.notify_all();
            if (!stop.stop_requested())
            {
                return LoadUnlocked(path);
            }
            stats.stoppedReturns += 1;
            if (!partialOnStop)
            {
                return {};
            }
            std::vector<uint8_t> bytes = LoadUnlocked(path);
            bytes.resize((std::min)(bytes.size(), static_cast<size_t>(40)));
            return bytes;
        }

        std::vector<uint8_t> ReadOnOtherThread(const std::wstring& path, std::stop_token stop) const
        {
            auto done = std::make_shared<bool>(false);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_helpers.emplace_back([this, done]
                {
                    const bool ok = ReadPipeByte();
                    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
                    std::lock_guard<std::mutex> lock(m_mutex);
                    *done = true;
                    m_helperCompleted += ok ? 1 : 0;
                    m_helperAborted += (error == ERROR_OPERATION_ABORTED) ? 1 : 0;
                    m_cv.notify_all();
                });
            }
            const auto wake = WakeOnStop(stop);  // registered before Enter (see ReadCooperative)
            Enter(path);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, kWaitTimeout, [&] { return stop.stop_requested() || *done; });
            PathStats& stats = m_stats[path];
            stats.returned += 1;
            m_cv.notify_all();
            if (stop.stop_requested())
            {
                stats.stoppedReturns += 1;
                return {};
            }
            return LoadUnlocked(path);
        }

        IPathByteSource* m_previous;
        std::filesystem::path m_dir;
        std::wstring m_host;
        HANDLE m_event { nullptr };
        HANDLE m_pipeRead { nullptr };
        HANDLE m_pipeWrite { nullptr };

        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        mutable std::unordered_map<std::wstring, ModeEntry> m_modes;
        mutable std::unordered_map<std::wstring, PathStats> m_stats;
        mutable std::vector<std::thread> m_helpers;
        mutable size_t m_helperCompleted { 0 };
        mutable size_t m_helperAborted { 0 };
        std::function<void()> m_onStop;
        mutable DWORD m_stopThread { 0 };
        bool m_released { false };
        std::atomic<int> m_jitterUs { 0 };
    };

    // Records the scheduler's test points and can hold a thread at one.
    class HookProbe
    {
    public:
        using Point = DecodeScheduler::TestPoint;

        DecodeScheduler::TestHook Hook()
        {
            return [this](Point point, uint64_t handle) { OnPoint(point, handle); };
        }

        // The next arrival at (point, handle) blocks until Release(point, handle).
        void HoldAt(Point point, uint64_t handle)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_holds[Key(point, handle)] = Hold {};
        }

        bool WaitHeld(Point point, uint64_t handle)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, kWaitTimeout, [&]
            {
                auto it = m_holds.find(Key(point, handle));
                return it != m_holds.end() && it->second.held;
            });
        }

        void Release(Point point, uint64_t handle)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_holds[Key(point, handle)].released = true;
            }
            m_cv.notify_all();
        }

        void ReleaseAll()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& [key, hold] : m_holds)
                {
                    hold.released = true;
                }
            }
            m_cv.notify_all();
        }

        size_t Count(Point point, uint64_t handle) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_counts.find(Key(point, handle));
            return it == m_counts.end() ? 0 : it->second;
        }

    private:
        struct Hold
        {
            bool held { false };
            bool released { false };
        };

        static std::pair<int, uint64_t> Key(Point point, uint64_t handle)
        {
            return { static_cast<int>(point), handle };
        }

        void OnPoint(Point point, uint64_t handle)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_counts[Key(point, handle)];
            auto it = m_holds.find(Key(point, handle));
            if (it != m_holds.end() && !it->second.held && !it->second.released)
            {
                it->second.held = true;
                m_cv.notify_all();
                m_cv.wait_for(lock, kWaitTimeout, [&] { return it->second.released; });
            }
        }

        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::map<std::pair<int, uint64_t>, Hold> m_holds;
        std::map<std::pair<int, uint64_t>, size_t> m_counts;
    };

    // Runs `release` on scope exit; declare it after the scheduler so blocked
    // reads and held hooks are released before the scheduler joins.
    struct OnExit
    {
        std::function<void()> release;
        ~OnExit() { release(); }
    };

    using Mode = ControlledSource::Mode;
    using Point = DecodeScheduler::TestPoint;

    bool Idle(DecodeScheduler& scheduler)
    {
        return WaitUntil([&] { return scheduler.LiveRequestCount() == 0 && scheduler.PrefetchedTaskCount() == 0; });
    }

    size_t DeliveryCount(Completions& done)
    {
        std::lock_guard<std::mutex> lock(done.mutex);
        return done.entries.size();
    }
}

IC_TEST(ReadCancel_CancelReleasesBlockedThumbnailRead)
{
    // A thumbnail read blocked on its token (on the serialized prefetch thread)
    // is released by Cancel, and the next queued request is read and delivered
    // without the blocked read ever completing normally (it is never released).
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("thumbnail read cancel", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring a = source.Entry(files.thumbs[0]);
    const std::wstring b = source.Entry(files.thumbs[1]);
    source.SetMode(a, Mode::Cooperative);
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(2);
        OnExit release { [&] { source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(a, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(b, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(a));
        IC_CHECK(source.Reads(b) == 0);  // serialized behind the blocked read

        const auto canceledAt = std::chrono::steady_clock::now();
        scheduler.Cancel(1);
        IC_REQUIRE(WaitUntil([&] { return Delivered(done, 2); }));
        const auto latency = std::chrono::steady_clock::now() - canceledAt;
        IC_CHECK(latency < std::chrono::seconds(5));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 2)));
        IC_REQUIRE(Idle(scheduler));
        IC_REQUIRE(WaitUntil([&] { return cache.Count() == 0; }));
        std::printf("  thumbnail: next request delivered %lld ms after Cancel\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(latency).count()));
    }
    IC_CHECK(source.StoppedReturns(a) == 1);
    IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
    IC_CHECK(DeliveredHr(done, 1) == E_PENDING);
}

IC_TEST(ReadCancel_CancelReleasesBlockedFullResolutionArchiveRead)
{
    // The same on a full-resolution worker: the archive read is released by
    // Cancel and the worker takes the next full-resolution request.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("full-resolution read cancel", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring a = source.Entry(files.thumbs[2]);
    const std::wstring b = source.Entry(files.thumbs[3]);
    source.SetMode(a, Mode::Cooperative);
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        OnExit release { [&] { source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(a, ImagePurpose::FullResolution, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(b, ImagePurpose::FullResolution, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(a));

        const auto canceledAt = std::chrono::steady_clock::now();
        scheduler.Cancel(1);
        IC_REQUIRE(WaitUntil([&] { return Delivered(done, 2); }));
        IC_CHECK(std::chrono::steady_clock::now() - canceledAt < std::chrono::seconds(5));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 2)));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.StoppedReturns(a) == 1);
    IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
    IC_CHECK(probe.Count(Point::DecodeStart, 2) == 1);
    IC_CHECK(DeliveredHr(done, 1) == E_PENDING);
}

IC_TEST(ReadCancel_PreCanceledRequestsNeverReadOrDecode)
{
    // Requests canceled while still queued are never read, never decoded and
    // never delivered: thumbnails (plain file and archive entry) behind a blocked
    // prefetch read, and full-resolution requests (archive and plain file)
    // behind a held worker.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    WorkerGate gate;
    Watchdog watchdog("pre-canceled requests", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring blocker = source.Entry(files.thumbs[4]);
    const std::wstring plainThumb = files.thumbs[5];
    const std::wstring archiveThumb = source.Entry(files.thumbs[6]);
    const std::wstring archiveFull = source.Entry(files.thumbs[7]);
    source.SetMode(blocker, Mode::Cooperative);
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        OnExit release { [&] { gate.Release(); source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, gate.Callback(done, 1))));
        IC_REQUIRE(gate.WaitEntered());
        IC_REQUIRE(scheduler.Enqueue(MakeTask(blocker, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(blocker));

        IC_REQUIRE(scheduler.Enqueue(MakeTask(plainThumb, ImagePurpose::Thumbnail, 10, done.Callback(10))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(archiveThumb, ImagePurpose::Thumbnail, 11, done.Callback(11))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(archiveFull, ImagePurpose::FullResolution, 12, done.Callback(12))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 13, done.Callback(13))));
        for (uint64_t handle = 10; handle <= 13; ++handle)
        {
            scheduler.Cancel(handle);
        }
        IC_CHECK(scheduler.LiveRequestCount() == 1);  // the blocked read

        scheduler.Cancel(2);
        gate.Release();
        IC_REQUIRE(done.WaitFor(1));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.Reads(plainThumb) == 0);
    IC_CHECK(source.Reads(archiveThumb) == 0);
    IC_CHECK(source.Reads(archiveFull) == 0);
    for (uint64_t handle : { 2u, 10u, 11u, 12u, 13u })
    {
        IC_CHECK(probe.Count(Point::DecodeStart, handle) == 0);
        IC_CHECK(DeliveredHr(done, handle) == E_PENDING);
    }
    IC_CHECK(DeliveryCount(done) == 1);
}

IC_TEST(ReadCancel_CanceledRequestsNeverReachDecode)
{
    // The decode stage never runs for a request canceled before decode
    // eligibility, so the decoders' path fallback (which would succeed here for
    // the real on-disk image) never re-reads the file:
    //   (a) thumbnail of a plain file whose read is canceled (it returns nothing);
    //   (b) full-resolution archive entry whose read is canceled;
    //   (c) full-resolution plain file canceled after a worker took it, before
    //       its decode started.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("canceled never decodes", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring plainThumb = files.thumbs[8];
    const std::wstring archiveFull = source.Entry(files.thumbs[9]);
    source.SetMode(plainThumb, Mode::Cooperative, 1, false);
    source.SetMode(archiveFull, Mode::Cooperative);
    probe.HoldAt(Point::TaskTaken, 3);
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(2);
        OnExit release { [&] { probe.ReleaseAll(); source.Release(); } };

        // (a)
        IC_REQUIRE(scheduler.Enqueue(MakeTask(plainThumb, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(source.WaitEntered(plainThumb));
        scheduler.Cancel(1);
        // (b)
        IC_REQUIRE(scheduler.Enqueue(MakeTask(archiveFull, ImagePurpose::FullResolution, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(archiveFull));
        scheduler.Cancel(2);
        // (c)
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 3, done.Callback(3))));
        IC_REQUIRE(probe.WaitHeld(Point::TaskTaken, 3));
        scheduler.Cancel(3);
        probe.Release(Point::TaskTaken, 3);

        // An uncanceled request of each kind still decodes.
        IC_REQUIRE(scheduler.Enqueue(MakeTask(plainThumb, ImagePurpose::Thumbnail, 4, done.Callback(4))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 5, done.Callback(5))));
        IC_REQUIRE(done.WaitFor(2));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.StoppedReturns(plainThumb) == 1);
    IC_CHECK(source.StoppedReturns(archiveFull) == 1);
    for (uint64_t handle : { 1u, 2u, 3u })
    {
        IC_CHECK(probe.Count(Point::DecodeStart, handle) == 0);
        IC_CHECK(DeliveredHr(done, handle) == E_PENDING);
    }
    IC_CHECK(probe.Count(Point::DecodeStart, 4) == 1);
    IC_CHECK(SUCCEEDED(DeliveredHr(done, 4)));
    IC_CHECK(SUCCEEDED(DeliveredHr(done, 5)));
}

IC_TEST(ReadCancel_CanceledReadNeverPopulatesByteCache)
{
    // A canceled read returns a partial prefix. It must not be cached: the
    // same-path request queued behind it performs its own read and decodes the
    // full bytes (a virtual archive entry decodes only from the bytes read).
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("canceled read not cached", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring entry = source.Entry(files.thumbs[10]);
    source.SetMode(entry, Mode::Cooperative);  // first read only
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(2);
        OnExit release { [&] { source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(entry));
        scheduler.Cancel(1);

        IC_REQUIRE(WaitUntil([&] { return Delivered(done, 2); }));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 2)));
        IC_REQUIRE(Idle(scheduler));
        IC_REQUIRE(WaitUntil([&] { return cache.Count() == 0; }));
    }
    IC_CHECK(source.StoppedReturns(entry) == 1);
    IC_CHECK(source.Reads(entry) == 2);  // the uncanceled request read for itself
    IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
    IC_CHECK(DeliveredHr(done, 1) == E_PENDING);
}

IC_TEST(ReadCancel_CancelRacingReadCompletionStress)
{
    // Cancels land at random points around read completion (before, during and
    // after short reads that honour the token) on thumbnail and full-resolution archive
    // requests. Nothing may deliver a partial-read failure, every uncanceled
    // request completes once, and request, slot and cache state drains to zero.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    constexpr uint64_t kRequests = 400;
    std::mutex mutex;
    std::unordered_map<uint64_t, int> deliveries;
    std::unordered_map<uint64_t, HRESULT> results;
    std::unordered_set<uint64_t> canceled;
    {
        Watchdog watchdog("cancel race stress", std::chrono::seconds(90));
        ControlledSource source(files.dir);
        source.SetJitter(2000);
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(3);
        OnExit release { [&] { source.Release(); } };

        uint32_t rng = 777;
        const auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        std::vector<uint64_t> pending;
        for (uint64_t handle = 1; handle <= kRequests; ++handle)
        {
            const std::wstring entry = source.Entry(files.thumbs[handle % files.thumbs.size()]);
            const bool full = (next() % 4) == 0;
            IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, full ? ImagePurpose::FullResolution : ImagePurpose::Thumbnail,
                handle, [&, handle](PipelineResult&& result)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++deliveries[handle];
                    results[handle] = result.hr;
                })));
            pending.push_back(handle);
            if (next() % 3 == 0 && !pending.empty())
            {
                const size_t index = next() % pending.size();
                const uint64_t victim = pending[index];
                if (next() % 2 == 0)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(next() % 500));
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    canceled.insert(victim);
                }
                scheduler.Cancel(victim);
            }
        }

        IC_REQUIRE(WaitUntil([&]
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (uint64_t handle = 1; handle <= kRequests; ++handle)
            {
                if (canceled.count(handle) == 0 && deliveries.count(handle) == 0)
                {
                    return false;
                }
            }
            return true;
        }));
        IC_REQUIRE(Idle(scheduler));
        IC_REQUIRE(WaitUntil([&] { return cache.Count() == 0; }));
    }

    std::lock_guard<std::mutex> lock(mutex);
    for (uint64_t handle = 1; handle <= kRequests; ++handle)
    {
        const auto it = deliveries.find(handle);
        const int count = (it == deliveries.end()) ? 0 : it->second;
        IC_CHECK(count <= 1);
        if (canceled.count(handle) == 0)
        {
            IC_CHECK(count == 1);
        }
        if (count == 1)
        {
            IC_CHECK(SUCCEEDED(results[handle]));
        }
    }
    std::printf("  cancel race: %zu delivered, %zu canceled of %llu\n",
        deliveries.size(), canceled.size(), static_cast<unsigned long long>(kRequests));
}

IC_TEST(ReadCancel_CancelInterruptsBlockedSynchronousRead)
{
    // A source that ignores its token and blocks in a synchronous ReadFile (a
    // pipe here; a network path or online-only file in practice) is released by
    // the CancelSynchronousIo attempt that Cancel makes on the reading thread:
    // on the prefetch thread (thumbnail) and on a decode worker (full resolution).
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("OS cancel of a blocked read", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring thumb = source.Entry(files.thumbs[11]);
    const std::wstring full = source.Entry(files.thumbs[12]);
    source.SetMode(thumb, Mode::Pipe);
    source.SetMode(full, Mode::Pipe);
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        OnExit release { [&] { for (int i = 0; i < 4; ++i) { source.WritePipe(); } } };

        for (const auto& [path, purpose, handle] : {
            std::tuple { thumb, ImagePurpose::Thumbnail, uint64_t { 1 } },
            std::tuple { full, ImagePurpose::FullResolution, uint64_t { 2 } } })
        {
            IC_REQUIRE(scheduler.Enqueue(MakeTask(path, purpose, handle, done.Callback(handle))));
            IC_REQUIRE(source.WaitEntered(path));
            // Let the thread reach ReadFile (CancelSynchronousIo affects only an
            // operation already in progress).
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            scheduler.Cancel(handle);
            IC_CHECK(source.WaitReturned(path, 1, std::chrono::milliseconds(5000)));
            IC_CHECK(source.Aborted(path) == 1);
            IC_CHECK(probe.Count(Point::CancelDispatch, handle) == 1);
        }

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 3, done.Callback(3))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[13], ImagePurpose::Thumbnail, 4, done.Callback(4))));
        IC_REQUIRE(done.WaitFor(2));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
    IC_CHECK(probe.Count(Point::DecodeStart, 2) == 0);
    IC_CHECK(DeliveryCount(done) == 2);
}

IC_TEST(ReadCancel_StopCallbackMayReenterScheduler)
{
    // The stop callback runs on the scheduler's stop-dispatch thread. It
    // queries the scheduler, cancels and re-prioritizes other requests and
    // enqueues a new one; a probe thread meanwhile needs the queue lock. Neither
    // may block: the callback runs with no scheduler lock held.
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("re-entrant stop callback", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring entry = source.Entry(files.thumbs[14]);
    source.SetMode(entry, Mode::Cooperative);
    // Written by the stop callback on the scheduler's stop-dispatch thread.
    std::atomic<bool> reentered { false };
    std::atomic<bool> probeFinished { false };
    std::atomic<bool> enqueued { false };
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(2);
        OnExit release { [&] { source.Release(); } };

        source.SetOnStop([&]
        {
            (void)scheduler.LiveRequestCount();
            (void)scheduler.PrefetchedTaskCount();
            scheduler.SetPriority(2, RequestPriority::MainImage);
            scheduler.Cancel(2);
            enqueued = scheduler.Enqueue(MakeTask(files.thumbs[15], ImagePurpose::Thumbnail, 3, done.Callback(3)));

            std::mutex m;
            std::condition_variable cv;
            bool finished = false;
            std::thread probeThread([&]
            {
                (void)scheduler.LiveRequestCount();
                std::lock_guard<std::mutex> lock(m);
                finished = true;
                cv.notify_all();
            });
            {
                std::unique_lock<std::mutex> lock(m);
                probeFinished = cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished; });
            }
            probeThread.join();
            reentered = true;
        });

        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[16], ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(entry));
        scheduler.Cancel(1);
        IC_REQUIRE(WaitUntil([&] { return reentered.load(); }));
        IC_CHECK(probeFinished.load());
        IC_CHECK(enqueued.load());
        IC_CHECK(source.StopCallbackThread() != 0);
        IC_CHECK(source.StopCallbackThread() != GetCurrentThreadId());
        IC_CHECK(source.StopCallbackThread() != source.LastReadThread(entry));
        IC_REQUIRE(WaitUntil([&] { return Delivered(done, 3); }));
        IC_REQUIRE(Idle(scheduler));
        source.SetOnStop({});
    }
    IC_CHECK(DeliveredHr(done, 1) == E_PENDING);
    IC_CHECK(DeliveredHr(done, 2) == E_PENDING);
    IC_CHECK(SUCCEEDED(DeliveredHr(done, 3)));
}

IC_TEST(ReadCancel_LegacyReadAllSourceStillWorks)
{
    // A source that implements only ReadAll(path) keeps working: the stop-aware
    // overload's default delegates to it. Its reads are not interruptible, but a
    // read canceled meanwhile is still discarded once it returns.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    {
        Watchdog watchdog("legacy byte source", std::chrono::seconds(60));
        VirtualArchiveSource source(files.dir);
        const std::wstring entry = source.Entry(files.thumbs[17]);
        const IPathByteSource& base = source;
        IC_CHECK(base.ReadAll(entry, std::stop_token {}) == source.ReadAll(entry));

        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(2);
        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(source.Entry(files.thumbs[18]), ImagePurpose::FullResolution, 2,
            done.Callback(2))));
        IC_REQUIRE(done.WaitFor(2));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 1)));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 2)));
        IC_REQUIRE(Idle(scheduler));
    }
    {
        // Canceled during a blocked legacy read: discarded after it returns.
        Watchdog watchdog("legacy blocked read", std::chrono::seconds(60));
        GatedByteSource source;
        source.archivePath = files.thumbs[19];
        source.gatedRead = files.thumbs[19];
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[19], ImagePurpose::FullResolution, 3, done.Callback(3))));
        IC_REQUIRE(source.WaitEntered());
        scheduler.Cancel(3);
        source.Release();
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 4, done.Callback(4))));
        IC_REQUIRE(WaitUntil([&] { return Delivered(done, 4); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(probe.Count(Point::DecodeStart, 3) == 0);
    IC_CHECK(DeliveredHr(done, 3) == E_PENDING);
}

namespace
{
    // Barrier test for the read lease. Request A's read on thread T finishes;
    // A's cancellation is delivered late, while T moves on to request B, whose
    // read blocks in a real (cancellable) pipe ReadFile. B's read must not be
    // aborted and B must complete. `cancelAt`:
    //   BeforeReadRegister - Cancel while T is about to register A's read: A is
    //                        then never read and no OS cancel is dispatched;
    //   BeforeReadRetire   - Cancel after A's read returned, before T retires it:
    //                        the OS cancel hits T while it is outside any I/O;
    //   CancelDispatch     - the canceler is held between finding A's read on
    //                        the lease and dispatching CancelSynchronousIo, while
    //                        T tries to retire A and start B: T must not start B
    //                        until the dispatch is done.
    void RunLateCancelBarrier(ImagePurpose purpose, Point cancelAt)
    {
        const auto& files = Files();
        Completions done;
        HookProbe probe;
        Watchdog watchdog("late cancel barrier", std::chrono::seconds(60));
        ControlledSource source(files.dir);
        const std::wstring a = source.Entry(files.thumbs[20]);
        const std::wstring b = source.Entry(files.thumbs[21]);
        source.SetMode(b, Mode::Pipe);
        const Point holdAt = (cancelAt == Point::CancelDispatch) ? Point::BeforeReadRetire : cancelAt;
        probe.HoldAt(holdAt, 1);
        if (cancelAt == Point::CancelDispatch)
        {
            probe.HoldAt(Point::CancelDispatch, 1);
        }
        bool bStartedEarly = false;
        {
            DecodeScheduler scheduler;
            scheduler.SetTestHook(probe.Hook());
            scheduler.SetThreadCount(1);
            OnExit release { [&] { probe.ReleaseAll(); for (int i = 0; i < 4; ++i) { source.WritePipe(); } } };

            IC_REQUIRE(scheduler.Enqueue(MakeTask(a, purpose, 1, done.Callback(1))));
            IC_REQUIRE(scheduler.Enqueue(MakeTask(b, purpose, 2, done.Callback(2))));
            IC_REQUIRE(probe.WaitHeld(holdAt, 1));

            std::thread canceler;
            if (cancelAt == Point::CancelDispatch)
            {
                // Cancel returns at once; the dispatch thread is held at the
                // dispatch point under the lease mutex.
                canceler = std::thread([&] { scheduler.Cancel(1); });
                IC_REQUIRE(probe.WaitHeld(Point::CancelDispatch, 1));
                probe.Release(holdAt, 1);
                // T now waits to retire A; it must not reach B's read.
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                bStartedEarly = source.Reads(b) != 0;
                probe.Release(Point::CancelDispatch, 1);
                canceler.join();
            }
            else
            {
                // The stop (and any OS cancel) is dispatched asynchronously;
                // release T only once it is done, so it lands at `cancelAt`.
                scheduler.Cancel(1);
                IC_REQUIRE(WaitUntil([&] { return probe.Count(Point::StopDispatched, 1) == 1; }));
                probe.Release(holdAt, 1);
            }

            IC_REQUIRE(source.WaitEntered(b));
            // B's read is in ReadFile on the same thread; any stray OS cancel
            // would abort it. Then let it complete.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            IC_CHECK(source.Aborted(b) == 0);
            source.WritePipe();
            IC_REQUIRE(WaitUntil([&] { return Delivered(done, 2); }));
            IC_REQUIRE(Idle(scheduler));
        }
        IC_CHECK(!bStartedEarly);
        IC_CHECK(source.Aborted(b) == 0);
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 2)));
        IC_CHECK(DeliveredHr(done, 1) == E_PENDING);
        IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
        IC_CHECK(probe.Count(Point::CancelDispatch, 2) == 0);
        if (cancelAt == Point::BeforeReadRegister)
        {
            IC_CHECK(source.Reads(a) == 0);
            IC_CHECK(probe.Count(Point::CancelDispatch, 1) == 0);
        }
        else
        {
            IC_CHECK(source.Reads(a) == 1);
            IC_CHECK(probe.Count(Point::CancelDispatch, 1) == 1);
        }
    }
}

IC_TEST(ReadCancel_LateCancelDispatchNeverReachesTheNextRead)
{
    RunLateCancelBarrier(ImagePurpose::Thumbnail, Point::CancelDispatch);
    RunLateCancelBarrier(ImagePurpose::FullResolution, Point::CancelDispatch);
}

IC_TEST(ReadCancel_CancelImmediatelyBeforeReadRegistration)
{
    RunLateCancelBarrier(ImagePurpose::Thumbnail, Point::BeforeReadRegister);
    RunLateCancelBarrier(ImagePurpose::FullResolution, Point::BeforeReadRegister);
}

IC_TEST(ReadCancel_CancelImmediatelyBeforeReadRetirement)
{
    RunLateCancelBarrier(ImagePurpose::Thumbnail, Point::BeforeReadRetire);
    RunLateCancelBarrier(ImagePurpose::FullResolution, Point::BeforeReadRetire);
}

IC_TEST(ReadCancel_CancelAfterRetirementNeverTargetsTheNextRead)
{
    // A's thumbnail read has retired and A waits for the (held) decode worker,
    // while B's read blocks in a pipe ReadFile on the same prefetch thread.
    // Canceling A must not OS-cancel B's read.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    WorkerGate gate;
    Watchdog watchdog("cancel after retirement", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring a = source.Entry(files.thumbs[22]);
    const std::wstring b = source.Entry(files.thumbs[23]);
    source.SetMode(b, Mode::Pipe);
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        OnExit release { [&] { gate.Release(); for (int i = 0; i < 4; ++i) { source.WritePipe(); } } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 9, gate.Callback(done, 9))));
        IC_REQUIRE(gate.WaitEntered());
        IC_REQUIRE(scheduler.Enqueue(MakeTask(a, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(b, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(b));
        IC_CHECK(scheduler.PrefetchedTaskCount() == 1);  // A, waiting for decode
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        scheduler.Cancel(1);
        IC_CHECK(scheduler.PrefetchedTaskCount() == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        IC_CHECK(source.Aborted(b) == 0);

        source.WritePipe();
        gate.Release();
        IC_REQUIRE(WaitUntil([&] { return Delivered(done, 2); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(probe.Count(Point::CancelDispatch, 1) == 0);
    IC_CHECK(source.Aborted(b) == 0);
    IC_CHECK(SUCCEEDED(DeliveredHr(done, 2)));
    IC_CHECK(DeliveredHr(done, 1) == E_PENDING);
    IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
}

IC_TEST(ReadCancel_TokenReleasesWaitForOtherThreadWork)
{
    // The host read waits (on its token) for work running on another thread,
    // here a pipe ReadFile on a helper thread. Cancel releases the read through
    // the token; the OS cancel attempt targets only the reading thread, so the
    // helper's ReadFile is not aborted and completes once data arrives.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("token wait for other-thread work", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring entry = source.Entry(files.thumbs[24]);
    source.SetMode(entry, Mode::OtherThread);
    // Keeps the read registered until the OS cancel attempt has been made.
    probe.HoldAt(Point::BeforeReadRetire, 1);
    DWORD readingThread = 0;
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        OnExit release { [&] { probe.ReleaseAll(); for (int i = 0; i < 4; ++i) { source.WritePipe(); } } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(source.WaitEntered(entry));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // helper is in ReadFile
        scheduler.Cancel(1);
        IC_CHECK(source.WaitReturned(entry, 1, std::chrono::milliseconds(5000)));
        IC_CHECK(source.StoppedReturns(entry) == 1);
        IC_CHECK(probe.WaitHeld(Point::BeforeReadRetire, 1));
        IC_CHECK(WaitUntil([&] { return probe.Count(Point::CancelDispatch, 1) == 1; }));
        probe.Release(Point::BeforeReadRetire, 1);
        readingThread = source.LastReadThread(entry);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        IC_CHECK(source.HelperAborted() == 0);
        IC_CHECK(source.HelperCompleted() == 0);  // still pending, untouched
        source.WritePipe();
        IC_CHECK(source.WaitHelperCompleted(1));
        IC_CHECK(source.HelperCompleted() == 1);
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(readingThread != 0 && readingThread != GetCurrentThreadId());
    IC_CHECK(probe.Count(Point::CancelDispatch, 1) == 1);
    IC_CHECK(probe.Count(Point::DecodeStart, 1) == 0);
    IC_CHECK(DeliveryCount(done) == 0);
}

// ---------------------------------------------------------------------------
// Bounded shutdown.

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

    bool IsSubset(const std::unordered_set<DWORD>& subset, const std::unordered_set<DWORD>& set)
    {
        return std::all_of(subset.begin(), subset.end(), [&](DWORD id) { return set.count(id) != 0; });
    }

    // Owns a SYNCHRONIZE handle to a thread, so its exit can be checked
    // without thread-id reuse.
    class ThreadWatch
    {
    public:
        explicit ThreadWatch(DWORD id)
            : m_handle(OpenThread(SYNCHRONIZE, FALSE, id))
        {
        }
        ~ThreadWatch()
        {
            if (m_handle)
            {
                CloseHandle(m_handle);
            }
        }
        ThreadWatch(const ThreadWatch&) = delete;
        ThreadWatch& operator=(const ThreadWatch&) = delete;

        bool Running() const { return m_handle && WaitForSingleObject(m_handle, 0) == WAIT_TIMEOUT; }
        bool Exited() const { return m_handle && WaitForSingleObject(m_handle, 0) == WAIT_OBJECT_0; }

    private:
        HANDLE m_handle;
    };

    long long ElapsedMs(std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
    }
}

IC_TEST(ReadCancel_BoundedShutdownReleasesCooperativeReads)
{
    // Reads blocked on their tokens on both decode workers (full resolution)
    // and on the prefetch thread, a prefetched thumbnail waiting for decode
    // (holding a prefetch slot) and queued requests: Shutdown(bound) releases the
    // reads, joins every thread well within the bound, invokes no callback and
    // leaves no request or slot behind.
    const auto& files = Files();
    FileByteCache& cache = FileByteCache::Instance();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("bounded shutdown, cooperative", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring fullB = source.Entry(files.thumbs[25]);
    const std::wstring fullD = source.Entry(files.thumbs[26]);
    const std::wstring thumbA = source.Entry(files.thumbs[27]);
    source.SetMode(fullB, Mode::Cooperative);
    source.SetMode(fullD, Mode::Cooperative);
    source.SetMode(thumbA, Mode::Cooperative);
    bool ok = false;
    long long elapsed = 0;
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(2);
        OnExit release { [&] { source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(fullB, ImagePurpose::FullResolution, 1, done.Callback(1))));
        IC_REQUIRE(source.WaitEntered(fullB));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(fullD, ImagePurpose::FullResolution, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(fullD));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(source.Entry(files.thumbs[28]), ImagePurpose::Thumbnail, 3, done.Callback(3))));
        IC_REQUIRE(WaitUntil([&] { return scheduler.PrefetchedTaskCount() == 1; }));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(thumbA, ImagePurpose::Thumbnail, 4, done.Callback(4))));
        IC_REQUIRE(source.WaitEntered(thumbA));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[29], ImagePurpose::Thumbnail, 5, done.Callback(5))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 6, done.Callback(6))));

        const auto start = std::chrono::steady_clock::now();
        ok = scheduler.Shutdown(std::chrono::milliseconds(5000));
        elapsed = ElapsedMs(start);
        IC_CHECK(ok);
        IC_CHECK(elapsed < 2000);
        IC_CHECK(scheduler.WorkerCount() == 0);
        IC_CHECK(scheduler.LiveRequestCount() == 0);
        IC_CHECK(scheduler.PrefetchedTaskCount() == 0);
        IC_CHECK(cache.Count() == 0);
        IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 7, done.Callback(7))));
    }
    std::printf("  bounded shutdown (cooperative reads): %s in %lld ms\n", ok ? "joined" : "timed out", elapsed);
    IC_CHECK(source.StoppedReturns(fullB) == 1);
    IC_CHECK(source.StoppedReturns(fullD) == 1);
    IC_CHECK(source.StoppedReturns(thumbA) == 1);
    for (uint64_t handle = 1; handle <= 6; ++handle)
    {
        IC_CHECK(probe.Count(Point::DecodeStart, handle) == 0);
    }
    IC_CHECK(DeliveryCount(done) == 0);
}

IC_TEST(ReadCancel_BoundedShutdownOsCancelsBlockedSynchronousRead)
{
    // Reads that ignore their token but block in a cancellable ReadFile, on the
    // prefetch thread and on the decode worker, are released by the OS cancel
    // attempt Shutdown(bound) makes through the read leases.
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("bounded shutdown, OS cancel", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring thumb = source.Entry(files.thumbs[30]);
    const std::wstring full = source.Entry(files.thumbs[31]);
    source.SetMode(thumb, Mode::Pipe);
    source.SetMode(full, Mode::Pipe);
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        OnExit release { [&] { for (int i = 0; i < 4; ++i) { source.WritePipe(); } } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(full, ImagePurpose::FullResolution, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(thumb, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(full));
        IC_REQUIRE(source.WaitEntered(thumb));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // both in ReadFile

        const auto start = std::chrono::steady_clock::now();
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(5000)));
        IC_CHECK(ElapsedMs(start) < 2000);
        IC_CHECK(scheduler.LiveRequestCount() == 0);
        IC_CHECK(scheduler.PrefetchedTaskCount() == 0);
    }
    IC_CHECK(source.Aborted(thumb) == 1);
    IC_CHECK(source.Aborted(full) == 1);
    IC_CHECK(DeliveryCount(done) == 0);
}

IC_TEST(ReadCancel_BoundedShutdownTimesOutOnNonCooperativeRead)
{
    // Reads that ignore both the token and OS cancellation, on the decode
    // worker and on the prefetch thread. Shutdown(bound) returns false after
    // about the bound without detaching: the stuck threads keep running, no new
    // request is accepted, and SetThreadCount neither restarts nor blocks. Once
    // the source is released a second call joins every thread; afterwards the
    // scheduler restarts normally.
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("bounded shutdown, non-cooperative", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring thumb = source.Entry(files.thumbs[32]);
    const std::wstring full = source.Entry(files.thumbs[33]);
    source.SetMode(thumb, Mode::IgnoreToken);
    source.SetMode(full, Mode::IgnoreToken);
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(2);
        OnExit release { [&] { source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(full, ImagePurpose::FullResolution, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(thumb, ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[34], ImagePurpose::Thumbnail, 3, done.Callback(3))));
        IC_REQUIRE(source.WaitEntered(full));
        IC_REQUIRE(source.WaitEntered(thumb));
        const ThreadWatch stuckWorker(source.LastReadThread(full));
        const ThreadWatch stuckPrefetch(source.LastReadThread(thumb));
        const size_t workers = scheduler.WorkerCount();
        IC_REQUIRE(workers == 2);

        auto start = std::chrono::steady_clock::now();
        IC_CHECK(!scheduler.Shutdown(std::chrono::milliseconds(300)));
        const long long elapsed = ElapsedMs(start);
        IC_CHECK(elapsed >= 290);
        IC_CHECK(elapsed < 2000);
        std::printf("  bounded shutdown (non-cooperative reads): timed out after %lld ms (bound 300 ms)\n", elapsed);
        IC_CHECK(stuckWorker.Running());
        IC_CHECK(stuckPrefetch.Running());
        IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 4, done.Callback(4))));
        IC_CHECK(scheduler.WorkerCount() == workers);

        // SetThreadCount neither restarts nor waits for the stuck threads.
        const auto threadsBefore = ProcessThreadIds();
        start = std::chrono::steady_clock::now();
        scheduler.SetThreadCount(3);
        IC_CHECK(ElapsedMs(start) < 1000);
        IC_CHECK(scheduler.WorkerCount() == workers);
        IC_CHECK(IsSubset(ProcessThreadIds(), threadsBefore));
        IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 5, done.Callback(5))));

        // Still stuck: a retry times out again.
        IC_CHECK(!scheduler.Shutdown(std::chrono::milliseconds(100)));

        source.Release();
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(5000)));
        IC_CHECK(stuckWorker.Exited());
        IC_CHECK(stuckPrefetch.Exited());
        IC_CHECK(scheduler.WorkerCount() == 0);
        IC_CHECK(scheduler.LiveRequestCount() == 0);
        IC_CHECK(scheduler.PrefetchedTaskCount() == 0);
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(100)));
        scheduler.Shutdown();

        // Restart after the successful join.
        scheduler.SetThreadCount(2);
        IC_CHECK(scheduler.WorkerCount() == 2);
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 6, done.Callback(6))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[35], ImagePurpose::Thumbnail, 7, done.Callback(7))));
        IC_REQUIRE(done.WaitFor(2));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 6)));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 7)));
    }
    IC_CHECK(DeliveryCount(done) == 2);
}

IC_TEST(ReadCancel_BoundedShutdownBeforeRequestsAndRepeated)
{
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("bounded shutdown, idle", std::chrono::seconds(60));
    DecodeScheduler scheduler;
    IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(2000)));
    IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(2000)));
    IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(0)));  // nothing left to join
    scheduler.Shutdown();
    IC_CHECK(scheduler.WorkerCount() == 0);
    IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 1, done.Callback(1))));

    scheduler.SetThreadCount(1);
    IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2, done.Callback(2))));
    IC_REQUIRE(done.WaitFor(1));
    IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(2000)));
    IC_CHECK(DeliveryCount(done) == 1);
}

// Background mode (thumbnail decodes, the prefetch read) runs a thread at base
// priority 4, which normal-priority load starves for seconds at a time, so a
// thread in it could not reach its exit within a shutdown bound. Shutdown
// raises every scheduler thread through its handle; these observe the raise
// from inside the in-flight work, so they need no CPU load.

namespace
{
    // Polls the calling thread's priority until it is at least `atLeast` or
    // the wait times out; returns the last value read.
    int WaitForOwnPriority(int atLeast)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        int priority = GetThreadPriority(GetCurrentThread());
        while (priority < atLeast && std::chrono::steady_clock::now() < deadline)
        {
            Sleep(1);
            priority = GetThreadPriority(GetCurrentThread());
        }
        return priority;
    }

    // Filesystem byte source whose read of one path records the reading
    // thread's priority, waits for the stop token, then waits for the thread's
    // priority to be raised (see WaitForOwnPriority) and returns nothing.
    class PriorityProbeSource final : public ScopedFileByteSource
    {
    public:
        using ScopedFileByteSource::ReadAll;

        explicit PriorityProbeSource(std::wstring probedPath)
            : m_probedPath(std::move(probedPath))
        {
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override
        {
            if (path != m_probedPath)
            {
                return ScopedFileByteSource::ReadAll(path);
            }
            const int before = GetThreadPriority(GetCurrentThread());
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_before = before;
                m_entered = true;
            }
            m_cv.notify_all();

            // Registered without m_mutex held: it runs inline if stop is
            // already requested.
            std::stop_callback onStop(stop, [this]()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopped = true;
                }
                m_cv.notify_all();
            });
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                (void)m_cv.wait_for(lock, kWaitTimeout, [&] { return m_stopped; });
            }
            const int after = WaitForOwnPriority(THREAD_PRIORITY_NORMAL);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_after = after;
            return {};
        }

        bool WaitEntered()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, kWaitTimeout, [&] { return m_entered; });
        }

        int Before() const { std::lock_guard<std::mutex> lock(m_mutex); return m_before; }
        int After() const { std::lock_guard<std::mutex> lock(m_mutex); return m_after; }
        bool Stopped() const { std::lock_guard<std::mutex> lock(m_mutex); return m_stopped; }

    private:
        std::wstring m_probedPath;
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        mutable bool m_entered { false };
        mutable bool m_stopped { false };
        mutable int m_before { THREAD_PRIORITY_ERROR_RETURN };
        mutable int m_after { THREAD_PRIORITY_ERROR_RETURN };
    };
}

IC_TEST(ReadCancel_ShutdownRaisesInFlightBackgroundDecode)
{
    // The callback of a thumbnail decode runs on the worker in background mode;
    // its delivery started before Shutdown, so it runs to completion while
    // Shutdown waits for the worker.
    const auto& files = Files();
    Watchdog watchdog("shutdown raises an in-flight decode", std::chrono::seconds(60));
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    std::atomic<int> before { THREAD_PRIORITY_ERROR_RETURN };
    std::atomic<int> after { THREAD_PRIORITY_ERROR_RETURN };

    DecodeScheduler scheduler;
    scheduler.SetThreadCount(1);
    IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[0], ImagePurpose::Thumbnail, 1,
        [&](PipelineResult&&)
        {
            before = GetThreadPriority(GetCurrentThread());
            {
                std::lock_guard<std::mutex> lock(mutex);
                entered = true;
            }
            cv.notify_all();
            after = WaitForOwnPriority(THREAD_PRIORITY_NORMAL);
        })));
    {
        std::unique_lock<std::mutex> lock(mutex);
        IC_REQUIRE(cv.wait_for(lock, kWaitTimeout, [&] { return entered; }));
    }

    IC_CHECK(scheduler.Shutdown(std::chrono::seconds(30)));
    IC_CHECK(before.load() < THREAD_PRIORITY_NORMAL);
    IC_CHECK(after.load() == THREAD_PRIORITY_ABOVE_NORMAL);
}

IC_TEST(ReadCancel_ShutdownRaisesInFlightPrefetchRead)
{
    // The prefetch thread reads thumbnail bytes in background mode; the read
    // honours its stop token, but must still be scheduled to return.
    const auto& files = Files();
    Watchdog watchdog("shutdown raises an in-flight prefetch read", std::chrono::seconds(60));
    PriorityProbeSource source(files.thumbs[1]);
    Completions done;

    DecodeScheduler scheduler;
    scheduler.SetThreadCount(1);
    IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[1], ImagePurpose::Thumbnail, 1, done.Callback(1))));
    IC_REQUIRE(source.WaitEntered());

    IC_CHECK(scheduler.Shutdown(std::chrono::seconds(30)));
    IC_CHECK(source.Stopped());
    IC_CHECK(source.Before() < THREAD_PRIORITY_NORMAL);
    IC_CHECK(source.After() == THREAD_PRIORITY_ABOVE_NORMAL);
    IC_CHECK(DeliveryCount(done) == 0);
}

// ---------------------------------------------------------------------------
// Shutdown under CPU saturation (floyd68/ImageCore#21). Opt-in: it loads every
// logical CPU for several seconds, so it is in no CTest group and does nothing
// unless IMAGECORE_LOAD_TESTS=1. Run it with
//   set IMAGECORE_LOAD_TESTS=1 && ImageCoreTests LoadShutdown_

namespace
{
    // Normal-priority busy threads, more than there are logical CPUs, until
    // destroyed.
    class CpuSaturation
    {
    public:
        CpuSaturation()
        {
            const unsigned count = (std::max)(std::thread::hardware_concurrency(), 1u) + 4;
            for (unsigned i = 0; i < count; ++i)
            {
                m_threads.emplace_back([this]()
                {
                    while (!m_stop.load(std::memory_order_relaxed))
                    {
                    }
                });
            }
        }

        ~CpuSaturation()
        {
            m_stop = true;
            for (auto& thread : m_threads)
            {
                thread.join();
            }
        }

    private:
        std::atomic<bool> m_stop { false };
        std::vector<std::thread> m_threads;
    };

    bool LoadTestsEnabled()
    {
        char value[8] {};
        return GetEnvironmentVariableA("IMAGECORE_LOAD_TESTS", value, sizeof(value)) > 0 && value[0] == '1';
    }
}

IC_TEST(LoadShutdown_BoundedUnderCpuSaturation)
{
    if (!LoadTestsEnabled())
    {
        std::printf("  skipped: set IMAGECORE_LOAD_TESTS=1 to run\n");
        return;
    }
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("shutdown under CPU saturation", std::chrono::seconds(120));
    CpuSaturation load;

    // The idle case of ReadCancel_BoundedShutdownBeforeRequestsAndRepeated:
    // threads that have just started (and, before the fix, entered background
    // mode) must still exit within the bound.
    constexpr int kRounds = 10;
    for (int round = 0; round < kRounds; ++round)
    {
        DecodeScheduler scheduler;
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(2000)));
        scheduler.SetThreadCount(1 + (round % 2));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution,
            static_cast<uint64_t>(round) + 1, done.Callback(static_cast<uint64_t>(round) + 1))));
        IC_REQUIRE(done.WaitFor(static_cast<size_t>(round) + 1));
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(2000)));
    }

    // Construct/shutdown cycles as in DecodeScheduler_ShutdownCyclesNeverHang.
    // A starved cycle takes seconds (11-40 s measured without the priority
    // raise); with it, a cycle only waits for its share of the loaded CPUs.
    constexpr int kCycles = 50;
    std::vector<double> cycleMs;
    const auto start = std::chrono::steady_clock::now();
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        const auto cycleStart = std::chrono::steady_clock::now();
        {
            DecodeScheduler scheduler;
            scheduler.SetThreadCount(1 + (cycle % 2));
        }
        cycleMs.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cycleStart).count());
    }
    const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::sort(cycleMs.begin(), cycleMs.end());
    std::printf("  %d cycles under load: total %.0f ms, median %.1f ms, max %.1f ms\n",
        kCycles, totalMs, cycleMs[cycleMs.size() / 2], cycleMs.back());
    IC_CHECK(cycleMs.back() < 2000.0);
}

IC_TEST(ReadCancel_StopCallbackDuringShutdownMayReenterScheduler)
{
    // Shutdown(bound) signals stop with no scheduler lock held: a stop callback
    // that queries the scheduler, cancels and enqueues (rejected) does not
    // deadlock, and a probe thread can take the queue lock meanwhile.
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("re-entrant stop callback at shutdown", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring entry = source.Entry(files.thumbs[36]);
    source.SetMode(entry, Mode::Cooperative);
    std::atomic<bool> reentered { false };
    std::atomic<bool> probeFinished { false };
    std::atomic<bool> enqueued { true };
    {
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        OnExit release { [&] { source.Release(); } };
        source.SetOnStop([&]
        {
            (void)scheduler.LiveRequestCount();
            (void)scheduler.WorkerCount();
            scheduler.SetPriority(2, RequestPriority::MainImage);
            scheduler.Cancel(2);
            enqueued = scheduler.Enqueue(MakeTask(files.thumbs[37], ImagePurpose::Thumbnail, 3, done.Callback(3)));
            std::mutex m;
            std::condition_variable cv;
            bool finished = false;
            std::thread probeThread([&]
            {
                (void)scheduler.PrefetchedTaskCount();
                std::lock_guard<std::mutex> lock(m);
                finished = true;
                cv.notify_all();
            });
            {
                std::unique_lock<std::mutex> lock(m);
                probeFinished = cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished; });
            }
            probeThread.join();
            reentered = true;
        });

        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[38], ImagePurpose::Thumbnail, 2, done.Callback(2))));
        IC_REQUIRE(source.WaitEntered(entry));
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(5000)));
        source.SetOnStop({});
    }
    IC_CHECK(source.StopCallbackThread() != 0);
    IC_CHECK(source.StopCallbackThread() != GetCurrentThreadId());
    IC_CHECK(source.StopCallbackThread() != source.LastReadThread(entry));
    IC_CHECK(reentered.load());
    IC_CHECK(probeFinished.load());
    IC_CHECK(!enqueued.load());
    IC_CHECK(DeliveryCount(done) == 0);
}

namespace
{
    // A host stop callback blocks (as one waiting on a mutex held by a stuck
    // read would). Neither Cancel nor a bounded Shutdown may run it on the
    // caller's thread: Cancel returns at once, and Shutdown(bound) times out
    // (keeping its threads) until the callback is released; a retry then joins.
    void RunBlockedStopCallback(bool cancelFirst)
    {
        const auto& files = Files();
        Completions done;
        Watchdog watchdog("blocked stop callback", std::chrono::seconds(60));
        ControlledSource source(files.dir);
        const std::wstring entry = source.Entry(files.thumbs[cancelFirst ? 1 : 0]);
        source.SetMode(entry, Mode::Cooperative);
        HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<DWORD> callbackThread { 0 };
        source.SetOnStop([&]
        {
            callbackThread = GetCurrentThreadId();
            SetEvent(entered);
            (void)WaitForSingleObject(gate, 10000);
        });
        {
            DecodeScheduler scheduler;
            scheduler.SetThreadCount(1);
            OnExit release { [&] { SetEvent(gate); source.Release(); } };

            IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
            IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[2], ImagePurpose::Thumbnail, 2, done.Callback(2))));
            IC_REQUIRE(source.WaitEntered(entry));

            if (cancelFirst)
            {
                const auto start = std::chrono::steady_clock::now();
                scheduler.Cancel(1);
                IC_CHECK(ElapsedMs(start) < 200);
                IC_REQUIRE(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0);
                IC_CHECK(callbackThread.load() != GetCurrentThreadId());
                IC_CHECK(callbackThread.load() != source.LastReadThread(entry));
                // Other scheduler calls keep working while the callback blocks.
                IC_CHECK(scheduler.LiveRequestCount() == 1);
            }

            auto start = std::chrono::steady_clock::now();
            IC_CHECK(!scheduler.Shutdown(std::chrono::milliseconds(300)));
            const long long elapsed = ElapsedMs(start);
            IC_CHECK(elapsed >= 290);
            IC_CHECK(elapsed < 2000);
            IC_CHECK(WaitForSingleObject(entered, 0) == WAIT_OBJECT_0);
            IC_CHECK(callbackThread.load() != 0);
            IC_CHECK(callbackThread.load() != GetCurrentThreadId());
            IC_CHECK(callbackThread.load() != source.LastReadThread(entry));
            IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 3, done.Callback(3))));
            IC_CHECK(scheduler.WorkerCount() == 1);
            std::printf("  blocked stop callback (%s): Shutdown(300 ms) timed out after %lld ms\n",
                cancelFirst ? "after Cancel" : "from Shutdown", elapsed);

            SetEvent(gate);
            start = std::chrono::steady_clock::now();
            IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(5000)));
            IC_CHECK(ElapsedMs(start) < 2000);
            IC_CHECK(scheduler.WorkerCount() == 0);
            IC_CHECK(scheduler.LiveRequestCount() == 0);
            IC_CHECK(scheduler.PrefetchedTaskCount() == 0);
        }
        source.SetOnStop({});
        CloseHandle(gate);
        CloseHandle(entered);
        IC_CHECK(source.StoppedReturns(entry) == 1);
        IC_CHECK(DeliveryCount(done) == 0);
    }
}

IC_TEST(ReadCancel_BlockedStopCallbackDuringShutdownTimesOut)
{
    RunBlockedStopCallback(false);
}

IC_TEST(ReadCancel_BlockedStopCallbackAfterCancelTimesOutShutdown)
{
    RunBlockedStopCallback(true);
}

namespace
{
    // A thread_local whose destructor blocks on a gate (bounded): it runs after
    // the scheduler thread function returned, during thread exit.
    HANDLE g_tlsGate = nullptr;

    struct BlockingTlsObject
    {
        bool touched { false };
        ~BlockingTlsObject()
        {
            if (touched && g_tlsGate)
            {
                (void)WaitForSingleObject(g_tlsGate, 10000);
            }
        }
    };

    thread_local BlockingTlsObject t_blockingTls;

    // Filesystem source whose reads create the blocking thread_local on the
    // reading thread (as a host library might).
    class TlsTouchingSource final : public IPathByteSource
    {
    public:
        TlsTouchingSource() : m_previous(GetPathByteSource()) { SetPathByteSource(this); }
        ~TlsTouchingSource() override { SetPathByteSource(m_previous); }

        std::wstring ResolveHostPath(const std::wstring& path) const override { return path; }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            t_blockingTls.touched = true;
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
        }

    private:
        IPathByteSource* m_previous;
    };
}

IC_TEST(ReadCancel_BoundedShutdownWaitsForThreadTermination)
{
    // The prefetch thread signals exit, but its thread_local destructor then
    // blocks. Shutdown(bound) must wait for real thread termination within the
    // bound and return false (keeping the thread) instead of blocking in join();
    // once the destructor is released a retry joins.
    const auto& files = Files();
    Completions done;
    Watchdog watchdog("TLS destructor at thread exit", std::chrono::seconds(60));
    g_tlsGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        TlsTouchingSource source;
        DecodeScheduler scheduler;
        scheduler.SetThreadCount(1);
        OnExit release { [&] { SetEvent(g_tlsGate); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(files.thumbs[3], ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(done.WaitFor(1));
        IC_CHECK(SUCCEEDED(DeliveredHr(done, 1)));

        auto start = std::chrono::steady_clock::now();
        IC_CHECK(!scheduler.Shutdown(std::chrono::milliseconds(300)));
        const long long elapsed = ElapsedMs(start);
        IC_CHECK(elapsed >= 290);
        IC_CHECK(elapsed < 2000);
        IC_CHECK(scheduler.WorkerCount() == 1);
        IC_CHECK(!scheduler.Enqueue(MakeTask(files.fullRes, ImagePurpose::FullResolution, 2, done.Callback(2))));
        std::printf("  TLS destructor: Shutdown(300 ms) timed out after %lld ms\n", elapsed);

        // Still blocked: a retry times out again without blocking.
        start = std::chrono::steady_clock::now();
        IC_CHECK(!scheduler.Shutdown(std::chrono::milliseconds(100)));
        IC_CHECK(ElapsedMs(start) < 2000);

        SetEvent(g_tlsGate);
        IC_CHECK(scheduler.Shutdown(std::chrono::milliseconds(5000)));
        IC_CHECK(scheduler.WorkerCount() == 0);
        IC_CHECK(scheduler.LiveRequestCount() == 0);
    }
    CloseHandle(g_tlsGate);
    g_tlsGate = nullptr;
    IC_CHECK(DeliveryCount(done) == 1);
}

IC_TEST(ReadCancel_ShutdownWhileCancelIsPublishingStillStopsTheRead)
{
    // Cancel is paused right after it removed the request from the live table
    // (CancelPublished). A Shutdown running then no longer finds the request in
    // the table, so the stop must already be queued for the dispatch thread:
    // the token-only read is released and Shutdown(bound) joins in time.
    const auto& files = Files();
    Completions done;
    HookProbe probe;
    Watchdog watchdog("shutdown during cancel publication", std::chrono::seconds(60));
    ControlledSource source(files.dir);
    const std::wstring entry = source.Entry(files.thumbs[4]);
    source.SetMode(entry, Mode::Cooperative);
    probe.HoldAt(Point::CancelPublished, 1);
    bool ok = false;
    long long elapsed = 0;
    {
        DecodeScheduler scheduler;
        scheduler.SetTestHook(probe.Hook());
        scheduler.SetThreadCount(1);
        OnExit release { [&] { probe.ReleaseAll(); source.Release(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, ImagePurpose::Thumbnail, 1, done.Callback(1))));
        IC_REQUIRE(source.WaitEntered(entry));
        std::thread canceler([&] { scheduler.Cancel(1); });
        IC_REQUIRE(probe.WaitHeld(Point::CancelPublished, 1));
        IC_CHECK(scheduler.LiveRequestCount() == 0);

        const auto start = std::chrono::steady_clock::now();
        ok = scheduler.Shutdown(std::chrono::milliseconds(3000));
        elapsed = ElapsedMs(start);
        probe.Release(Point::CancelPublished, 1);
        canceler.join();
        IC_CHECK(ok);
        IC_CHECK(elapsed < 2000);
    }
    std::printf("  shutdown during cancel publication: %s in %lld ms\n", ok ? "joined" : "timed out", elapsed);
    IC_CHECK(source.StoppedReturns(entry) == 1);
    IC_CHECK(DeliveryCount(done) == 0);
}

namespace
{
    // A handle-0 task is in no live table, so Shutdown can stop its read only
    // through the lease scan. Hold the reading thread just before it registers
    // the read, start Shutdown, let it scan the leases (nothing registered) and
    // wait until the drained dispatch thread has exited, then let the thread
    // register: the read must not start, and Shutdown(bound) joins in time.
    void RunRegisterAfterShutdownBegan(ImagePurpose purpose)
    {
        const auto& files = Files();
        Completions done;
        HookProbe probe;
        Watchdog watchdog("read registering after shutdown began", std::chrono::seconds(60));
        ControlledSource source(files.dir);
        const std::wstring entry = source.Entry(files.thumbs[purpose == ImagePurpose::Thumbnail ? 5 : 6]);
        source.SetMode(entry, Mode::Cooperative);
        probe.HoldAt(Point::BeforeReadRegister, 0);
        std::atomic<bool> ok { false };
        std::atomic<long long> elapsed { 0 };
        {
            DecodeScheduler scheduler;
            scheduler.SetTestHook(probe.Hook());
            scheduler.SetThreadCount(1);
            OnExit release { [&] { probe.ReleaseAll(); source.Release(); } };

            IC_REQUIRE(scheduler.Enqueue(MakeTask(entry, purpose, 0, done.Callback(0))));
            IC_REQUIRE(probe.WaitHeld(Point::BeforeReadRegister, 0));
            IC_CHECK(scheduler.LiveRequestCount() == 0);

            // SetThreadCount may have retired an earlier dispatch thread.
            const size_t exitsBefore = probe.Count(Point::StopDispatchExit, 0);
            std::thread shutdownThread([&]
            {
                const auto start = std::chrono::steady_clock::now();
                ok = scheduler.Shutdown(std::chrono::milliseconds(3000));
                elapsed = ElapsedMs(start);
            });
            IC_CHECK(WaitUntil([&] { return probe.Count(Point::StopDispatchExit, 0) == exitsBefore + 1; }));
            probe.Release(Point::BeforeReadRegister, 0);
            shutdownThread.join();
            IC_CHECK(ok.load());
            IC_CHECK(elapsed.load() < 2000);
        }
        std::printf("  %s read registering after shutdown began: %s in %lld ms\n",
            purpose == ImagePurpose::Thumbnail ? "thumbnail" : "full-resolution",
            ok.load() ? "joined" : "timed out", elapsed.load());
        IC_CHECK(source.Reads(entry) == 0);
        IC_CHECK(probe.Count(Point::DecodeStart, 0) == 0);
        IC_CHECK(DeliveryCount(done) == 0);
    }
}

IC_TEST(ReadCancel_ReadRegisteringAfterShutdownBeganDoesNotStart)
{
    RunRegisterAfterShutdownBegan(ImagePurpose::Thumbnail);
    RunRegisterAfterShutdownBegan(ImagePurpose::FullResolution);
}

IC_TEST(ReadCancel_CancelFinishShutdownRaceStress)
{
    // Cancel, read completion and bounded shutdown race: a canceler thread keeps
    // canceling (also after Shutdown returned, when the lease handles are
    // closed) while requests read, decode and finish. Every cycle must join
    // within the bound, deliver nothing after Shutdown returned and leave no
    // request or prefetch slot behind.
    const auto& files = Files();
    constexpr int kCycles = 150;
    Watchdog watchdog("cancel/finish/shutdown stress", std::chrono::seconds(90));
    int timedOut = 0;
    int lateDeliveries = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        ControlledSource source(files.dir);
        source.SetJitter(500);
        std::atomic<bool> shutDown { false };
        std::atomic<int> late { 0 };
        {
            DecodeScheduler scheduler;
            scheduler.SetThreadCount(1 + (cycle % 3));
            OnExit release { [&] { source.Release(); } };

            constexpr uint64_t kRequests = 24;
            for (uint64_t handle = 1; handle <= kRequests; ++handle)
            {
                const std::wstring entry = source.Entry(files.thumbs[(cycle + handle) % files.thumbs.size()]);
                const bool full = (handle % 3) == 0;
                scheduler.Enqueue(MakeTask(entry, full ? ImagePurpose::FullResolution : ImagePurpose::Thumbnail, handle,
                    [&](PipelineResult&&)
                    {
                        if (shutDown.load())
                        {
                            late.fetch_add(1);
                        }
                    }));
            }
            std::atomic<bool> stop { false };
            std::thread canceler([&]
            {
                uint32_t rng = static_cast<uint32_t>(cycle) * 2654435761u;
                while (!stop.load())
                {
                    rng = rng * 1664525u + 1013904223u;
                    scheduler.Cancel(1 + (rng >> 8) % kRequests);
                    std::this_thread::yield();
                }
            });
            std::this_thread::sleep_for(std::chrono::microseconds((cycle * 37) % 3000));
            if (!scheduler.Shutdown(std::chrono::milliseconds(5000)))
            {
                ++timedOut;
            }
            shutDown.store(true);
            IC_CHECK(scheduler.LiveRequestCount() == 0);
            IC_CHECK(scheduler.PrefetchedTaskCount() == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            stop.store(true);
            canceler.join();
        }
        lateDeliveries += late.load();
    }
    IC_CHECK(timedOut == 0);
    IC_CHECK(lateDeliveries == 0);
}
