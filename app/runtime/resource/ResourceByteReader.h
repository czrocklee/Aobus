// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "ResourceByteDiskCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/compat/AtomicSharedPtr.h>
#include <ao/library/ResourceLayout.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class ResourceCarrierIndex;

  /// Frontend resource delivery is bounded independently of the disk-cache budget.
  inline constexpr std::size_t kMaximumInteractiveResourceBytes = std::size_t{32U} * 1024U * 1024U;

  /** Inputs for one verified whole-resource read. References outlive the call. */
  struct ResourceByteReadContext final
  {
    /// The row the handle resolved to. Its digest is the whole acceptance test.
    library::ResourceDescriptor descriptor;

    /// Files that reference this resource, in the index's stable order.
    std::span<std::string const> candidateUris;

    /// Where a relative manifest URI resolves, so a relocated root works.
    std::filesystem::path const& musicRoot;

    ResourceByteDiskCache const& diskCache;

    /// Absent for CLI export, which is exempt from the interactive byte limit.
    std::optional<std::size_t> optMaximumBytes;
  };

  /**
   * @brief Reads the bytes a resource names, or reports that none can be reproduced.
   *
   * Walks by cost: the derived disk cache first, then every referencing carrier.
   * A source answers only when its bytes hash to the descriptor digest; missing,
   * unreadable, changed, or unsupported carriers advance to the next candidate
   * rather than failing the request.
   *
   * Returns no bytes after every source is exhausted, `ValueTooLarge` when the
   * verified payload exceeds the caller's ceiling, and throws
   * `async::OperationCancelled` when @p stopToken is signalled. Cancellation is
   * checked between carriers so a slow multi-file walk remains stoppable.
   *
   * Opens no library transaction. The caller pins the descriptor and immutable
   * carrier snapshot first, so filesystem I/O never holds back database page
   * reuse for concurrent writers.
   */
  Result<std::optional<std::vector<std::byte>>> readResourceBytes(ResourceByteReadContext const& context,
                                                                  std::stop_token const& stopToken);

  /**
   * Core-runtime-owned reader for ResourceId-keyed encoded byte payloads.
   *
   * Each request copies its descriptor and an immutable carrier-index snapshot
   * under a short read transaction, then performs cache and media-file I/O on a
   * worker. Calls may begin on any executor: a valid id goes directly to a worker
   * without staging through the callback executor, and every result resumes on
   * that callback executor before return. Interactive and export callers share
   * the read and differ only in whether the encoded-byte ceiling is present.
   */
  class ResourceByteReader final
  {
  public:
    ResourceByteReader(async::Runtime& asyncRuntime,
                       library::MusicLibrary& library,
                       std::filesystem::path const& cacheDirectory);

    async::Task<Result<std::optional<std::vector<std::byte>>>> readInteractiveAsync(ResourceId resourceId,
                                                                                    std::stop_token stopToken = {});
    async::Task<Result<std::optional<std::vector<std::byte>>>> readForExportAsync(ResourceId resourceId,
                                                                                  std::stop_token stopToken = {});

    /// Source-private seam proving lazy, serialized carrier-index rebuilds.
    std::uint64_t carrierIndexBuildCount() const noexcept;

  private:
    std::shared_ptr<ResourceCarrierIndex const> rebuildCarrierIndex(std::uint64_t requestRevision);
    Result<std::optional<std::vector<std::byte>>> readOnWorker(ResourceId resourceId,
                                                               std::optional<std::size_t> optMaximumBytes,
                                                               std::stop_token const& stopToken);
    async::Task<Result<std::optional<std::vector<std::byte>>>> readAsync(ResourceId resourceId,
                                                                         std::optional<std::size_t> optMaximumBytes,
                                                                         std::stop_token stopToken);

    async::Runtime& _asyncRuntime;
    library::MusicLibrary& _library;
    ResourceByteDiskCache _diskCache;
    compat::AtomicSharedPtr<ResourceCarrierIndex const> _carrierIndexSlot{};
    std::mutex _carrierIndexMutex;
    std::atomic<std::uint64_t> _carrierIndexBuildCount{0};
  };
} // namespace ao::rt
