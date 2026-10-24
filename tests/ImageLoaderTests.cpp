#include "TestHarness.h"

#include "ImageCore.h"
#include "ImageLoader.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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
