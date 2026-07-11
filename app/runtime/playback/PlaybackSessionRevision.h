// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::rt
{
  /** Monotonic exact-acknowledgement state for one serialized playback intent. */
  class PlaybackSessionRevision final
  {
  public:
    using Value = std::uint64_t;

    bool markDirty() noexcept
    {
      auto const cleanToDirty = !_dirty;
      ++_current;
      _dirty = true;
      return cleanToDirty;
    }

    Value capture() const noexcept { return _current; }

    void acknowledge(Value const captured) noexcept
    {
      if (_current == captured)
      {
        _dirty = false;
      }
    }

    void resetClean() noexcept { _dirty = false; }
    bool dirty() const noexcept { return _dirty; }

  private:
    Value _current = 0;
    bool _dirty = false;
  };
} // namespace ao::rt
