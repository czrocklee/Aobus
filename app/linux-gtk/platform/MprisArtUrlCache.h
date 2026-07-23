// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Task.h>
#include <ao/utility/ScopedRegistration.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ao::rt
{
  class LibraryTaskService;
}

namespace ao::async
{
  class LifetimeScope;
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

    MprisArtUrlCache(rt::LibraryTaskService& tasks, async::Runtime& runtime);
    MprisArtUrlCache(rt::LibraryTaskService& tasks, async::Runtime& runtime, std::filesystem::path cacheDir);
    ~MprisArtUrlCache();

    MprisArtUrlCache(MprisArtUrlCache const&) = delete;
    MprisArtUrlCache& operator=(MprisArtUrlCache const&) = delete;
    MprisArtUrlCache(MprisArtUrlCache&&) = delete;
    MprisArtUrlCache& operator=(MprisArtUrlCache&&) = delete;

    Request requestUrl(ResourceId resourceId, OnUrlReady onReady);

    static std::filesystem::path defaultCacheDirectory();
    static std::string_view extensionForBytes(std::span<std::byte const> bytes) noexcept;

  private:
    struct CacheEntry final
    {
      std::filesystem::path path;
      std::string url;
      std::uintmax_t byteSize = 0;
    };

    struct RequestState final
    {
      std::atomic_bool active{true};
    };

    struct RequestWaiter final
    {
      std::shared_ptr<RequestState> statePtr;
      OnUrlReady onReady;
    };

    void spawnMaterialization(ResourceId resourceId, std::optional<CacheEntry> optCachedEntry);
    static async::Task<void> materialize(MprisArtUrlCache* cache,
                                         rt::LibraryTaskService* tasks,
                                         async::Runtime* runtime,
                                         std::filesystem::path cacheDir,
                                         ResourceId resourceId,
                                         std::optional<CacheEntry> optCachedEntry,
                                         std::stop_token stopToken);
    static std::optional<CacheEntry> exportResource(std::filesystem::path const& cacheDir,
                                                    ResourceId resourceId,
                                                    std::span<std::byte const> bytes);
    static bool isCacheEntryValid(CacheEntry const& entry) noexcept;
    static void removeStaleResourceFiles(std::filesystem::path const& cacheDir,
                                         ResourceId resourceId,
                                         std::filesystem::path const& keepPath);
    static std::string fileUriForPath(std::filesystem::path const& path);

    rt::LibraryTaskService& _tasks;
    async::Runtime& _runtime;
    std::filesystem::path _cacheDir;
    std::unique_ptr<async::LifetimeScope> _scopePtr;
    std::unordered_map<ResourceId, CacheEntry> _cache;
    std::unordered_map<ResourceId, std::vector<RequestWaiter>> _inFlight;
  };
} // namespace ao::gtk::platform
