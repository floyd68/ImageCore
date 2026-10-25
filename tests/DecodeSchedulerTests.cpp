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
