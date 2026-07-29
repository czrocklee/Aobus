// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace ao::library
{
  struct ListHeader;

  /** Read-only, structurally validated view over a serialized List record. */
  class ListView final
  {
  public:
    explicit ListView(std::span<std::byte const> data) noexcept;

    /** True when the record passed its structural gate. */
    bool isValid() const noexcept { return _header != nullptr; }

    std::string_view name() const noexcept;
    std::string_view description() const noexcept;

    /** Local expression; an empty value is the identity expression. */
    std::string_view filter() const noexcept;

    /** Parent list ID (0 = All Tracks / root) */
    ListId parentId() const noexcept;

    /** True if this list's parent is All Tracks (the root) */
    bool isRootParent() const noexcept;

    class OrderTrackIdProxy : public std::ranges::view_interface<OrderTrackIdProxy>
    {
    public:
      OrderTrackIdProxy() = default;

      explicit OrderTrackIdProxy(std::span<TrackId const> trackIds) noexcept
        : _trackIds{trackIds}
      {
      }

      TrackId at(std::size_t index) const noexcept
      {
        gsl_Expects(index < _trackIds.size());
        return _trackIds[index];
      }

      TrackId operator[](std::size_t index) const noexcept { return at(index); }

      TrackId const* data() const noexcept { return _trackIds.data(); }
      TrackId const* begin() const noexcept { return _trackIds.data(); }
      TrackId const* end() const noexcept { return _trackIds.data() + _trackIds.size(); }
      // These O(1) members intentionally refine view_interface's CRTP fallback.
      bool empty() const noexcept { return _trackIds.empty(); }
      std::size_t size() const noexcept { return _trackIds.size(); }

    private:
      std::span<TrackId const> _trackIds{};
    };

    OrderTrackIdProxy orderTrackIds() const noexcept;

    std::span<std::byte const> rawData() const noexcept { return _payload; }

  private:
    std::string_view stringAt(std::size_t offset, std::uint32_t length) const noexcept;

    std::span<std::byte const> _payload;
    ListHeader const* _header = nullptr;
    std::size_t _nameOffset = 0;
    std::size_t _descOffset = 0;
    std::size_t _filterOffset = 0;
  };
} // namespace ao::library
