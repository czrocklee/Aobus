// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ao::rt
{
  class Library;
}

namespace ao::gtk::platform
{
  // Exports library cover-art resources as file:// URLs for MPRIS clients.
  // Public methods are expected to run on the GTK main thread.
  class MprisArtUrlCache final
  {
  public:
    explicit MprisArtUrlCache(rt::Library const& library);
    MprisArtUrlCache(rt::Library const& library, std::filesystem::path cacheDir);
    ~MprisArtUrlCache();

    MprisArtUrlCache(MprisArtUrlCache const&) = delete;
    MprisArtUrlCache& operator=(MprisArtUrlCache const&) = delete;
    MprisArtUrlCache(MprisArtUrlCache&&) = delete;
    MprisArtUrlCache& operator=(MprisArtUrlCache&&) = delete;

    std::string urlForResource(ResourceId resourceId);

    static std::filesystem::path defaultCacheDirectory();
    static std::string_view extensionForBytes(std::span<std::byte const> bytes) noexcept;

  private:
    struct CacheEntry final
    {
      std::filesystem::path path;
      std::string url;
      std::uintmax_t byteSize = 0;
    };

    std::string cachedUrl(ResourceId resourceId) const;
    std::string exportResource(ResourceId resourceId);
    static bool cacheEntryValid(CacheEntry const& entry) noexcept;
    static void removeStaleResourceFiles(std::filesystem::path const& cacheDir,
                                         ResourceId resourceId,
                                         std::filesystem::path const& keepPath);
    static std::string fileUriForPath(std::filesystem::path const& path);

    rt::Library const& _library;
    std::filesystem::path _cacheDir;
    std::unordered_map<ResourceId, CacheEntry> _cache;
  };
} // namespace ao::gtk::platform
