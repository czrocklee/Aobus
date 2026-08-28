// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ResourceMaterializer.h"

#include "ResourceCarrierIndex.h"
#include "ResourceMaterialization.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/compat/AtomicSharedPtr.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/rt/resource/ResourceDiskCache.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace ao::rt
{
  struct ResourceMaterializer::Impl final
  {
    Impl(async::Runtime& runtimeRef, library::MusicLibrary& libraryRef, std::filesystem::path const& cacheDirectory)
      : asyncRuntime{runtimeRef}
      , library{libraryRef}
      , diskCache{ResourceDiskCache::Config{
          .directory = coverCacheDirectory(cacheDirectory),
          .maximumEntryBytes = kMaximumInteractiveResourceBytes,
        }}
    {
    }

    /**
     * Rebuilds the carrier index when the published snapshot is behind.
     *
     * The transaction is opened under the rebuild mutex so a late worker cannot
     * publish an older revision after a newer one. A burst on one stale stamp
     * therefore pays for one build.
     */
    std::shared_ptr<ResourceCarrierIndex const> rebuildCarrierIndex(std::uint64_t const requestRevision)
    {
      auto const lock = std::scoped_lock{carrierIndexMutex};

      if (auto const currentPtr = carrierIndexSlot.load(); currentPtr && currentPtr->answersRevision(requestRevision))
      {
        return currentPtr;
      }

      auto const transaction = library.readTransaction();
      auto snapshotPtr = std::make_shared<ResourceCarrierIndex const>(buildResourceCarrierIndex(library, transaction));
      carrierIndexBuildCount.fetch_add(1);
      carrierIndexSlot.store(snapshotPtr);
      return snapshotPtr;
    }

    /** Resolves one descriptor and index snapshot before performing file I/O. */
    Result<std::optional<std::vector<std::byte>>> loadResource(ResourceId const resourceId,
                                                               std::optional<std::size_t> const optMaximumBytes,
                                                               std::stop_token const& stopToken)
    {
      auto optDescriptor = std::optional<library::ResourceDescriptor>{};
      std::uint64_t revision = 0;
      auto indexPtr = std::shared_ptr<ResourceCarrierIndex const>{};

      {
        auto const transaction = library.readTransaction();
        optDescriptor = library.resources().reader(transaction).get(resourceId);
        revision = library.libraryRevision(transaction);
        indexPtr = carrierIndexSlot.load();
      }

      if (!optDescriptor)
      {
        return std::optional<std::vector<std::byte>>{};
      }

      if (!indexPtr || !indexPtr->answersRevision(revision))
      {
        indexPtr = rebuildCarrierIndex(revision);
      }

      auto const context = ResourceMaterializationContext{
        .descriptor = *optDescriptor,
        .candidateUris = indexPtr->carrierUris(resourceId),
        .musicRoot = library.rootPath(),
        .cache = diskCache,
        .optMaximumBytes = optMaximumBytes,
      };
      return materializeResource(context, stopToken);
    }

    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    ResourceDiskCache diskCache;
    compat::AtomicSharedPtr<ResourceCarrierIndex const> carrierIndexSlot{};
    std::mutex carrierIndexMutex;
    std::atomic<std::uint64_t> carrierIndexBuildCount{0};
  };

  ResourceMaterializer::ResourceMaterializer(async::Runtime& asyncRuntime,
                                             library::MusicLibrary& library,
                                             std::filesystem::path const& cacheDirectory)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, library, cacheDirectory)}
  {
  }

  ResourceMaterializer::~ResourceMaterializer() = default;

  async::Task<Result<std::optional<std::vector<std::byte>>>> ResourceMaterializer::loadInteractiveAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    return loadAsync(resourceId, kMaximumInteractiveResourceBytes, stopToken);
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> ResourceMaterializer::loadAdministrativeAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    return loadAsync(resourceId, std::nullopt, stopToken);
  }

  std::uint64_t ResourceMaterializer::carrierIndexBuildCount() const noexcept
  {
    return _implPtr->carrierIndexBuildCount.load();
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> ResourceMaterializer::loadAsync(
    ResourceId const resourceId,
    std::optional<std::size_t> const optMaximumBytes,
    std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (resourceId == kInvalidResourceId)
    {
      co_return std::optional<std::vector<std::byte>>{};
    }

    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    auto result = _implPtr->loadResource(resourceId, optMaximumBytes, stopToken);
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    co_return result;
  }
} // namespace ao::rt
