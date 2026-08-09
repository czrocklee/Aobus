// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MprisArtUrlCache.h"

#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/ByteView.h>

#include <giomm/file.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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

  MprisArtUrlCache::MprisArtUrlCache(rt::ResourceByteLoader& byteLoader, async::Runtime& runtime)
    : MprisArtUrlCache{byteLoader, runtime, defaultCacheDirectory()}
  {
  }

  MprisArtUrlCache::MprisArtUrlCache(rt::ResourceByteLoader& byteLoader,
                                     async::Runtime& runtime,
                                     std::filesystem::path cacheDir)
    : _byteLoader{byteLoader}
    , _runtime{runtime}
    , _cacheDir{std::move(cacheDir)}
    , _scopePtr{std::make_unique<async::LifetimeScope>()}
  {
  }

  MprisArtUrlCache::~MprisArtUrlCache()
  {
    _scopePtr->cancelAll();
    _requests.clear();
  }

  MprisArtUrlCache::Request MprisArtUrlCache::requestUrl(ResourceId const resourceId, OnUrlReady onReady)
  {
    if (resourceId == kInvalidResourceId)
    {
      if (onReady)
      {
        onReady({});
      }

      return {};
    }

    auto optCachedEntry = std::optional<CacheEntry>{};

    if (auto const it = _cache.find(resourceId); it != _cache.end())
    {
      optCachedEntry = it->second;
    }

    auto callback = decltype(_requests)::Callback{};

    if (onReady)
    {
      callback = [onReady = std::move(onReady)](std::string const& url) { onReady(url); };
    }

    return _requests.request(
      resourceId,
      std::move(callback),
      [this, resourceId, optCachedEntry = std::move(optCachedEntry)](Requests::FlightToken token) mutable
      { spawnMaterialization(resourceId, std::move(optCachedEntry), std::move(token)); });
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

  void MprisArtUrlCache::spawnMaterialization(ResourceId const resourceId,
                                              std::optional<CacheEntry> optCachedEntry,
                                              Requests::FlightToken token)
  {
    _runtime.spawnWithLifetime(
      _scopePtr.get(),
      [cache = this,
       runtime = &_runtime,
       resourceId,
       optCachedEntry = std::move(optCachedEntry),
       token = std::move(token)](std::stop_token const stopToken) mutable
      { return validate(cache, runtime, resourceId, std::move(optCachedEntry), std::move(token), stopToken); },
      "MPRIS cover-art cache validation");
  }

  async::Task<void> MprisArtUrlCache::validate(MprisArtUrlCache* const cache,
                                               async::Runtime* const runtime,
                                               ResourceId const resourceId,
                                               std::optional<CacheEntry> optCachedEntry,
                                               Requests::FlightToken token,
                                               std::stop_token const stopToken)
  {
    if (optCachedEntry)
    {
      co_await runtime->resumeOnWorker(stopToken);

      if (isCacheEntryValid(*optCachedEntry))
      {
        co_await runtime->resumeOnCallbackExecutor(stopToken);
        cache->_cache[resourceId] = *optCachedEntry;
        cache->_requests.complete(token, optCachedEntry->url);
        co_return;
      }
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);
    cache->requestBytes(resourceId, std::move(token));
  }

  void MprisArtUrlCache::requestBytes(ResourceId const resourceId, Requests::FlightToken token)
  {
    auto dependency = _byteLoader.request(resourceId,
                                          [this, resourceId, token](rt::ResourceBytes bytes) mutable
                                          { spawnExport(resourceId, std::move(token), std::move(bytes)); });
    _requests.retainDependency(token, std::move(dependency));
  }

  void MprisArtUrlCache::spawnExport(ResourceId const resourceId, Requests::FlightToken token, rt::ResourceBytes bytes)
  {
    _runtime.spawnWithLifetime(
      _scopePtr.get(),
      [cache = this,
       runtime = &_runtime,
       cacheDir = _cacheDir,
       resourceId,
       token = std::move(token),
       bytes = std::move(bytes)](std::stop_token const stopToken) mutable
      {
        return exportBytes(
          cache, runtime, std::move(cacheDir), resourceId, std::move(token), std::move(bytes), stopToken);
      },
      "MPRIS cover-art export");
  }

  async::Task<void> MprisArtUrlCache::exportBytes(MprisArtUrlCache* const cache,
                                                  async::Runtime* const runtime,
                                                  std::filesystem::path cacheDir,
                                                  ResourceId const resourceId,
                                                  Requests::FlightToken token,
                                                  rt::ResourceBytes bytes,
                                                  std::stop_token const stopToken)
  {
    auto optResult = std::optional<CacheEntry>{};

    co_await runtime->resumeOnWorker(stopToken);

    if (!bytes.empty())
    {
      optResult = exportResource(cacheDir, resourceId, bytes.view());
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (optResult)
    {
      cache->_cache.insert_or_assign(resourceId, *optResult);
    }
    else
    {
      cache->_cache.erase(resourceId);
    }

    auto const url = optResult ? optResult->url : std::string{};
    cache->_requests.complete(token, url);
  }

  std::optional<MprisArtUrlCache::CacheEntry> MprisArtUrlCache::exportResource(std::filesystem::path const& cacheDir,
                                                                               ResourceId const resourceId,
                                                                               std::span<std::byte const> const bytes)
  {
    if (bytes.empty())
    {
      return std::nullopt;
    }

    auto ec = std::error_code{};
    std::filesystem::create_directories(cacheDir, ec);

    if (ec)
    {
      return std::nullopt;
    }

    auto const path = cacheDir / (std::to_string(resourceId.raw()) + std::string{extensionForBytes(bytes)});
    removeStaleResourceFiles(cacheDir, resourceId, path);

    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};

    if (!output)
    {
      return std::nullopt;
    }

    auto const byteView = utility::bytes::stringView(bytes);
    output.write(byteView.data(), static_cast<std::streamsize>(byteView.size()));
    output.flush();

    if (!output)
    {
      auto ec = std::error_code{};
      std::filesystem::remove(path, ec);
      return std::nullopt;
    }

    output.close();

    if (!output)
    {
      auto ec = std::error_code{};
      std::filesystem::remove(path, ec);
      return std::nullopt;
    }

    auto url = fileUriForPath(path);

    if (url.empty())
    {
      return std::nullopt;
    }

    return CacheEntry{.path = path, .url = std::move(url), .byteSize = bytes.size()};
  }

  bool MprisArtUrlCache::isCacheEntryValid(CacheEntry const& entry) noexcept
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
    try
    {
      auto const filePtr = Gio::File::create_for_path(path.string());
      return filePtr ? filePtr->get_uri() : std::string{};
    }
    catch (Glib::Error const&)
    {
      return {};
    }
  }
} // namespace ao::gtk::platform
