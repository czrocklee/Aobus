// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/RequestCoalescer.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/ScopedRegistration.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ao::rt
{
  class ResourceByteMemoryCache;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::gtk::platform
{
  // Exports library cover-art resources as file:// URLs for MPRIS clients.
  // Public methods are expected to run on the GTK main thread.
  class MprisArtUrlCache final
  {
  public:
    using OnUrlReady = std::function<void(std::string)>;
    using Request = utility::ScopedRegistration;

    MprisArtUrlCache(rt::ResourceByteMemoryCache& byteCache, async::Runtime& runtime);
    MprisArtUrlCache(rt::ResourceByteMemoryCache& byteCache, async::Runtime& runtime, std::filesystem::path cacheDir);
    ~MprisArtUrlCache();

    MprisArtUrlCache(MprisArtUrlCache const&) = delete;
    MprisArtUrlCache& operator=(MprisArtUrlCache const&) = delete;
    MprisArtUrlCache(MprisArtUrlCache&&) = delete;
    MprisArtUrlCache& operator=(MprisArtUrlCache&&) = delete;

    Request requestUrl(ResourceId resourceId, OnUrlReady onReady);

    static std::filesystem::path defaultCacheDirectory();
    static std::string_view extensionForBytes(std::span<std::byte const> bytes) noexcept;

  private:
    using Requests = async::RequestCoalescer<ResourceId, std::string>;

    struct CacheEntry final
    {
      std::filesystem::path path;
      std::string url;
      std::uintmax_t byteSize = 0;
    };

    void startEntryValidation(ResourceId resourceId, CacheEntry cachedEntry, Requests::FlightToken token);
    void requestBytes(ResourceId resourceId, Requests::FlightToken token);
    void spawnExport(ResourceId resourceId, Requests::FlightToken token, rt::ResourceBytes bytes);
    static async::Task<void> validateEntry(MprisArtUrlCache* cache,
                                           async::Runtime* runtime,
                                           ResourceId resourceId,
                                           CacheEntry cachedEntry,
                                           Requests::FlightToken token,
                                           std::stop_token stopToken);
    static async::Task<void> exportBytes(MprisArtUrlCache* cache,
                                         async::Runtime* runtime,
                                         std::filesystem::path cacheDir,
                                         ResourceId resourceId,
                                         Requests::FlightToken token,
                                         rt::ResourceBytes bytes,
                                         std::stop_token stopToken);
    static std::optional<CacheEntry> exportResource(std::filesystem::path const& cacheDir,
                                                    ResourceId resourceId,
                                                    std::span<std::byte const> bytes);
    static bool isCacheEntryValid(CacheEntry const& entry) noexcept;
    static void removeStaleResourceFiles(std::filesystem::path const& cacheDir,
                                         ResourceId resourceId,
                                         std::filesystem::path const& keepPath);
    static std::string fileUriForPath(std::filesystem::path const& path);

    rt::ResourceByteMemoryCache& _byteCache;
    async::Runtime& _runtime;
    std::filesystem::path _cacheDir;
    async::LifetimeScope _scope;
    std::unordered_map<ResourceId, CacheEntry> _cache;
    Requests _requests;
  };
} // namespace ao::gtk::platform
