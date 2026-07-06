// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

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
    std::string exportResource(ResourceId resourceId);
    static std::string fileUriForPath(std::filesystem::path const& path);

    rt::Library const& _library;
    std::filesystem::path _cacheDir;
  };
} // namespace ao::gtk::platform
