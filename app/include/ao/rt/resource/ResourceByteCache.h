// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace ao::rt
{
  class ResourceByteCache final
  {
  public:
    static constexpr std::size_t kDefaultMaximumEntries = 128;
    static constexpr std::size_t kDefaultMaximumBytes = std::size_t{128U} * 1024U * 1024U;

    explicit ResourceByteCache(std::size_t maximumEntries = kDefaultMaximumEntries,
                               std::size_t maximumBytes = kDefaultMaximumBytes);

    void reset();
    bool store(ResourceId resourceId, ResourceBytes bytes);
    ResourceBytes cached(ResourceId resourceId);

    std::size_t cachedCount() const noexcept { return _entries.size(); }
    std::size_t cachedBytes() const noexcept { return _cachedBytes; }
    std::size_t maximumEntries() const noexcept { return _maximumEntries; }
    std::size_t maximumBytes() const noexcept { return _maximumBytes; }

  private:
    struct Entry final
    {
      ResourceBytes bytes;
      std::uint64_t lastUse = 0;
    };

    void evictLeastRecentlyUsed();

    std::size_t _maximumEntries = kDefaultMaximumEntries;
    std::size_t _maximumBytes = kDefaultMaximumBytes;
    std::size_t _cachedBytes = 0;
    std::uint64_t _useSequence = 0;
    std::unordered_map<ResourceId, Entry> _entries{};
  };
} // namespace ao::rt
