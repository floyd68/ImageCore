#include "TestHarness.h"

#include "DecodeScheduler.h"
#include "FileByteCache.h"
#include "IPathByteSource.h"
#include "ImageCore.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// I/O lane tests (floyd68/ImageCore#15). Paths are virtual: a fake drive letter
// ("D:", "F:", ...) or a UNC server, mapped by LaneSource to small BMP files in
// a temporary directory, and placed on simulated devices by FakeTopology. A
// path is blocked in LaneSource until the test unblocks it (or its request is
// canceled), so a test controls exactly which reads are slow.
//
// The tests assert progress, never throughput: a request "completes" when its
// callback runs before a generous deadline (a timeout prints the scheduler
// state), and "does not start" when its read has not begun after a quiet
// period while the read it waits for is still blocked (a slow machine can
// only make such a check pass late, not fail). Files are read back once at
// normal priority before any test, so no timed step depends on a first open.

namespace
{
    using namespace ImageCore;
    using Topology = DecodeScheduler::VolumeTopology;

    constexpr auto kDeadline = std::chrono::seconds(20);
    constexpr auto kQuiet = std::chrono::milliseconds(300);

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

    struct LaneFiles
    {
        std::filesystem::path dir;
        std::wstring fullRes;  // a real local file (C: temp), for local full-resolution requests

        LaneFiles()
        {
            wchar_t tempPath[MAX_PATH] {};
            GetTempPathW(MAX_PATH, tempPath);
            dir = std::filesystem::path(tempPath) / (L"ImageCoreLaneTests_" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directories(dir);
            for (int i = 0; i < 64; ++i)
            {
                WriteBmp(dir / (L"t" + std::to_wstring(i) + L".bmp"), 32, 32);
            }
            WriteBmp(dir / L"full.bmp", 16, 8);
            fullRes = (dir / L"full.bmp").wstring();

            // First opens happen here, at normal priority (see the file comment).
            for (const auto& entry : std::filesystem::directory_iterator(dir))
            {
                std::ifstream in(entry.path(), std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
                (void)bytes;
            }

            RegisterBuiltInDecoders();
            ImageCoreTest::RegisterTeardown([this]()
            {
                std::error_code ec;
                std::filesystem::remove_all(dir, ec);
            });
        }
    };

    LaneFiles& Files()
    {
        static LaneFiles files;
        return files;
    }

    std::wstring Name(int i)
    {
        return L"t" + std::to_wstring(i) + L".bmp";
    }

    // Virtual paths. LaneSource classifies them lexically: "<x>.zip\" inside
    // -> Archive, UNC or "\cloud\" -> Remote, "\unknown\" -> Unknown, else Local.
    std::wstring LocalPath(const wchar_t* volume, int i) { return std::wstring(volume) + L"\\pics\\" + Name(i); }
    std::wstring UnknownPath(const wchar_t* volume, int i) { return std::wstring(volume) + L"\\unknown\\" + Name(i); }
    std::wstring ThrowingPath(const wchar_t* volume, int i) { return std::wstring(volume) + L"\\throw\\" + Name(i); }
    std::wstring CloudPath(const wchar_t* volume, int i) { return std::wstring(volume) + L"\\cloud\\" + Name(i); }
    std::wstring EntryPath(const wchar_t* volume, const wchar_t* archive, int i)
    {
        return std::wstring(volume) + L"\\packs\\" + archive + L"\\" + Name(i);
    }
    std::wstring UncPath(const wchar_t* server, int i)
    {
        return std::wstring(L"\\\\") + server + L"\\share\\" + Name(i);
    }

    // Simulated devices: D: and E: are two volumes of one HDD, F: and H: are
    // HDDs of their own, G: and K: are SSDs; any other volume is its own HDD.
    // Counts the queries per volume.
    class FakeTopology
    {
    public:
        FakeTopology()
        {
            m_volumes[L"D:"] = { L"hdd1", true };
            m_volumes[L"E:"] = { L"hdd1", true };
            m_volumes[L"F:"] = { L"hdd2", true };
            m_volumes[L"H:"] = { L"hdd3", true };
            m_volumes[L"G:"] = { L"ssd1", false };
            m_volumes[L"K:"] = { L"ssd2", false };
        }

        DecodeScheduler::VolumeTopologyQuery Query()
        {
            return [this](const std::wstring& volume)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_calls[volume];
                return Lookup(volume);
            };
        }

        size_t Calls(const std::wstring& volume) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_calls.find(volume);
            return it == m_calls.end() ? 0 : it->second;
        }

        size_t TotalCalls() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            size_t total = 0;
            for (const auto& [volume, calls] : m_calls)
            {
                total += calls;
            }
            return total;
        }

        // Simulated device of a virtual path, for concurrency peaks.
        std::wstring DeviceOf(const std::wstring& path) const
        {
            if (path.rfind(L"\\\\", 0) == 0 || path.find(L"\\cloud\\") != std::wstring::npos)
            {
                return L"remote";
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            return Lookup(path.substr(0, 2)).device;
        }

    private:
        Topology Lookup(const std::wstring& volume) const
        {
            auto it = m_volumes.find(volume);
            return it != m_volumes.end() ? it->second : Topology { L"own:" + volume, true };
        }

        mutable std::mutex m_mutex;
        std::map<std::wstring, Topology> m_volumes;
        std::map<std::wstring, size_t> m_calls;
    };

    // Byte source for virtual paths with per-path blocking. A blocked read
    // waits until Unblock or until its request is canceled (it honours the
    // token, then returns nothing). Records reads in start order and the peak
    // number of concurrent reads, in total and per simulated device.
    class LaneSource final : public IPathByteSource
    {
    public:
        LaneSource(const std::filesystem::path& dir, const FakeTopology& topology, bool classify = true)
            : m_previous(GetPathByteSource())
            , m_dir(dir)
            , m_topology(topology)
            , m_classify(classify)
        {
            SetPathByteSource(this);
        }

        ~LaneSource() override
        {
            SetPathByteSource(m_previous);
            UnblockAll();
        }

        void Block(const std::wstring& path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_blocked.insert(path);
        }

        void Unblock(const std::wstring& path)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_blocked.erase(path);
            }
            m_cv.notify_all();
        }

        void UnblockAll()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_blocked.clear();
            }
            m_cv.notify_all();
        }

        std::wstring ResolveHostPath(const std::wstring& path) const override
        {
            const size_t zip = path.find(L".zip\\");
            return zip == std::wstring::npos ? path : path.substr(0, zip + 4);
        }

        PathSourceKind ClassifyPath(const std::wstring& path) const override
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_classified;
            }
            if (path.find(L"\\throw\\") != std::wstring::npos)
            {
                throw std::runtime_error("classification failed");
            }
            if (!m_classify)
            {
                return IPathByteSource::ClassifyPath(path);  // the conservative default
            }
            if (path.find(L".zip\\") != std::wstring::npos)
            {
                return PathSourceKind::Archive;
            }
            if (path.rfind(L"\\\\", 0) == 0 || path.find(L"\\cloud\\") != std::wstring::npos)
            {
                return PathSourceKind::Remote;
            }
            if (path.find(L"\\unknown\\") != std::wstring::npos)
            {
                return PathSourceKind::Unknown;
            }
            return PathSourceKind::Local;
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path) const override
        {
            return ReadAll(path, std::stop_token {});
        }

        std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override
        {
            const std::wstring device = m_topology.DeviceOf(path);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_reads[path];
                m_order.push_back(path);
                m_peak = (std::max)(m_peak, ++m_active);
                size_t& onDevice = m_activeByDevice[device];
                ++onDevice;
                m_peakByDevice[device] = (std::max)(m_peakByDevice[device], onDevice);
            }
            m_cv.notify_all();

            bool stopped = false;
            {
                std::stop_callback wake(stop, [this]
                {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                    }
                    m_cv.notify_all();
                });
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, kDeadline * 3, [&] { return stop.stop_requested() || !m_blocked.count(path); });
                stopped = stop.stop_requested();
            }

            std::vector<uint8_t> bytes;
            if (!stopped)
            {
                std::ifstream in(m_dir / std::filesystem::path(path).filename(), std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), {});
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_active;
                --m_activeByDevice[device];
                ++m_returned[path];
                m_stoppedReturns += stopped ? 1 : 0;
            }
            m_cv.notify_all();
            return bytes;
        }

        size_t Reads(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_reads.find(path);
            return it == m_reads.end() ? 0 : it->second;
        }

        size_t Returned(const std::wstring& path) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_returned.find(path);
            return it == m_returned.end() ? 0 : it->second;
        }

        size_t TotalReads() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_order.size();
        }

        std::vector<std::wstring> Order() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_order;
        }

        size_t Peak() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_peak;
        }

        size_t PeakOn(const std::wstring& device) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_peakByDevice.find(device);
            return it == m_peakByDevice.end() ? 0 : it->second;
        }

        size_t StoppedReturns() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_stoppedReturns;
        }

        void ResetPeaks()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_peak = m_active;
            for (auto& [device, peak] : m_peakByDevice)
            {
                peak = m_activeByDevice[device];
            }
        }

    private:
        IPathByteSource* m_previous;
        std::filesystem::path m_dir;
        const FakeTopology& m_topology;
        bool m_classify;

        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        std::set<std::wstring> m_blocked;
        mutable std::map<std::wstring, size_t> m_reads;
        mutable std::map<std::wstring, size_t> m_returned;
        mutable std::vector<std::wstring> m_order;
        mutable size_t m_active { 0 };
        mutable size_t m_peak { 0 };
        mutable std::map<std::wstring, size_t> m_activeByDevice;
        mutable std::map<std::wstring, size_t> m_peakByDevice;
        mutable size_t m_stoppedReturns { 0 };
        mutable size_t m_classified { 0 };
    };

    // Lane-test scheduler: simulated topology, and thumbnail work at normal
    // priority (scheduling logic only; see DecodeSchedulerTests.cpp).
    class LaneScheduler : public DecodeScheduler
    {
    public:
        explicit LaneScheduler(FakeTopology& topology, size_t workers)
        {
            SetBackgroundWorkPriority(BackgroundWorkPriority::Normal);
            SetVolumeTopologyQuery(topology.Query());
            SetThreadCount(workers);
        }
    };

    void PrintDiagnostics(const DecodeScheduler& scheduler)
    {
        const auto d = scheduler.GetDiagnostics();
        std::fprintf(stderr, "  scheduler: queued high=%zu io=%zu main-io=%zu decode=%zu live=%zu prefetched=%zu+%zu/%zu main=%zu lanes=%zu\n",
            d.highQueued, d.ioQueued, d.mainIoQueued, d.decodeQueued, d.liveRequests, d.prefetchedTasks,
            d.prefetchReads, d.prefetchCap, d.mainInputs, d.lanes);
        for (const auto& t : d.threads)
        {
            std::fprintf(stderr, "    thread %lu role=%d lane=%d phase=%d handle=%llu priority=%d cpu=%.1f ms\n",
                t.threadId, static_cast<int>(t.role), static_cast<int>(t.lane), static_cast<int>(t.phase),
                static_cast<unsigned long long>(t.handle), t.priority, t.cpuMs);
        }
        std::fflush(stderr);
    }

    // Waits (up to kDeadline) until `predicate` holds; prints `what` and the
    // scheduler state on a timeout.
    template <typename Predicate>
    bool WaitUntil(const DecodeScheduler& scheduler, const char* what, Predicate predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::fprintf(stderr, "  timeout after %lld s: %s\n", static_cast<long long>(kDeadline.count()), what);
                PrintDiagnostics(scheduler);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    // Completions by handle.
    struct Completions
    {
        std::mutex mutex;
        std::map<uint64_t, HRESULT> results;

        std::function<void(PipelineResult&&)> Callback(uint64_t handle)
        {
            return [this, handle](PipelineResult&& result)
            {
                std::lock_guard<std::mutex> lock(mutex);
                results[handle] = result.hr;
            };
        }

        bool Done(uint64_t handle)
        {
            std::lock_guard<std::mutex> lock(mutex);
            return results.count(handle) != 0;
        }

        bool Succeeded(uint64_t handle)
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = results.find(handle);
            return it != results.end() && SUCCEEDED(it->second);
        }

        bool AllDone(uint64_t first, uint64_t last)
        {
            for (uint64_t handle = first; handle <= last; ++handle)
            {
                if (!Done(handle))
                {
                    return false;
                }
            }
            return true;
        }

        size_t Count()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return results.size();
        }
    };

    // Holds a decode worker inside a callback until released.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered { false };
        bool released { false };

        std::function<void(PipelineResult&&)> Callback(Completions& done, uint64_t handle)
        {
            return [this, inner = done.Callback(handle)](PipelineResult&& result) mutable
            {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    entered = true;
                    cv.notify_all();
                    cv.wait_for(lock, kDeadline * 3, [&] { return released; });
                }
                inner(std::move(result));
            };
        }

        bool Entered()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return entered;
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

    // Runs on scope exit; declare after the scheduler so blocked reads and
    // held callbacks are released before the scheduler joins its threads.
    struct OnExit
    {
        std::function<void()> action;
        ~OnExit() { action(); }
    };

    // Fails the process with a message if a test outlives `limit`.
    class Watchdog
    {
    public:
        explicit Watchdog(const char* what, std::chrono::seconds limit = std::chrono::seconds(180))
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

    DecodeTask MakeTask(const std::wstring& path, ImagePurpose purpose, uint64_t handle,
        std::function<void(PipelineResult&&)> callback, RequestPriority priority)
    {
        DecodeTask task;
        task.request = ImageRequest(path, purpose);
        task.request.allowGpuCompressedDDS = false;
        if (purpose != ImagePurpose::FullResolution)
        {
            task.request.targetSize = { 16.0f, 16.0f };
        }
        task.handle = handle;
        task.priority = priority;
        task.callback = std::move(callback);
        return task;
    }

    DecodeTask Thumb(const std::wstring& path, uint64_t handle, Completions& done,
        RequestPriority priority = RequestPriority::Visible)
    {
        return MakeTask(path, ImagePurpose::Thumbnail, handle, done.Callback(handle), priority);
    }

    DecodeTask Full(const std::wstring& path, uint64_t handle, Completions& done)
    {
        return MakeTask(path, ImagePurpose::FullResolution, handle, done.Callback(handle), RequestPriority::MainImage);
    }

    // Passes when `path` has not been read after the quiet period.
    bool StaysUnread(const LaneSource& source, const std::wstring& path)
    {
        std::this_thread::sleep_for(kQuiet);
        return source.Reads(path) == 0;
    }

    bool Idle(const DecodeScheduler& scheduler)
    {
        return WaitUntil(scheduler, "scheduler idle", [&]
        {
            const auto d = scheduler.GetDiagnostics();
            return d.liveRequests == 0 && d.prefetchedTasks == 0 && d.prefetchReads == 0 && d.mainInputs == 0 &&
                d.lanes == 0;
        });
    }

    long long MsSince(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }
}

IC_TEST(Lanes_LaneMapping)
{
    IC_CHECK(IoLaneFor(PathSourceKind::Unknown) == IoLane::Local);
    IC_CHECK(IoLaneFor(PathSourceKind::Local) == IoLane::Local);
    IC_CHECK(IoLaneFor(PathSourceKind::Archive) == IoLane::Archive);
    IC_CHECK(IoLaneFor(PathSourceKind::Remote) == IoLane::Remote);

    // The default classification: archive when the host path differs, else Unknown.
    FakeTopology topology;
    LaneSource source(Files().dir, topology, false);
    const IPathByteSource& base = source;
    IC_CHECK(base.ClassifyPath(EntryPath(L"D:", L"a.zip", 0)) == PathSourceKind::Archive);
    IC_CHECK(base.ClassifyPath(LocalPath(L"D:", 0)) == PathSourceKind::Unknown);
    IC_CHECK(base.ClassifyPath(UncPath(L"server1", 0)) == PathSourceKind::Unknown);
    IC_CHECK(base.ClassifyPath(L"") == PathSourceKind::Unknown);
}

IC_TEST(Lanes_BlockedArchiveAndRemoteReadsDoNotHoldUpLocalThumbnails)
{
    // One slow read in each slow lane (a solid-archive entry, a network file,
    // a cloud placeholder) stays blocked while a local folder's thumbnails all
    // complete; no local device query is made for the remote paths.
    Watchdog watchdog("slow lanes vs local thumbnails");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring archive = EntryPath(L"F:", L"a.zip", 0);
    const std::wstring network = UncPath(L"server1", 1);
    const std::wstring cloud = CloudPath(L"C:", 2);
    source.Block(archive);
    source.Block(network);
    source.Block(cloud);
    long long localMs = 0;
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(archive, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(network, 2, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(cloud, 3, done)));
        IC_REQUIRE(WaitUntil(scheduler, "slow reads started", [&]
        {
            return source.Reads(archive) == 1 && source.Reads(network) == 1 && source.Reads(cloud) == 1;
        }));

        const auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < 12; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"D:", 10 + static_cast<int>(i)), 10 + i, done)));
        }
        IC_REQUIRE(WaitUntil(scheduler, "local thumbnails", [&] { return done.AllDone(10, 21); }));
        localMs = MsSince(start);
        for (uint64_t handle = 10; handle <= 21; ++handle)
        {
            IC_CHECK(done.Succeeded(handle));
        }
        // ...while every slow read is still blocked.
        IC_CHECK(source.Returned(archive) == 0);
        IC_CHECK(source.Returned(network) == 0);
        IC_CHECK(source.Returned(cloud) == 0);
        IC_CHECK(!done.Done(1) && !done.Done(2) && !done.Done(3));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "slow reads delivered", [&] { return done.AllDone(1, 3); }));
        IC_CHECK(done.Succeeded(1) && done.Succeeded(2) && done.Succeeded(3));
        IC_REQUIRE(Idle(scheduler));
    }
    // Remote lanes never query a local device; only D: and F: were placed.
    IC_CHECK(topology.Calls(L"C:") == 0);
    IC_CHECK(topology.Calls(L"D:") == 1);
    IC_CHECK(topology.Calls(L"F:") == 1);
    IC_CHECK(topology.TotalCalls() == 2);
    std::printf("  12 local thumbnails completed in %lld ms with archive, network and cloud reads blocked\n", localMs);
}

IC_TEST(Lanes_SeparateDevicesProgressIndependently)
{
    // A blocked read on one device does not hold up a lane of the same kind on
    // another device; on an SSD every archive is its own lane.
    Watchdog watchdog("separate devices");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring localD = LocalPath(L"D:", 0);
    const std::wstring archiveF = EntryPath(L"F:", L"a.zip", 1);
    const std::wstring ssdA = EntryPath(L"G:", L"a.zip", 2);
    const std::wstring ssdB = EntryPath(L"G:", L"b.zip", 3);
    source.Block(localD);
    source.Block(archiveF);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(localD, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(archiveF, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "blocked reads started", [&]
        {
            return source.Reads(localD) == 1 && source.Reads(archiveF) == 1;
        }));

        // Local F: (hdd2) and an archive on H: (hdd3) complete.
        for (uint64_t i = 0; i < 4; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 10 + static_cast<int>(i)), 10 + i, done)));
            IC_REQUIRE(scheduler.Enqueue(Thumb(EntryPath(L"H:", L"c.zip", 20 + static_cast<int>(i)), 20 + i, done)));
        }
        IC_REQUIRE(WaitUntil(scheduler, "other devices", [&] { return done.AllDone(10, 13) && done.AllDone(20, 23); }));
        IC_CHECK(source.Returned(localD) == 0 && source.Returned(archiveF) == 0);
        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "blocked reads delivered", [&] { return done.AllDone(1, 2); }));

        // Two archives of one SSD read concurrently, beside local reads of it.
        source.Block(ssdA);
        source.Block(ssdB);
        IC_REQUIRE(scheduler.Enqueue(Thumb(ssdA, 30, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(ssdB, 31, done)));
        IC_REQUIRE(WaitUntil(scheduler, "both SSD archives reading", [&]
        {
            return source.Reads(ssdA) == 1 && source.Reads(ssdB) == 1;
        }));
        for (uint64_t i = 0; i < 4; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"G:", 40 + static_cast<int>(i)), 40 + i, done)));
        }
        IC_REQUIRE(WaitUntil(scheduler, "SSD local thumbnails", [&] { return done.AllDone(40, 43); }));
        IC_CHECK(source.PeakOn(L"ssd1") == 3);
        IC_CHECK(source.PeakOn(L"ssd1") <= DecodeScheduler::kMaxReadsPerSolidStateDevice);
        IC_CHECK(!done.Done(30) && !done.Done(31));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "all delivered", [&] { return done.AllDone(30, 31); }));
        IC_REQUIRE(Idle(scheduler));
    }
}

IC_TEST(Lanes_OneHddAdmitsOneReadPerLaneKind)
{
    // Same-HDD policy (bounded override): on one HDD a blocked local read does
    // not stop archive reads, but each lane kind stays serialized there, so the
    // HDD never has more than kMaxReadsPerSeekDevice reads at once.
    Watchdog watchdog("one HDD");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring localD = LocalPath(L"D:", 0);
    const std::wstring localE = LocalPath(L"E:", 1);           // same HDD, other volume
    const std::wstring archiveB = EntryPath(L"E:", L"b.zip", 7);
    const std::wstring archiveA = EntryPath(L"D:", L"a.zip", 8);
    source.Block(localD);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(localD, 1, done)));
        IC_REQUIRE(WaitUntil(scheduler, "local read started", [&] { return source.Reads(localD) == 1; }));
        IC_REQUIRE(scheduler.Enqueue(Thumb(localE, 2, done)));

        // The archive lane of the same HDD progresses past the blocked local read.
        for (uint64_t i = 0; i < 4; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(EntryPath(L"D:", L"a.zip", 2 + static_cast<int>(i)), 3 + i, done)));
        }
        IC_REQUIRE(WaitUntil(scheduler, "archive on the same HDD", [&] { return done.AllDone(3, 6); }));
        // The other local volume of the HDD waits for the device's local lane.
        IC_CHECK(StaysUnread(source, localE));

        // With an archive read blocked too, the HDD is at its bound: another
        // archive of that HDD waits (one archive lane per HDD).
        source.Block(archiveB);
        IC_REQUIRE(scheduler.Enqueue(Thumb(archiveB, 7, done)));
        IC_REQUIRE(WaitUntil(scheduler, "archive read started", [&] { return source.Reads(archiveB) == 1; }));
        IC_REQUIRE(scheduler.Enqueue(Thumb(archiveA, 8, done)));
        IC_CHECK(StaysUnread(source, archiveA));
        IC_CHECK(StaysUnread(source, localE));
        IC_CHECK(source.PeakOn(L"hdd1") == 2);

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "all delivered", [&] { return done.AllDone(1, 8); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.PeakOn(L"hdd1") <= DecodeScheduler::kMaxReadsPerSeekDevice);
    IC_CHECK(DecodeScheduler::kMaxReadsPerSeekDevice == 2);
}

IC_TEST(Lanes_MultipleVolumeNamesShareOneDevice)
{
    // D: and E: are volumes of one HDD: one local lane serves both, each
    // volume is queried once (cached), and another device is unaffected.
    Watchdog watchdog("volumes of one device");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring localD = LocalPath(L"D:", 0);
    source.Block(localD);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(localD, 1, done)));
        IC_REQUIRE(WaitUntil(scheduler, "local read started", [&] { return source.Reads(localD) == 1; }));
        for (uint64_t i = 0; i < 6; ++i)
        {
            const wchar_t* volume = (i % 2) ? L"D:" : L"E:";
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(volume, 10 + static_cast<int>(i)), 10 + i, done)));
        }
        IC_CHECK(scheduler.GetDiagnostics().lanes == 1);
        std::this_thread::sleep_for(kQuiet);
        IC_CHECK(source.TotalReads() == 1);  // only the blocked one

        IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 20), 20, done)));
        IC_REQUIRE(WaitUntil(scheduler, "other device", [&] { return done.Done(20); }));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "all delivered", [&] { return done.Done(1) && done.AllDone(10, 15); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(topology.Calls(L"D:") == 1);
    IC_CHECK(topology.Calls(L"E:") == 1);
    IC_CHECK(topology.Calls(L"F:") == 1);
}

IC_TEST(Lanes_UnknownClassificationIsConservative)
{
    // A source without its own classification (and a classification that
    // throws) is scheduled in the Local lane of its device, as before lanes:
    // never isolated from that device's local reads, never treated as remote.
    // Archive entries still get the Archive lane by the default rule.
    Watchdog watchdog("unknown classification");
    FakeTopology topology;
    LaneSource source(Files().dir, topology, false);
    Completions done;
    const std::wstring blocked = UnknownPath(L"D:", 0);
    const std::wstring sameDevice = UnknownPath(L"E:", 1);
    const std::wstring throwing = ThrowingPath(L"D:", 2);
    source.Block(blocked);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(blocked, 1, done)));
        IC_REQUIRE(WaitUntil(scheduler, "read started", [&] { return source.Reads(blocked) == 1; }));
        IC_REQUIRE(scheduler.Enqueue(Thumb(sameDevice, 2, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(throwing, 3, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 4), 4, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(EntryPath(L"D:", L"a.zip", 5), 5, done)));
        IC_REQUIRE(WaitUntil(scheduler, "other device and archive lane", [&] { return done.Done(4) && done.Done(5); }));
        IC_CHECK(StaysUnread(source, sameDevice));
        IC_CHECK(source.Reads(throwing) == 0);

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "all delivered", [&] { return done.AllDone(1, 5); }));
        for (uint64_t handle = 1; handle <= 5; ++handle)
        {
            IC_CHECK(done.Succeeded(handle));
        }
        IC_REQUIRE(Idle(scheduler));
    }
}

IC_TEST(Lanes_NewlyActiveSourceIsServedAtTheReaderCap)
{
    // All local readers are busy with blocked reads of two devices that both
    // have a backlog (all lanes busy: a third device waits). When one read
    // returns, the freed reader serves the newly active device before the
    // older backlog of the lane it just served (fairness by least recent
    // service, at equal priority).
    Watchdog watchdog("fairness at the reader cap");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring d0 = LocalPath(L"D:", 0);
    const std::wstring f1 = LocalPath(L"F:", 1);
    const std::wstring h8 = LocalPath(L"H:", 8);
    source.Block(d0);
    source.Block(f1);
    std::vector<std::wstring> order;
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };
        static_assert(DecodeScheduler::kReadersPerLane == 2, "the test blocks every local reader");

        IC_REQUIRE(scheduler.Enqueue(Thumb(d0, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(f1, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "both readers busy", [&]
        {
            return source.Reads(d0) == 1 && source.Reads(f1) == 1;
        }));
        for (int i = 0; i < 3; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"D:", 2 + i), 3 + i, done)));
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 5 + i), 6 + i, done)));
        }
        IC_REQUIRE(scheduler.Enqueue(Thumb(h8, 9, done)));
        IC_CHECK(StaysUnread(source, h8));  // every local reader is blocked

        source.Unblock(d0);
        IC_REQUIRE(WaitUntil(scheduler, "newly active device", [&] { return done.Done(9); }));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "all delivered", [&] { return done.AllDone(1, 9); }));
        IC_REQUIRE(Idle(scheduler));
        order = source.Order();
    }
    const auto indexOf = [&](const std::wstring& path)
    {
        return static_cast<size_t>(std::find(order.begin(), order.end(), path) - order.begin());
    };
    IC_CHECK(indexOf(h8) < indexOf(LocalPath(L"D:", 2)));
    IC_CHECK(indexOf(h8) == 2);  // the first read after the two blocked ones
}

IC_TEST(Lanes_AllArchiveLanesBusyQueueCancelAndReclaim)
{
    // Both archive readers are blocked on two HDDs: a third archive waits for
    // a reader while local thumbnails complete. Canceling the waiting request
    // removes it (and its lane) at once; canceling a blocked read frees its
    // reader for the next archive.
    Watchdog watchdog("all archive lanes busy");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring a = EntryPath(L"D:", L"a.zip", 0);
    const std::wstring b = EntryPath(L"F:", L"b.zip", 1);
    const std::wstring c = EntryPath(L"H:", L"c.zip", 2);
    const std::wstring c2 = EntryPath(L"H:", L"c.zip", 3);
    source.Block(a);
    source.Block(b);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(a, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(b, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "archive readers busy", [&] { return source.Reads(a) == 1 && source.Reads(b) == 1; }));
        IC_REQUIRE(scheduler.Enqueue(Thumb(c, 3, done)));
        for (uint64_t i = 0; i < 4; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"D:", 10 + static_cast<int>(i)), 10 + i, done)));
        }
        IC_REQUIRE(WaitUntil(scheduler, "local thumbnails", [&] { return done.AllDone(10, 13); }));
        IC_CHECK(StaysUnread(source, c));

        // The three archive lanes remain (the local lane is reclaimed once its
        // last thumbnail has released its slot).
        IC_REQUIRE(WaitUntil(scheduler, "three archive lanes", [&] { return scheduler.GetDiagnostics().lanes == 3; }));
        IC_CHECK(scheduler.LiveRequestCount() == 3);
        scheduler.Cancel(3);
        IC_CHECK(scheduler.LiveRequestCount() == 2);
        IC_CHECK(scheduler.GetDiagnostics().lanes == 2);  // c.zip's lane reclaimed at once

        IC_REQUIRE(scheduler.Enqueue(Thumb(c2, 4, done)));
        IC_CHECK(StaysUnread(source, c2));
        scheduler.Cancel(1);  // the blocked read of a.zip: its reader moves on
        IC_REQUIRE(WaitUntil(scheduler, "next archive", [&] { return done.Done(4); }));
        IC_CHECK(done.Succeeded(4));
        IC_CHECK(source.Returned(a) == 1);

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "remaining archive", [&] { return done.Done(2); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.Reads(c) == 0);
    IC_CHECK(!done.Done(1) && !done.Done(3));
    IC_CHECK(source.StoppedReturns() == 1);
}

IC_TEST(Lanes_MainImagesBypassThumbnailReadsAndAdmission)
{
    // A main image of an archive is read while a thumbnail read of the same
    // archive is blocked and every archive reader is busy; a remote main image
    // too. Main images of one archive are read one at a time: one queued
    // behind a blocked main-image read is removed by Cancel without being read.
    Watchdog watchdog("main images vs admission");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring thumbA = EntryPath(L"D:", L"a.zip", 0);
    const std::wstring thumbB = EntryPath(L"F:", L"b.zip", 1);
    const std::wstring mainBlocked = EntryPath(L"D:", L"a.zip", 7);
    const std::wstring mainQueued = EntryPath(L"D:", L"a.zip", 8);
    source.Block(thumbA);
    source.Block(thumbB);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Thumb(thumbA, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(thumbB, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "archive readers busy", [&]
        {
            return source.Reads(thumbA) == 1 && source.Reads(thumbB) == 1;
        }));
        IC_REQUIRE(scheduler.Enqueue(Full(EntryPath(L"D:", L"a.zip", 5), 3, done)));
        IC_REQUIRE(scheduler.Enqueue(Full(UncPath(L"server1", 6), 4, done)));
        IC_REQUIRE(WaitUntil(scheduler, "main images", [&] { return done.AllDone(3, 4); }));
        IC_CHECK(done.Succeeded(3) && done.Succeeded(4));

        source.Block(mainBlocked);
        IC_REQUIRE(scheduler.Enqueue(Full(mainBlocked, 5, done)));
        IC_REQUIRE(WaitUntil(scheduler, "main read started", [&] { return source.Reads(mainBlocked) == 1; }));
        IC_REQUIRE(scheduler.Enqueue(Full(mainQueued, 6, done)));
        IC_CHECK(StaysUnread(source, mainQueued));
        const size_t live = scheduler.LiveRequestCount();
        scheduler.Cancel(6);
        IC_CHECK(scheduler.LiveRequestCount() == live - 1);
        scheduler.Cancel(5);
        IC_REQUIRE(WaitUntil(scheduler, "canceled main read returned", [&] { return source.Returned(mainBlocked) == 1; }));

        // A local main image is decoded meanwhile.
        IC_REQUIRE(scheduler.Enqueue(Full(Files().fullRes, 7, done)));
        IC_REQUIRE(WaitUntil(scheduler, "local main image", [&] { return done.Done(7); }));
        IC_CHECK(done.Succeeded(7));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "thumbnails", [&] { return done.AllDone(1, 2); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.Reads(mainQueued) == 0);
    IC_CHECK(!done.Done(5) && !done.Done(6));
}

IC_TEST(Lanes_IdleLanesAreReclaimed)
{
    // Lanes exist while they have work and are dropped once idle.
    Watchdog watchdog("lane reclamation");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring blocked = EntryPath(L"G:", L"x.zip", 0);
    source.Block(blocked);
    {
        LaneScheduler scheduler(topology, 2);
        OnExit release { [&] { source.UnblockAll(); } };
        IC_CHECK(scheduler.GetDiagnostics().lanes == 0);

        IC_REQUIRE(scheduler.Enqueue(Thumb(blocked, 1, done)));
        const std::vector<std::wstring> paths {
            LocalPath(L"D:", 1), LocalPath(L"F:", 2), EntryPath(L"D:", L"a.zip", 3), UncPath(L"server1", 4),
            UncPath(L"server2", 5), CloudPath(L"C:", 6) };
        uint64_t handle = 10;
        for (const auto& path : paths)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(path, handle++, done)));
        }
        IC_REQUIRE(scheduler.Enqueue(Full(EntryPath(L"F:", L"m.zip", 7), handle++, done)));
        IC_REQUIRE(scheduler.Enqueue(Full(UncPath(L"server3", 8), handle++, done)));
        IC_REQUIRE(WaitUntil(scheduler, "everything but the blocked read", [&] { return done.AllDone(10, handle - 1); }));
        IC_REQUIRE(WaitUntil(scheduler, "idle lanes reclaimed", [&] { return scheduler.GetDiagnostics().lanes == 1; }));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "blocked read", [&] { return done.Done(1); }));
        IC_REQUIRE(Idle(scheduler));
        IC_CHECK(scheduler.GetDiagnostics().lanes == 0);
    }
}

IC_TEST(Lanes_GlobalPrefetchBoundHoldsAcrossLanes)
{
    // With the only decode worker held, thumbnails of many lanes are read until
    // the global count bound stops them: tasks holding bytes plus reads in
    // progress never exceed the cap (at most kMaxPrefetchCap), nor does
    // FileByteCache; concurrent reads never exceed the readers, nor the device
    // bounds. Every request is then read exactly once and decoded.
    Watchdog watchdog("global prefetch bound");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    Gate gate;
    size_t requests = 0;
    {
        LaneScheduler scheduler(topology, 1);
        OnExit release { [&] { gate.Release(); source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(Files().fullRes, ImagePurpose::FullResolution, 1, gate.Callback(done, 1),
            RequestPriority::MainImage)));
        IC_REQUIRE(WaitUntil(scheduler, "worker held", [&] { return gate.Entered(); }));

        const std::vector<std::function<std::wstring(int)>> lanes {
            [](int i) { return LocalPath(L"D:", i); },
            [](int i) { return LocalPath(L"F:", i); },
            [](int i) { return LocalPath(L"G:", i); },
            [](int i) { return EntryPath(L"G:", L"a.zip", i); },
            [](int i) { return EntryPath(L"K:", L"b.zip", i); },
            [](int i) { return EntryPath(L"H:", L"c.zip", i); },
            [](int i) { return UncPath(L"server1", i); },
            [](int i) { return UncPath(L"server2", i); },
        };
        uint64_t handle = 100;
        for (int i = 0; i < 6; ++i)
        {
            for (const auto& lane : lanes)
            {
                IC_REQUIRE(scheduler.Enqueue(Thumb(lane(i), handle++, done)));
            }
        }
        requests = static_cast<size_t>(handle - 100);

        // Fill: wait until the read count stops changing.
        size_t last = 0;
        auto lastChange = std::chrono::steady_clock::now();
        IC_REQUIRE(WaitUntil(scheduler, "reads settle", [&]
        {
            const size_t count = source.TotalReads();
            const auto now = std::chrono::steady_clock::now();
            if (count != last)
            {
                last = count;
                lastChange = now;
            }
            return count > 0 && now - lastChange >= std::chrono::milliseconds(500);
        }));
        const auto d = scheduler.GetDiagnostics();
        std::printf("  bound: %zu of %zu thumbnails read with the worker held; cap %zu, held %zu, reads %zu, cache %zu, peak reads %zu\n",
            source.TotalReads(), requests, d.prefetchCap, d.prefetchedTasks, d.prefetchReads,
            FileByteCache::Instance().Count(), source.Peak());
        IC_CHECK(d.prefetchCap <= DecodeScheduler::kMaxPrefetchCap);
        IC_CHECK(d.prefetchCap == DecodeScheduler::kMaxPrefetchCap);  // sum of depths exceeds it here
        IC_CHECK(d.prefetchedTasks + d.prefetchReads <= d.prefetchCap);
        IC_CHECK(source.TotalReads() == d.prefetchCap);
        IC_CHECK(FileByteCache::Instance().Count() <= d.prefetchCap);
        IC_CHECK(source.TotalReads() < requests);

        gate.Release();
        IC_REQUIRE(WaitUntil(scheduler, "all decoded", [&] { return done.Count() == requests + 1; }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(source.TotalReads() == requests);
    IC_CHECK(source.Peak() <= 3 * DecodeScheduler::kReadersPerLane);
    IC_CHECK(source.PeakOn(L"hdd2") <= DecodeScheduler::kMaxReadsPerSeekDevice);
    IC_CHECK(source.PeakOn(L"ssd1") <= DecodeScheduler::kMaxReadsPerSolidStateDevice);
    IC_CHECK(FileByteCache::Instance().Count() == 0);
}

IC_TEST(Lanes_PriorityAcrossLanesTakesTheLastSlot)
{
    // #11 across lanes: one HDD (global cap 2). The only worker is held with
    // both slots used by prefetch-priority local thumbnails. When one slot
    // frees, a visible archive thumbnail queued later takes it before the
    // older local backlog, although the local lane is admissible too.
    Watchdog watchdog("priority across lanes");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    Gate gate;
    const std::wstring visible = EntryPath(L"X:", L"a.zip", 9);
    std::vector<std::wstring> order;
    {
        LaneScheduler scheduler(topology, 1);
        OnExit release { [&] { gate.Release(); source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(MakeTask(LocalPath(L"X:", 0), ImagePurpose::Thumbnail, 1, gate.Callback(done, 1),
            RequestPriority::Prefetch)));
        for (uint64_t i = 1; i < 6; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"X:", static_cast<int>(i)), 1 + i, done, RequestPriority::Prefetch)));
        }
        IC_REQUIRE(WaitUntil(scheduler, "worker held", [&] { return gate.Entered(); }));
        IC_REQUIRE(WaitUntil(scheduler, "slots full", [&] { return scheduler.PrefetchedTaskCount() == 2; }));
        IC_CHECK(scheduler.GetDiagnostics().prefetchCap == 2);
        IC_REQUIRE(scheduler.Enqueue(Thumb(visible, 9, done, RequestPriority::Visible)));
        IC_CHECK(StaysUnread(source, visible));
        IC_CHECK(source.TotalReads() == 2);

        gate.Release();
        IC_REQUIRE(WaitUntil(scheduler, "all delivered", [&] { return done.AllDone(1, 6) && done.Done(9); }));
        IC_REQUIRE(Idle(scheduler));
        order = source.Order();
    }
    IC_REQUIRE(order.size() == 7);
    IC_CHECK(order[2] == visible);
}

IC_TEST(Lanes_SingleWorkerDecodesWhileArchiveMainImageIsBlocked)
{
    // SetThreadCount(1): the only decode worker never waits for a main-image
    // read. With an archive main image blocked in its read, local thumbnails
    // and a local main image complete.
    Watchdog watchdog("one worker, blocked archive main image");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring archiveMain = EntryPath(L"D:", L"a.zip", 0);
    source.Block(archiveMain);
    long long ms = 0;
    {
        LaneScheduler scheduler(topology, 1);
        IC_REQUIRE(scheduler.WorkerCount() == 1);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Full(archiveMain, 1, done)));
        IC_REQUIRE(WaitUntil(scheduler, "main read started", [&] { return source.Reads(archiveMain) == 1; }));
        const auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < 10; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 10 + static_cast<int>(i)), 10 + i, done)));
        }
        IC_REQUIRE(scheduler.Enqueue(Full(Files().fullRes, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "local work", [&] { return done.AllDone(10, 19) && done.Done(2); }));
        ms = MsSince(start);
        IC_CHECK(done.Succeeded(2));
        IC_CHECK(source.Returned(archiveMain) == 0);

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "archive main image", [&] { return done.Done(1); }));
        IC_CHECK(done.Succeeded(1));
        IC_REQUIRE(Idle(scheduler));
    }
    std::printf("  one worker: 10 local thumbnails + local main image in %lld ms with an archive main image blocked\n", ms);
}

IC_TEST(Lanes_TwoWorkersDecodeWhileTwoArchiveMainImagesAreBlocked)
{
    // SetThreadCount(2) with two concurrent full-resolution archive requests
    // blocked in their reads (both main-image readers): local thumbnails and a
    // local main image complete. A third archive main image waits for the
    // main-image cap and is read once one blocked request is canceled.
    Watchdog watchdog("two workers, two blocked archive main images");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;
    const std::wstring mainA = EntryPath(L"D:", L"a.zip", 0);
    const std::wstring mainB = EntryPath(L"F:", L"b.zip", 1);
    const std::wstring mainC = EntryPath(L"H:", L"c.zip", 2);
    source.Block(mainA);
    source.Block(mainB);
    long long ms = 0;
    {
        LaneScheduler scheduler(topology, 2);
        IC_REQUIRE(scheduler.WorkerCount() == 2);
        OnExit release { [&] { source.UnblockAll(); } };

        IC_REQUIRE(scheduler.Enqueue(Full(mainA, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Full(mainB, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "both main reads started", [&]
        {
            return source.Reads(mainA) == 1 && source.Reads(mainB) == 1;
        }));
        IC_REQUIRE(scheduler.Enqueue(Full(mainC, 3, done)));

        const auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < 10; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 10 + static_cast<int>(i)), 10 + i, done)));
        }
        IC_REQUIRE(scheduler.Enqueue(Full(Files().fullRes, 4, done)));
        IC_REQUIRE(WaitUntil(scheduler, "local work", [&] { return done.AllDone(10, 19) && done.Done(4); }));
        ms = MsSince(start);
        IC_CHECK(done.Succeeded(4));
        IC_CHECK(source.Reads(mainC) == 0);
        IC_CHECK(scheduler.GetDiagnostics().mainInputs == DecodeScheduler::kMainImageInputCap);

        scheduler.Cancel(1);
        IC_REQUIRE(WaitUntil(scheduler, "third main image", [&] { return done.Done(3); }));
        IC_CHECK(done.Succeeded(3));

        source.UnblockAll();
        IC_REQUIRE(WaitUntil(scheduler, "remaining main image", [&] { return done.Done(2); }));
        IC_REQUIRE(Idle(scheduler));
    }
    IC_CHECK(!done.Done(1));
    std::printf("  two workers: 10 local thumbnails + local main image in %lld ms with two archive main images blocked\n", ms);
}

IC_TEST(Lanes_FailedSlowReadsNeverFallBackToThePathOnAWorker)
{
    // A Remote read that returns no bytes is delivered as a failure without
    // decoding: the decoders' path fallback would reopen the source on the
    // decode worker, outside the lanes. The path is a local named pipe whose
    // reads block until the pipe is closed, so a fallback would hold the only
    // worker; local thumbnails and a local main image must still complete.
    Watchdog watchdog("failed slow reads, one worker");
    FakeTopology topology;
    LaneSource source(Files().dir, topology);
    Completions done;

    const std::wstring pipe = L"\\\\.\\pipe\\ImageCoreLaneFallback_" + std::to_wstring(GetCurrentProcessId()) + L".bmp";
    // Two listening instances: one per possible fallback open.
    HANDLE instances[2] {};
    for (HANDLE& instance : instances)
    {
        instance = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
            2, 4096, 4096, 0, nullptr);
        IC_REQUIRE(instance != INVALID_HANDLE_VALUE);
    }
    // Without the fix a decoder blocks on the pipe: closing it releases the
    // worker before the scheduler joins it (the check below then fails).
    const auto closePipes = [&]
    {
        for (HANDLE& instance : instances)
        {
            if (instance)
            {
                DisconnectNamedPipe(instance);
                CloseHandle(instance);
                instance = nullptr;
            }
        }
    };
    OnExit closeAtEnd { closePipes };

    long long ms = 0;
    {
        LaneScheduler scheduler(topology, 1);
        IC_REQUIRE(scheduler.WorkerCount() == 1);
        OnExit releaseWorker { closePipes };

        // LaneSource has no file of that name: both reads return no bytes.
        IC_REQUIRE(scheduler.Enqueue(Full(pipe, 1, done)));
        IC_REQUIRE(scheduler.Enqueue(Thumb(pipe, 2, done)));
        IC_REQUIRE(WaitUntil(scheduler, "failed slow reads delivered", [&] { return done.AllDone(1, 2); }));
        IC_CHECK(!done.Succeeded(1));
        IC_CHECK(!done.Succeeded(2));
        IC_CHECK(source.Reads(pipe) == 2);

        const auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < 10; ++i)
        {
            IC_REQUIRE(scheduler.Enqueue(Thumb(LocalPath(L"F:", 30 + static_cast<int>(i)), 10 + i, done)));
        }
        IC_REQUIRE(scheduler.Enqueue(Full(Files().fullRes, 3, done)));
        IC_REQUIRE(WaitUntil(scheduler, "local work", [&] { return done.AllDone(10, 19) && done.Done(3); }));
        ms = MsSince(start);
        IC_CHECK(done.Succeeded(3));
        IC_REQUIRE(Idle(scheduler));

        // No decoder opened the pipe: neither instance has a client.
        for (HANDLE instance : instances)
        {
            DWORD mode = PIPE_NOWAIT;
            IC_REQUIRE(SetNamedPipeHandleState(instance, &mode, nullptr, nullptr));
            IC_CHECK(!ConnectNamedPipe(instance, nullptr) && GetLastError() == ERROR_PIPE_LISTENING);
        }
    }

    std::printf("  failed slow reads delivered without a path fallback; local work in %lld ms\n", ms);
}
