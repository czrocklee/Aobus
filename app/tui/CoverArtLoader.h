// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CoverArt.h"
#include <ao/CoreIds.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>

namespace ao::rt
{
  class LibraryTaskService;
}

namespace ao::tui
{
  enum class CoverArtDeliveryMode : std::uint8_t
  {
    Off,
    Blocks,
    Kitty,
  };

  /**
   * Callback-executor-confined owner for TUI cover-art delivery.
   *
   * Resource reads and decoding run asynchronously. A replacement request
   * clears the published result immediately, and generation checks prevent a
   * stale decode from publishing over the current selection.
   */
  class CoverArtLoader final
  {
  public:
    using RefreshCallback = std::function<void()>;

    CoverArtLoader(rt::LibraryTaskService& tasks,
                   async::Runtime& runtime,
                   CoverArtDeliveryMode mode,
                   RefreshCallback refresh);
    ~CoverArtLoader();

    CoverArtLoader(CoverArtLoader const&) = delete;
    CoverArtLoader& operator=(CoverArtLoader const&) = delete;
    CoverArtLoader(CoverArtLoader&&) = delete;
    CoverArtLoader& operator=(CoverArtLoader&&) = delete;

    void request(ResourceId resourceId);
    void clear();
    void cancel() noexcept;

    ResourceId resourceId() const noexcept { return _resourceId; }
    std::optional<CoverArtRows> const& preview() const noexcept { return _optPreview; }
    std::optional<std::vector<std::byte>> const& kittyPng() const noexcept { return _optKittyPng; }

  private:
    static async::Task<void> load(CoverArtLoader* loader,
                                  rt::LibraryTaskService* tasks,
                                  async::Runtime* runtime,
                                  CoverArtDeliveryMode mode,
                                  ResourceId resourceId,
                                  std::uint64_t generation,
                                  std::stop_token stopToken);

    rt::LibraryTaskService& _tasks;
    async::Runtime& _runtime;
    CoverArtDeliveryMode _mode;
    RefreshCallback _refresh;
    ResourceId _resourceId = kInvalidResourceId;
    std::optional<CoverArtRows> _optPreview;
    std::optional<std::vector<std::byte>> _optKittyPng;
    std::uint64_t _generation = 0;
    async::TaskHandle _task;
  };
} // namespace ao::tui
