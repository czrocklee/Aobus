/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace rs::core
{

  /**
   * ListHeader - POD struct for binary list storage.
   * Total size: 32 bytes with 8-byte alignment.
   */
  struct ListHeader
  {
    // 8-byte section
    std::uint64_t trackIdsOffset; // Offset to track IDs array in payload
    std::uint64_t trackIdsCount;  // Number of track IDs

    // 4-byte section
    std::uint32_t nameId;   // Dictionary ID for list name
    std::uint32_t descId;   // Dictionary ID for list description
    std::uint32_t filterId; // Dictionary ID for filter expression

    // 2-byte section
    std::uint16_t nameOffset; // Offset to name string in payload
    std::uint16_t nameLen;    // Length of name string
    std::uint16_t descOffset; // Offset to description string in payload
    std::uint16_t descLen;    // Length of description string

    // 1-byte section
    std::uint8_t flags; // List flags (0 = manual, 1 = smart)

    // Padding
    std::uint8_t reserved[5];
  };

  static_assert(sizeof(ListHeader) == 48, "ListHeader must be exactly 48 bytes");
  static_assert(alignof(ListHeader) == 8, "ListHeader must have 8-byte alignment");

  /**
   * ListView - Safe accessor for list data stored in binary format.
   */
  class ListView
  {
  public:
    ListView() noexcept : _header(nullptr), _payloadBase(nullptr), _size(0) {}

    ListView(const void* data, std::size_t size) noexcept
      : _header(static_cast<const ListHeader*>(data))
      , _payloadBase(static_cast<const std::uint8_t*>(data))
      , _size(size)
    {
    }

    bool is_valid() const noexcept { return _header != nullptr && _size >= sizeof(ListHeader); }

    const ListHeader* header() const noexcept { return _header; }

    std::uint64_t trackIdsCount() const noexcept { return _header->trackIdsCount; }
    std::uint32_t nameId() const noexcept { return _header->nameId; }
    std::uint32_t descId() const noexcept { return _header->descId; }
    std::uint32_t filterId() const noexcept { return _header->filterId; }
    std::uint8_t flags() const noexcept { return _header->flags; }

    std::string_view name() const { return getString(_header->nameOffset, _header->nameLen); }

    std::string_view description() const { return getString(_header->descOffset, _header->descLen); }

    std::string_view payload() const
    {
      if (!is_valid())
        return {};
      auto payloadStart = _payloadBase + sizeof(ListHeader);
      auto payloadSize = _size - sizeof(ListHeader);
      return {reinterpret_cast<const char*>(payloadStart), payloadSize};
    }

  private:
    const ListHeader* _header;
    const std::uint8_t* _payloadBase;
    std::size_t _size;

    std::string_view getString(std::uint16_t offset, std::uint16_t len) const;
  };

} // namespace rs::core
