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
#include <utility>

namespace ao::gtk::platform
{
  namespace
  {
    bool startsWith(std::span<std::byte const> bytes, std::span<std::byte const> prefix) noexcept
    {
      if (bytes.size() < prefix.size())
      {
        return false;
      }

      return std::equal(prefix.begin(), prefix.end(), bytes.begin());
    }
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

    if (startsWith(bytes, kPng))
    {
      return ".png";
    }

    if (startsWith(bytes, kJpeg))
    {
      return ".jpg";
    }

    if (startsWith(bytes, kGif87) || startsWith(bytes, kGif89))
    {
      return ".gif";
    }

    constexpr std::size_t kWebpHeaderSize = 12;
    constexpr std::size_t kRiffPrefixSize = 8;

    if (bytes.size() >= kWebpHeaderSize && startsWith(bytes, kRiff) &&
        startsWith(bytes.subspan(kRiffPrefixSize), kWebp))
    {
      return ".webp";
    }

    return ".img";
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

    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};

    if (!output)
    {
      return {};
    }

    auto const bytes = utility::bytes::stringView(*optBytes);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    if (!output)
    {
      return {};
    }

    return fileUriForPath(path);
  }

  std::string MprisArtUrlCache::fileUriForPath(std::filesystem::path const& path)
  {
    auto const filePtr = Gio::File::create_for_path(path.string());
    return filePtr ? filePtr->get_uri() : std::string{};
  }
} // namespace ao::gtk::platform
