// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackRow.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

namespace ao::uimodel
{
  class IndexedTrackRowCache final
  {
  public:
    using Loader = std::function<std::optional<rt::TrackRow>(std::size_t)>;

    static constexpr std::size_t kDefaultMaximumEntries = 2048;

    explicit IndexedTrackRowCache(std::size_t maximumEntries = kDefaultMaximumEntries);

    void reset(std::size_t sourceSize, Loader loader);
    rt::TrackRow const* rowAt(std::size_t index);

    std::size_t sourceSize() const noexcept { return _sourceSize; }
    std::size_t maximumEntries() const noexcept { return _maximumEntries; }
    bool contains(std::size_t index) const noexcept { return _entries.contains(index); }

  private:
    struct Entry final
    {
      rt::TrackRow row{};
      std::uint64_t lastUse = 0;
    };

    void evictLeastRecentlyUsed();

    std::size_t _maximumEntries = kDefaultMaximumEntries;
    std::size_t _sourceSize = 0;
    std::uint64_t _useSequence = 0;
    Loader _loader{};
    std::unordered_map<std::size_t, Entry> _entries{};
  };
} // namespace ao::uimodel
