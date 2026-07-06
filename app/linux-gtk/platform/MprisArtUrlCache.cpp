// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MprisArtUrlCache.h"

#include <ao/CoreIds.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/utility/ByteView.h>

#include <giomm/file.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::gtk::platform
{
  namespace
  {
    constexpr auto kKnownExtensions = std::array<std::string_view, 5>{".png", ".jpg", ".gif", ".webp", ".img"};
  } // namespace

  MprisArtUrlCache::MprisArtUrlCache(rt::Library const& library)
    : MprisArtUrlCache{library, defaultCacheDirectory()}
  {
  }

  MprisArtUrlCache::MprisArtUrlCache(rt::Library const& library, std::filesystem::path cacheDir)
    : _library{library}, _cacheDir{std::move(cacheDir)}
  {
  }

  MprisArtUrlCache::~MprisArtUrlCache() = default;

  std::string MprisArtUrlCache::urlForResource(ResourceId const resourceId)
  {
    if (resourceId == kInvalidResourceId)
    {
      return {};
    }

    try
    {
      if (auto cached = cachedUrl(resourceId); !cached.empty())
      {
        return cached;
      }

      return exportResource(resourceId);
    }
    catch (std::exception const& e)
    {
      APP_LOG_WARN("Failed to export MPRIS cover art resource {}: {}", resourceId.raw(), e.what());
    }
    catch (...)
    {
      APP_LOG_WARN("Failed to export MPRIS cover art resource {}: unknown exception", resourceId.raw());
    }

    return {};
  }

  std::filesystem::path MprisArtUrlCache::defaultCacheDirectory()
  {
    return std::filesystem::path{Glib::get_user_cache_dir()} / "aobus" / "mpris-art";
  }

  std::string_view MprisArtUrlCache::extensionForBytes(std::span<std::byte const> bytes) noexcept
  {
    constexpr auto kPng = std::array{std::byte{0x89},
                                     std::byte{0x50},
                                     std::byte{0x4E},
                                     std::byte{0x47},
                                     std::byte{0x0D},
                                     std::byte{0x0A},
                                     std::byte{0x1A},
                                     std::byte{0x0A}};
    constexpr auto kJpeg = std::array{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}};
    constexpr auto kGif87 =
      std::array{std::byte{0x47}, std::byte{0x49}, std::byte{0x46}, std::byte{0x38}, std::byte{0x37}, std::byte{0x61}};
    constexpr auto kGif89 =
      std::array{std::byte{0x47}, std::byte{0x49}, std::byte{0x46}, std::byte{0x38}, std::byte{0x39}, std::byte{0x61}};
    constexpr auto kRiff = std::array{std::byte{0x52}, std::byte{0x49}, std::byte{0x46}, std::byte{0x46}};
    constexpr auto kWebp = std::array{std::byte{0x57}, std::byte{0x45}, std::byte{0x42}, std::byte{0x50}};
    auto const hasPrefix = [](std::span<std::byte const> input, auto const& prefix)
    { return input.size() >= prefix.size() && std::ranges::equal(prefix, input.first(prefix.size())); };

    if (hasPrefix(bytes, kPng))
    {
      return ".png";
    }

    if (hasPrefix(bytes, kJpeg))
    {
      return ".jpg";
    }

    if (hasPrefix(bytes, kGif87) || hasPrefix(bytes, kGif89))
    {
      return ".gif";
    }

    constexpr std::size_t kWebpHeaderSize = 12;
    constexpr std::size_t kRiffPrefixSize = 8;

    if (bytes.size() >= kWebpHeaderSize && hasPrefix(bytes, kRiff) && hasPrefix(bytes.subspan(kRiffPrefixSize), kWebp))
    {
      return ".webp";
    }

    return ".img";
  }

  std::string MprisArtUrlCache::cachedUrl(ResourceId const resourceId) const
  {
    auto const it = _cache.find(resourceId);

    if (it == _cache.end() || !cacheEntryValid(it->second))
    {
      return {};
    }

    return it->second.url;
  }

  std::string MprisArtUrlCache::exportResource(ResourceId const resourceId)
  {
    auto const reader = _library.reader();
    auto const optBytes = reader.loadResource(resourceId);

    if (!optBytes || optBytes->empty())
    {
      return {};
    }

    std::filesystem::create_directories(_cacheDir);
    auto const path = _cacheDir / (std::to_string(resourceId.raw()) + std::string{extensionForBytes(*optBytes)});
    removeStaleResourceFiles(_cacheDir, resourceId, path);

    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};

    if (!output)
    {
      return {};
    }

    auto const bytes = utility::bytes::stringView(*optBytes);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();

    if (!output)
    {
      auto ec = std::error_code{};
      std::filesystem::remove(path, ec);
      return {};
    }

    output.close();

    if (!output)
    {
      auto ec = std::error_code{};
      std::filesystem::remove(path, ec);
      return {};
    }

    auto url = fileUriForPath(path);

    if (url.empty())
    {
      return {};
    }

    _cache[resourceId] = CacheEntry{.path = path, .url = url, .byteSize = optBytes->size()};
    return url;
  }

  bool MprisArtUrlCache::cacheEntryValid(CacheEntry const& entry) noexcept
  {
    auto ec = std::error_code{};

    if (!std::filesystem::is_regular_file(entry.path, ec) || ec)
    {
      return false;
    }

    auto const size = std::filesystem::file_size(entry.path, ec);
    return !ec && size == entry.byteSize;
  }

  void MprisArtUrlCache::removeStaleResourceFiles(std::filesystem::path const& cacheDir,
                                                  ResourceId const resourceId,
                                                  std::filesystem::path const& keepPath)
  {
    for (auto const extension : kKnownExtensions)
    {
      auto const candidate = cacheDir / (std::to_string(resourceId.raw()) + std::string{extension});

      if (candidate == keepPath)
      {
        continue;
      }

      auto ec = std::error_code{};
      std::filesystem::remove(candidate, ec);
    }
  }

  std::string MprisArtUrlCache::fileUriForPath(std::filesystem::path const& path)
  {
    auto const filePtr = Gio::File::create_for_path(path.string());
    return filePtr ? filePtr->get_uri() : std::string{};
  }
} // namespace ao::gtk::platform
