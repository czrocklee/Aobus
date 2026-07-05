// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/utility/ByteView.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace ao::library::detail
{
  /**
   * TrackColdReader - Deep structural verifier for cold track records.
   *
   * This is the diagnostic counterpart to TrackView's O(1) read gate: it
   * re-checks every write-side invariant (no inter-block gaps, zeroed
   * padding and reserved fields, exact block sizes, sorted custom keys,
   * contiguous value ranges). It is intentionally O(record) and belongs in
   * verification paths only - CLI record dumps and serialization tests -
   * never in the row read path.
   */
  class TrackColdReader final
  {
  public:
    explicit TrackColdReader(std::span<std::byte const> coldBytes) noexcept;

    bool valid() const noexcept { return _valid; }

    TrackColdHeader const& header() const noexcept;

    ClassicalProxy classical() const noexcept { return ClassicalProxy{_classicalPayload}; }

    CoverArtProxy coverArt() const noexcept
    {
      return CoverArtProxy{utility::layout::viewArray<CoverArtEntry>(_coverPayload)};
    }

    CustomMetadataProxy custom() const noexcept { return CustomMetadataProxy{_customPayload}; }

    std::string_view uri() const noexcept;

  private:
    std::span<std::byte const> _coldBytes{};
    TrackColdHeader const* _header = nullptr;
    std::span<std::byte const> _coverPayload{};
    std::span<std::byte const> _classicalPayload{};
    std::span<std::byte const> _customPayload{};
    bool _valid = false;
  };
} // namespace ao::library::detail
