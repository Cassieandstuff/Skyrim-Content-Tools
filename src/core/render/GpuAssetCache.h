#pragma once
#include "renderer/ISceneRenderer.h"
#include <string>
#include <string_view>
#include <unordered_map>

// Lowercase-normalise a file path for use as a dedup key.
// Call once on the raw path before passing to GpuAssetCache::GetOrLoad.
inline std::string LowerPath(std::string_view path)
{
    std::string r(path);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c += 32;
    return r;
}

// ── GpuAssetCache ─────────────────────────────────────────────────────────────
// Deduplicating key → TextureHandle cache.
// GetOrLoad() invokes the load function only on a cache miss; the result
// (even TextureHandle::Invalid for a failed load) is stored and returned on
// subsequent calls with the same key.
//
// Caller is responsible for key normalisation (use LowerPath for file paths).
// Call Free() before destroying the renderer that owns the handles.
template<typename Key = std::string>
class GpuAssetCache {
public:
    // Return the cached handle for key, or call loadFn() to produce one.
    template<typename LoadFn>
    TextureHandle GetOrLoad(const Key& key, LoadFn&& loadFn)
    {
        auto it = map_.find(key);
        if (it != map_.end()) return it->second;
        TextureHandle h = std::forward<LoadFn>(loadFn)();
        map_.emplace(key, h);
        return h;
    }

    // Release all non-Invalid handles and clear the map.
    void Free(ISceneRenderer& renderer)
    {
        for (auto& [k, h] : map_)
            if (h != TextureHandle::Invalid) renderer.FreeTexture(h);
        map_.clear();
    }

    bool        Empty() const { return map_.empty(); }
    std::size_t Size()  const { return map_.size(); }

private:
    std::unordered_map<Key, TextureHandle> map_;
};
