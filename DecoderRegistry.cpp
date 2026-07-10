#include "DecoderRegistry.h"
#include "IImageDecoderFactory.h"
#include "ImageFormatDetector.h"
#include <algorithm>
#include <filesystem>

namespace ImageCore
{
    DecoderRegistry& DecoderRegistry::Instance()
    {
        static DecoderRegistry instance;
        return instance;
    }

    std::wstring DecoderRegistry::NormalizeExtension(std::wstring_view ext)
    {
        std::wstring e(ext);
        if (!e.empty() && e[0] != L'.')
        {
            e.insert(e.begin(), L'.');
        }

        std::transform(e.begin(), e.end(), e.begin(), ::towlower);
        return e;
    }

    std::wstring DecoderRegistry::GetLowerExtensionFromPath(const std::wstring& path)
    {
        // path.extension() already includes the leading dot when present,
        // so the shared lowercase helper is sufficient here.
        return ImageFormatDetector::GetLowerExtension(path);
    }

    bool DecoderRegistry::RegisterFactory(const std::shared_ptr<IImageDecoderFactory>& factory)
    {
        if (!factory)
        {
            return false;
        }

        std::wstring id(factory->Id());
        if (id.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_factoriesById.find(id) != m_factoriesById.end())
        {
            return false;
        }

        // register id
        m_factoriesById.emplace(id, factory);

        // register extensions -> id
        for (auto extView : factory->SupportedExtensions())
        {
            std::wstring ext = NormalizeExtension(extView);
            if (ext.empty() || ext == L".")
            {
                continue;
            }

            auto& vec = m_factoriesByExtension[ext];
            vec.push_back(id);

            // priority sort (highest first)
            std::stable_sort(vec.begin(), vec.end(), [this](const std::wstring& a, const std::wstring& b)
            {
                const auto ia = m_factoriesById.find(a);
                const auto ib = m_factoriesById.find(b);
                const int pa = (ia != m_factoriesById.end() && ia->second) ? ia->second->Priority() : 0;
                const int pb = (ib != m_factoriesById.end() && ib->second) ? ib->second->Priority() : 0;
                return pa > pb;
            });
        }

        return true;
    }

    bool DecoderRegistry::UnregisterFactory(std::wstring_view idView)
    {
        std::wstring id(idView);
        if (id.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_factoriesById.find(id);
        if (it == m_factoriesById.end())
        {
            return false;
        }

        m_factoriesById.erase(it);

        // remove id from extension lists
        for (auto extIt = m_factoriesByExtension.begin(); extIt != m_factoriesByExtension.end(); )
        {
            auto& vec = extIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
            if (vec.empty())
            {
                extIt = m_factoriesByExtension.erase(extIt);
            }
            else
            {
                ++extIt;
            }
        }

        return true;
    }

    bool DecoderRegistry::IsSupportedPath(const std::wstring& path) const
    {
        if (path.empty())
        {
            return false;
        }

        return IsSupportedExtension(GetLowerExtensionFromPath(path));
    }

    bool DecoderRegistry::IsSupportedExtension(std::wstring_view ext) const
    {
        std::wstring e = NormalizeExtension(ext);
        if (e.empty() || e == L".")
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_factoriesByExtension.find(e);
        return it != m_factoriesByExtension.end() && !it->second.empty();
    }

    std::vector<std::wstring> DecoderRegistry::GetSupportedExtensions() const
    {
        std::vector<std::wstring> exts {};
        std::lock_guard<std::mutex> lock(m_mutex);
        exts.reserve(m_factoriesByExtension.size());

        for (const auto& kv : m_factoriesByExtension)
        {
            if (!kv.second.empty())
            {
                exts.push_back(kv.first);
            }
        }

        std::sort(exts.begin(), exts.end());
        return exts;
    }

    std::vector<std::shared_ptr<IImageDecoderFactory>> DecoderRegistry::GetCandidateFactories(const ImageRequest& request) const
    {
        return GetCandidateFactories(request, {});
    }

    std::vector<std::shared_ptr<IImageDecoderFactory>> DecoderRegistry::GetCandidateFactories(const ImageRequest& request, std::span<const uint8_t> header) const
    {
        std::vector<std::shared_ptr<IImageDecoderFactory>> result;

        const std::wstring ext = GetLowerExtensionFromPath(request.source);

        std::lock_guard<std::mutex> lock(m_mutex);

        auto AddFactoryById = [&](const std::wstring& id)
        {
            auto it = m_factoriesById.find(id);
            if (it == m_factoriesById.end() || !it->second)
            {
                return;
            }

            // avoid duplicates
            for (auto& existing : result)
            {
                if (existing && existing->Id() == id)
                {
                    return;
                }
            }

            if (it->second->SupportsPurpose(request.purpose))
            {
                // Allow it to be excluded in header/magic-based probing.
                if (it->second->Probe(header, ext))
                {
                    result.push_back(it->second);
                }
            }
        };

        // 1) extension match candidates first (priority order already)
        auto extIt = m_factoriesByExtension.find(ext);
        if (extIt != m_factoriesByExtension.end())
        {
            for (const auto& id : extIt->second)
            {
                AddFactoryById(id);
            }
        }

        // 2) fallback: any other factory that supports purpose (priority order by factory->Priority)
        std::vector<std::shared_ptr<IImageDecoderFactory>> all;
        all.reserve(m_factoriesById.size());
        for (const auto& kv : m_factoriesById)
        {
            if (kv.second && kv.second->SupportsPurpose(request.purpose))
            {
                all.push_back(kv.second);
            }
        }
        std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b)
        {
            return a->Priority() > b->Priority();
        });

        for (const auto& f : all)
        {
            AddFactoryById(std::wstring(f->Id()));
        }

        return result;
    }
}


