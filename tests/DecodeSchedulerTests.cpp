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
