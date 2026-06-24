// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/FieldCatalog.h>
#include <ao/query/detail/FieldResolver.h>

#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace ao::query::detail
{
  Result<Field> resolveVariableField(VariableType type, std::string_view name)
  {
    switch (type)
    {
      case VariableType::Property:
      {
        if (auto const* spec = findQueryVariableCompletionSpec(type, name); spec != nullptr)
        {
          return spec->field;
        }

        return makeError(Error::Code::FormatRejected, std::format("unknown property field '@{}'", name));
      }
      case VariableType::Metadata:
      {
        if (auto const* spec = findQueryVariableCompletionSpec(type, name); spec != nullptr)
        {
          return spec->field;
        }

        return makeError(Error::Code::FormatRejected, std::format("unknown metadata field '${}'", name));
      }
      case VariableType::Tag: return Field::Tag;
      case VariableType::Custom: return Field::Custom;
      default: break;
    }

    return makeError(Error::Code::FormatRejected, std::format("unsupported variable type for '{}'", name));
  }

  Result<Field> resolveVariableField(VariableExpression const& variable)
  {
    return resolveVariableField(variable.type, variable.name);
  }

  std::optional<Field> lookupVariableField(VariableType type, std::string_view name)
  {
    switch (type)
    {
      case VariableType::Property:
      case VariableType::Metadata:
      {
        if (auto const* spec = findQueryVariableCompletionSpec(type, name); spec != nullptr)
        {
          return spec->field;
        }

        return std::nullopt;
      }
      case VariableType::Tag: return Field::Tag;
      case VariableType::Custom: return Field::Custom;
      default: return std::nullopt;
    }
  }

  std::optional<Field> lookupVariableField(VariableExpression const& variable)
  {
    return lookupVariableField(variable.type, variable.name);
  }
} // namespace ao::query::detail

namespace ao::query
{
  bool isColdField(Field field)
  {
    switch (field)
    {
      case Field::Uri:
      case Field::CoverArtId:
      case Field::WorkId:
      case Field::TrackNumber:
      case Field::TrackTotal:
      case Field::DiscNumber:
      case Field::DiscTotal:
      case Field::Custom:
      case Field::Duration:
      case Field::Bitrate:
      case Field::Channels: return true;
      default: return false;
    }
  }

  bool isDictionaryField(Field field)
  {
    switch (field)
    {
      case Field::ArtistId:
      case Field::AlbumId:
      case Field::GenreId:
      case Field::AlbumArtistId:
      case Field::ComposerId:
      case Field::WorkId: return true;
      default: return false;
    }
  }

  bool isStringField(Field field)
  {
    switch (field)
    {
      case Field::Title:
      case Field::Uri:
      case Field::Custom: return true;
      default: return false;
    }
  }

  bool isTagField(Field field)
  {
    return field == Field::Tag;
  }

  std::string_view fieldDisplayName(Field field)
  {
    switch (field)
    {
      case Field::Duration: return "duration";
      case Field::Bitrate: return "bitrate";
      case Field::SampleRate: return "sampleRate";
      case Field::Channels: return "channels";
      case Field::BitDepth: return "bitDepth";
      case Field::Year: return "year";
      case Field::TrackNumber: return "trackNumber";
      case Field::TrackTotal: return "trackTotal";
      case Field::DiscNumber: return "discNumber";
      case Field::DiscTotal: return "discTotal";
      case Field::ArtistId: return "artist";
      case Field::AlbumId: return "album";
      case Field::GenreId: return "genre";
      case Field::AlbumArtistId: return "albumArtist";
      case Field::ComposerId: return "composer";
      case Field::WorkId: return "work";
      default: return "field";
    }
  }

  char variablePrefix(VariableType type)
  {
    switch (type)
    {
      case VariableType::Metadata: return '$';
      case VariableType::Property: return '@';
      case VariableType::Tag: return '#';
      case VariableType::Custom: return '%';
      default: return '?';
    }
  }

  std::string variableDisplayName(VariableExpression const& variable)
  {
    auto name = std::string{};
    name.push_back(variablePrefix(variable.type));
    name += variable.name;
    return name;
  }

  bool requiresHotData(AccessProfile profile)
  {
    return profile == AccessProfile::HotOnly || profile == AccessProfile::HotAndCold;
  }

  bool requiresColdData(AccessProfile profile)
  {
    return profile == AccessProfile::ColdOnly || profile == AccessProfile::HotAndCold;
  }

  bool hasRequiredTrackData(AccessProfile profile, library::TrackView const& track)
  {
    if (requiresHotData(profile) && !track.isHotValid())
    {
      return false;
    }

    if (requiresColdData(profile) && !track.isColdValid())
    {
      return false;
    }

    return true;
  }

  std::string_view dictionaryFieldValue(library::TrackView const& track,
                                        Field field,
                                        library::DictionaryStore const& dict)
  {
    auto dictionaryId = kInvalidDictionaryId;

    switch (field)
    {
      case Field::ArtistId: dictionaryId = track.metadata().artistId(); break;
      case Field::AlbumId: dictionaryId = track.metadata().albumId(); break;
      case Field::GenreId: dictionaryId = track.metadata().genreId(); break;
      case Field::AlbumArtistId: dictionaryId = track.metadata().albumArtistId(); break;
      case Field::ComposerId: dictionaryId = track.metadata().composerId(); break;
      case Field::WorkId: dictionaryId = track.metadata().workId(); break;
      default: return {};
    }

    if (dictionaryId == kInvalidDictionaryId)
    {
      return {};
    }

    return dict.get(dictionaryId);
  }
} // namespace ao::query
