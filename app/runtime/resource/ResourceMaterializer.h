// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Task.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
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
  /**
   * Source-private resolution of resource handles into verified encoded bytes.
   *
   * Descriptor and carrier-index reads finish before cache or media-file I/O.
   * Interactive and administrative callers share that walk and differ only in
   * whether the encoded-byte ceiling is present.
   */
  class ResourceMaterializer final
  {
  public:
    ResourceMaterializer(async::Runtime& asyncRuntime,
                         library::MusicLibrary& library,
                         std::filesystem::path const& cacheDirectory);
    ~ResourceMaterializer();

    ResourceMaterializer(ResourceMaterializer const&) = delete;
    ResourceMaterializer& operator=(ResourceMaterializer const&) = delete;
    ResourceMaterializer(ResourceMaterializer&&) = delete;
    ResourceMaterializer& operator=(ResourceMaterializer&&) = delete;

    async::Task<Result<std::optional<std::vector<std::byte>>>> loadInteractiveAsync(ResourceId resourceId,
                                                                                    std::stop_token stopToken = {});
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadAdministrativeAsync(ResourceId resourceId,
                                                                                       std::stop_token stopToken = {});

    /// Source-private seam proving lazy, serialized carrier-index rebuilds.
    std::uint64_t carrierIndexBuildCount() const noexcept;

  private:
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadAsync(ResourceId resourceId,
                                                                         std::optional<std::size_t> optMaximumBytes,
                                                                         std::stop_token stopToken);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
