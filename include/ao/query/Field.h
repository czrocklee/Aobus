// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::library
{
  class TrackView;
  class DictionaryStore;
} // namespace ao::library

namespace ao::query
{
  /**
   * Field - Identifies which field to read from a track.
   * Ordered by category: string -> property -> metadata -> tags.
   */
  enum class Field : std::uint8_t
  {
    // String fields
    Title = 0,
    Uri = 1,

    // Property fields (@ prefix) - audio technical properties
    Duration = 2,
    Bitrate = 3,
    SampleRate = 4,
    Channels = 5,
    BitDepth = 6,
    Codec = 7,
    // 8 retired (formerly Rating; field IDs are a stable contract, slot left as a gap)

    // Metadata ID fields (Dictionary IDs)
    ArtistId = 9,
    AlbumId = 10,
    GenreId = 11,
    AlbumArtistId = 12,
    ComposerId = 13,
    CoverArtId = 14,
    WorkId = 15,

    // Metadata numeric fields
    Year = 16,
    TrackNumber = 17,
    TrackTotal = 18,
    DiscNumber = 19,
    DiscTotal = 20,

    // Tag fields
    TagBloom = 21,
    TagCount = 22,
    Tag = 23,

    // Custom field (for %custom_key lookups from cold storage)
    Custom = 24,
  };

  /**
   * AccessProfile - Indicates which storage tier(s) an expression needs.
   */
  enum class AccessProfile : std::uint8_t
  {
    NoTrackData, // Does not access track storage
    HotOnly,     // Only accesses hot data (metadata, property, tags)
    ColdOnly,    // Only accesses cold data (custom KV)
    HotAndCold   // Mixed access
  };

  bool isColdField(Field field);
  bool isDictionaryField(Field field);
  bool isStringField(Field field);
  bool isTagField(Field field);

  /**
   * Resolves a dictionary-backed metadata field (ArtistId, AlbumId, ...) to its
   * interned string. Returns an empty view for non-dictionary fields or an
   * invalid/unset id. The returned view borrows from @p dict and is valid for as
   * long as the dictionary entry lives.
   */
  std::string_view dictionaryFieldValue(library::TrackView const& track,
                                        Field field,
                                        library::DictionaryStore const& dict);

  std::string_view fieldDisplayName(Field field);
  char variablePrefix(VariableType type);
  std::string variableDisplayName(VariableExpression const& variable);

  bool requiresHotData(AccessProfile profile);
  bool requiresColdData(AccessProfile profile);
  bool hasRequiredTrackData(AccessProfile profile, library::TrackView const& track);
} // namespace ao::query
