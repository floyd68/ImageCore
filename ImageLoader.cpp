#include "ImageLoader.h"
#include "ImageCache.h"
#include "DecodeScheduler.h"
#include <algorithm>
#include <wincodec.h>
#include <comdef.h>

namespace ImageCore
{
    ImageLoader& ImageLoader::Instance()
    {
        static ImageLoader instance;
        return instance;
    }

    ImageLoader::ImageLoader()
        : m_nextHandle(1)
    {
        Initialize();
    }

    ImageLoader::~ImageLoader()
    {
        Shutdown();
    }

    void ImageLoader::Initialize()
    {
        // WIC Factory 생성 (ImageCore가 자체적으로 관리)
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_wicFactory));
        
        if (FAILED(hr))
        {
            // WIC Factory 생성 실패 시 nullptr로 유지
            m_wicFactory.Reset();
        }

        m_cache = std::make_unique<ImageCache>();
        m_scheduler = std::make_unique<DecodeScheduler>();
    }

    void ImageLoader::Shutdown()
    {
        if (m_scheduler)
        {
            m_scheduler->Shutdown();
        }
        m_scheduler.reset();
        m_cache.reset();
        m_wicFactory.Reset();
    }

    ImageHandle ImageLoader::Request(
        const ImageRequest& request,
        ImageLoadCallback callback)
    {
        if (request.source.empty() || !callback)
        {
            return 0;
        }

        // WIC Factory 확인
        if (m_wicFactory == nullptr)
        {
            callback(E_FAIL, nullptr, nullptr);
            return 0;
        }

        ImageHandle handle = m_nextHandle.fetch_add(1);

        // 1. 캐시 확인은 제거 (D2D1Bitmap 캐시는 FD2D 레이어에서 관리)

        // 2. 비동기 디코드 작업 큐에 추가
        DecodeTask task;
        task.request = request;
        task.handle = handle;
        task.wicFactory = m_wicFactory.Get();  // ImageCore가 관리하는 Factory 사용
        task.callback = [callback](PipelineResult&& result)
        {
            if (FAILED(result.hr))
            {
                callback(result.hr, nullptr, nullptr);
                return;
            }

            // 콜백은 worker thread에서 즉시 호출됨.
            // UI 레이어(FD2D)는 여기서 받은 WIC/Scratch를 저장하고 InvalidateRect 등으로 렌더를 유도하면 됨.
            callback(S_OK, std::move(result.wicBitmap), std::move(result.scratchImage));
        };

        m_scheduler->Enqueue(std::move(task));

        return handle;
    }

    void ImageLoader::Cancel(ImageHandle handle)
    {
        // 현재 구현에서는 큐에서 제거하기 어려움
        // 향후 개선 필요: handle을 task에 포함시키고 큐에서 찾아서 제거
        // 현재는 단순히 무시 (콜백이 호출되면 무시하도록 클라이언트에서 처리)
        UNREFERENCED_PARAMETER(handle);
    }

    void ImageLoader::ClearCache()
    {
        if (m_cache)
        {
            m_cache->Clear();
        }
    }

    void ImageLoader::SetCacheSizeLimit(size_t maxMemoryBytes)
    {
        if (m_cache)
        {
            m_cache->SetSizeLimit(maxMemoryBytes);
        }
    }

    void ImageLoader::SetWorkerThreadCount(size_t count)
    {
        if (m_scheduler)
        {
            m_scheduler->SetThreadCount(count);
        }
    }
}

