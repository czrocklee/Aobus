// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace ao::uimodel
{
  struct CoverArtRequestToken final
  {
    ResourceId resourceId{kInvalidResourceId};
    std::uint64_t generation = 0;

    friend bool operator==(CoverArtRequestToken const&, CoverArtRequestToken const&) = default;
  };

  class CoverArtRequestModel final
  {
  public:
    static constexpr std::size_t kDefaultMaximumEntries = 128;

    explicit CoverArtRequestModel(std::size_t maximumEntries = kDefaultMaximumEntries);

    CoverArtRequestToken select(ResourceId resourceId);
    void clearSelection();
    void reset();
    bool accepts(CoverArtRequestToken token) const noexcept;
    bool store(CoverArtRequestToken token, std::vector<std::byte> bytes);
    std::span<std::byte const> cached(ResourceId resourceId);

    ResourceId selectedResourceId() const noexcept { return _selectedResourceId; }
    std::size_t cachedCount() const noexcept { return _entries.size(); }
    std::size_t maximumEntries() const noexcept { return _maximumEntries; }

  private:
    struct Entry final
    {
      std::vector<std::byte> bytes{};
      std::uint64_t lastUse = 0;
    };

    void evictLeastRecentlyUsed();

    std::size_t _maximumEntries = kDefaultMaximumEntries;
    ResourceId _selectedResourceId{kInvalidResourceId};
    std::uint64_t _generation = 0;
    std::uint64_t _useSequence = 0;
    std::unordered_map<ResourceId, Entry> _entries{};
  };
} // namespace ao::uimodel
