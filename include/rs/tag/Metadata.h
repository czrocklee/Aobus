// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <array>
#include <map>
#include <rs/tag/ValueType.h>

namespace rs::tag
{
  enum class MetaField : size_t
  {
    Title = 0,
    Artist,
    Album,
    AlbumArtist,
    AlbumArt,
    Genre,
    Year,
    TrackNumber,
    TotalTracks,
    DiscNumber,
    TotalDiscs,
    LAST_FIELD
  };

  class Metadata final
  {
  public:
    ValueType const& get(MetaField field) const { return _fields[static_cast<std::size_t>(field)]; }

    void set(MetaField field, ValueType value) { _fields[static_cast<std::size_t>(field)] = std::move(value); }

    // NOLINTBEGIN(readability-convert-member-functions-to-static)
    ValueType const& getCustom(std::string_view field) const
    {
      if (auto iter = _customFields.find(field); iter != _customFields.end())
      {
        return iter->second;
      }

      static ValueType const empty{};
      return empty;
    }
    // NOLINTEND(readability-convert-member-functions-to-static)

    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    void setCustom(std::string_view field, ValueType value)
    // NOLINTEND(bugprone-easily-swappable-parameters)
    {
      _customFields.insert_or_assign(std::string{field}, std::move(value));
    }

  private:
    std::array<ValueType, static_cast<std::size_t>(MetaField::LAST_FIELD)> _fields;
    std::map<std::string, ValueType, std::less<>> _customFields;
  };

}
