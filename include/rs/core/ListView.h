// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/ListLayout.h>
#include <rs/utility/ByteView.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace rs::core
{
  /**
   * ListView - Safe accessor for list data stored in binary format.
   * Reads fields directly from payload without storing header.
   */
  class ListView final
  {
  public:
    explicit ListView(std::span<std::byte const> data);

    std::string_view name() const;
    std::string_view description() const;
    std::string_view filter() const;

    bool isSmart() const noexcept { return !filter().empty(); }

    /**
     * TrackProxy - Iterator and index access to track IDs in the list.
     */
    class TrackProxy
    {
    public:
      TrackProxy(std::span<TrackId const> trackIds);

      TrackId at(std::size_t index) const;
      TrackId operator[](std::size_t index) const { return at(index); }

      TrackId const* begin() const { return _trackIds.data(); }
      TrackId const* end() const { return _trackIds.data() + _trackIds.size(); }
      std::size_t size() const { return _trackIds.size(); }

    private:
      std::span<TrackId const> _trackIds;
    };

    TrackProxy tracks() const;

  private:
    ListHeader const* header() const { return utility::as<ListHeader>(_payload); }
    std::string_view getString(std::uint16_t offset, std::uint16_t length) const;

    std::span<std::byte const> _payload;
  };

} // namespace rs::core
