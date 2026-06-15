// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Exception.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>

#include <string>
#include <string_view>

namespace ao::query
{
  Field resolveVariableField(VariableType type, std::string_view name)
  {
    switch (type)
    {
      case VariableType::Property:
      {
        if (auto const* spec = findQueryVariableCompletionSpec(type, name); spec != nullptr)
        {
          return spec->field;
        }

        throwException<Exception>("unknown property field '@{}'", name);
      }
      case VariableType::Metadata:
      {
        if (auto const* spec = findQueryVariableCompletionSpec(type, name); spec != nullptr)
        {
          return spec->field;
        }

        throwException<Exception>("unknown metadata field '${}'", name);
      }
      case VariableType::Tag: return Field::Tag;
      case VariableType::Custom: return Field::Custom;
      default: break;
    }

    throwException<Exception>("unsupported variable type for '{}'", name);
  }

  Field resolveVariableField(VariableExpression const& variable)
  {
    return resolveVariableField(variable.type, variable.name);
  }

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
} // namespace ao::query
