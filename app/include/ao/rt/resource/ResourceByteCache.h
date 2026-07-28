// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <cstdint>
#include <unordered_map>

namespace ao::rt
{
  class ResourceByteCache final
  {
  public:
    static constexpr std::size_t kDefaultMaximumEntries = 128;

    explicit ResourceByteCache(std::size_t maximumEntries = kDefaultMaximumEntries);

    void reset();
    bool store(ResourceId resourceId, ResourceBytes bytes);
    ResourceBytes cached(ResourceId resourceId);

    std::size_t cachedCount() const noexcept { return _entries.size(); }
    std::size_t maximumEntries() const noexcept { return _maximumEntries; }

  private:
    struct Entry final
    {
      ResourceBytes bytes;
      std::uint64_t lastUse = 0;
    };

    void evictLeastRecentlyUsed();

    std::size_t _maximumEntries = kDefaultMaximumEntries;
    std::uint64_t _useSequence = 0;
    std::unordered_map<ResourceId, Entry> _entries{};
  };
} // namespace ao::rt
