// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "MprisArtUrlCache.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/utility/ByteView.h>

#include <giomm/file.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::gtk::platform
{
  namespace
  {
    constexpr auto kKnownExtensions = std::array<std::string_view, 5>{".png", ".jpg", ".gif", ".webp", ".img"};
  } // namespace

  MprisArtUrlCache::MprisArtUrlCache(rt::LibraryTaskService& tasks, async::Runtime& runtime)
    : MprisArtUrlCache{tasks, runtime, defaultCacheDirectory()}
  {
  }

  MprisArtUrlCache::MprisArtUrlCache(rt::LibraryTaskService& tasks,
                                     async::Runtime& runtime,
                                     std::filesystem::path cacheDir)
    : _tasks{tasks}
    , _runtime{runtime}
    , _cacheDir{std::move(cacheDir)}
    , _scopePtr{std::make_unique<async::LifetimeScope>()}
  {
  }

  MprisArtUrlCache::~MprisArtUrlCache()
  {
    _scopePtr->cancelAll();
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

    auto statePtr = std::shared_ptr<RequestState>{};
    auto request = Request{};

    if (onReady)
    {
      statePtr = std::make_shared<RequestState>();
      request = Request{[statePtr] { statePtr->active.store(false, std::memory_order_relaxed); }};
    }

    if (auto const it = _inFlight.find(resourceId); it != _inFlight.end())
    {
      if (onReady)
      {
        it->second.push_back({.statePtr = std::move(statePtr), .onReady = std::move(onReady)});
      }

      return request;
    }

    if (auto& waiters = _inFlight[resourceId]; onReady)
    {
      waiters.push_back({.statePtr = std::move(statePtr), .onReady = std::move(onReady)});
    }

    auto optCachedEntry = std::optional<CacheEntry>{};

    if (auto const it = _cache.find(resourceId); it != _cache.end())
    {
      optCachedEntry = it->second;
    }

    spawnMaterialization(resourceId, std::move(optCachedEntry));
    return request;
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

  void MprisArtUrlCache::spawnMaterialization(ResourceId const resourceId, std::optional<CacheEntry> optCachedEntry)
  {
    _runtime.spawnWithLifetime(
      _scopePtr.get(),
      [cache = this,
       tasks = &_tasks,
       runtime = &_runtime,
       cacheDir = _cacheDir,
       resourceId,
       optCachedEntry = std::move(optCachedEntry)](std::stop_token const stopToken) mutable
      {
        return materialize(
          cache, tasks, runtime, std::move(cacheDir), resourceId, std::move(optCachedEntry), stopToken);
      });
  }

  async::Task<void> MprisArtUrlCache::materialize(MprisArtUrlCache* const cache,
                                                  rt::LibraryTaskService* const tasks,
                                                  async::Runtime* const runtime,
                                                  std::filesystem::path cacheDir,
                                                  ResourceId const resourceId,
                                                  std::optional<CacheEntry> optCachedEntry,
                                                  std::stop_token const stopToken)
  {
    auto optResult = std::optional<CacheEntry>{};

    try
    {
      if (optCachedEntry)
      {
        co_await runtime->resumeOnWorker(stopToken);

        if (isCacheEntryValid(*optCachedEntry))
        {
          optResult = std::move(optCachedEntry);
        }
      }

      if (!optResult)
      {
        auto bytesResult = co_await tasks->loadResourceAsync(resourceId, stopToken);

        if (!bytesResult)
        {
          if (bytesResult.error().code == Error::Code::ValueTooLarge)
          {
            APP_LOG_WARN("MPRIS cover resource {} exceeds the interactive byte limit", resourceId.raw());
          }
        }
        else if (*bytesResult && !(**bytesResult).empty())
        {
          auto bytes = std::move(**bytesResult);
          co_await runtime->resumeOnWorker(stopToken);
          optResult = exportResource(cacheDir, resourceId, bytes);
        }
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      runtime->reportUnhandledException(std::current_exception(), "MPRIS cover-art materialization workflow");
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (optResult)
    {
      cache->_cache[resourceId] = *optResult;
    }
    else
    {
      cache->_cache.erase(resourceId);
    }

    auto waiters = std::vector<RequestWaiter>{};

    if (auto const it = cache->_inFlight.find(resourceId); it != cache->_inFlight.end())
    {
      waiters = std::move(it->second);
      cache->_inFlight.erase(it);
    }

    auto const url = optResult ? optResult->url : std::string{};

    for (auto const& waiter : waiters)
    {
      if (waiter.statePtr->active.load(std::memory_order_relaxed))
      {
        waiter.onReady(url);
      }
    }
  }

  std::optional<MprisArtUrlCache::CacheEntry> MprisArtUrlCache::exportResource(std::filesystem::path const& cacheDir,
                                                                               ResourceId const resourceId,
                                                                               std::span<std::byte const> const bytes)
  {
    if (bytes.empty())
    {
      return std::nullopt;
    }

    std::filesystem::create_directories(cacheDir);
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
    auto const filePtr = Gio::File::create_for_path(path.string());
    return filePtr ? filePtr->get_uri() : std::string{};
  }
} // namespace ao::gtk::platform
