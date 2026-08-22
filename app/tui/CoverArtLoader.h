// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CoverArt.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>

namespace ao::rt
{
  class ResourceByteLoader;
}

namespace ao::tui
{
  /// How long a selection must stand still before its cover is worth reading.
  inline constexpr auto kCoverArtSelectionSettleInterval = std::chrono::milliseconds{100};

  /**
   * Callback-executor-confined owner for TUI cover-art delivery.
   *
   * Resource reads and decoding run asynchronously. A replacement request
   * clears the published result immediately, and task cancellation prevents a
   * stale decode from publishing over the current selection.
   *
   * Detail follows the track table, so a held arrow key walks through covers
   * the user never sees. Each request therefore waits out
   * @ref kCoverArtSelectionSettleInterval before it reads anything: cancelling
   * an interest does not unstart a read the shared loader has already begun,
   * and every skipped read is a cover re-extracted from a media file for
   * nothing.
   */
  class CoverArtLoader final
  {
  public:
    using RefreshCallback = std::function<void()>;

    CoverArtLoader(rt::ResourceByteLoader& byteLoader,
                   async::Runtime& runtime,
                   CoverArtDeliveryMode mode,
                   RefreshCallback refresh,
                   std::int32_t columns);
    ~CoverArtLoader();

    CoverArtLoader(CoverArtLoader const&) = delete;
    CoverArtLoader& operator=(CoverArtLoader const&) = delete;
    CoverArtLoader(CoverArtLoader&&) = delete;
    CoverArtLoader& operator=(CoverArtLoader&&) = delete;

    void request(ResourceId resourceId);
    void clear();
    void cancel() noexcept;

    ResourceId resourceId() const noexcept { return _resourceId; }
    std::int32_t columns() const noexcept { return _columns; }
    std::optional<CoverArtRows> const& preview() const noexcept { return _optPreview; }
    std::optional<std::vector<std::byte>> const& kittyPng() const noexcept { return _optKittyPng; }

  private:
    static async::Task<void> waitForSelectionSettle(CoverArtLoader* loader,
                                                    async::Runtime* runtime,
                                                    ResourceId resourceId,
                                                    std::stop_token stopToken);
    static async::Task<void> load(CoverArtLoader* loader,
                                  async::Runtime* runtime,
                                  CoverArtDeliveryMode mode,
                                  std::int32_t columns,
                                  rt::ResourceBytes bytes,
                                  std::stop_token stopToken);
    void startByteRequest(ResourceId resourceId);

    rt::ResourceByteLoader& _byteLoader;
    async::Runtime& _runtime;
    CoverArtDeliveryMode _mode;
    RefreshCallback _refresh;
    std::int32_t _columns;
    ResourceId _resourceId = kInvalidResourceId;
    std::optional<CoverArtRows> _optPreview;
    std::optional<std::vector<std::byte>> _optKittyPng;
    rt::ResourceByteLoader::Request _byteRequest;
    // Declared last so teardown requests stop before any callback target dies.
    async::TaskHandle _settleTask;
    async::TaskHandle _task;
  };
} // namespace ao::tui
