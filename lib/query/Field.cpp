// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/query/Serializer.h>
#include <ao/query/detail/FieldResolver.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query::detail
{
  namespace
  {
    std::string fieldListText(VariableType type)
    {
      auto result = std::string{};

      for (auto const& descriptor : queryVariableDescriptors(type))
      {
        if (!result.empty())
        {
          result += ", ";
        }

        result += variableDisplayName(type, descriptor.canonicalName);

        if (!descriptor.aliases.empty())
        {
          result += " (";

          for (std::size_t index = 0; index < descriptor.aliases.size(); ++index)
          {
            if (index > 0)
            {
              result += ", ";
            }

            result += variableDisplayName(type, descriptor.aliases[index]);
          }

          result += ")";
        }
      }

      return result;
    }

    char lowerAscii(char ch)
    {
      if (ch >= 'A' && ch <= 'Z')
      {
        return static_cast<char>(ch - 'A' + 'a');
      }

      return ch;
    }

    std::size_t editDistanceInsensitive(std::string_view lhs, std::string_view rhs)
    {
      auto previous = std::vector<std::size_t>(rhs.size() + 1);
      auto current = std::vector<std::size_t>(rhs.size() + 1);

      for (std::size_t index = 0; index <= rhs.size(); ++index)
      {
        previous[index] = index;
      }

      for (std::size_t lhsIndex = 0; lhsIndex < lhs.size(); ++lhsIndex)
      {
        current[0] = lhsIndex + 1;

        for (std::size_t rhsIndex = 0; rhsIndex < rhs.size(); ++rhsIndex)
        {
          auto const substitutionCost = lowerAscii(lhs[lhsIndex]) == lowerAscii(rhs[rhsIndex]) ? 0U : 1U;
          current[rhsIndex + 1] =
            std::min({previous[rhsIndex + 1] + 1, current[rhsIndex] + 1, previous[rhsIndex] + substitutionCost});
        }

        previous.swap(current);
      }

      return previous.back();
    }

    std::string closestFieldSuggestion(VariableType type, std::string_view name)
    {
      auto bestDistance = std::numeric_limits<std::size_t>::max();
      auto bestName = std::string_view{};

      for (auto const& descriptor : queryVariableDescriptors(type))
      {
        auto const check = [&](std::string_view candidate)
        {
          auto const distance = editDistanceInsensitive(name, candidate);

          if (distance < bestDistance)
          {
            bestDistance = distance;
            bestName = descriptor.canonicalName;
          }
        };

        check(descriptor.canonicalName);

        for (auto const alias : descriptor.aliases)
        {
          check(alias);
        }
      }

      if (bestDistance > 2 || bestName.empty())
      {
        return {};
      }

      return variableDisplayName(type, bestName);
    }

    std::string variableDomainName(VariableType type)
    {
      switch (type)
      {
        case VariableType::Metadata: return "metadata";
        case VariableType::Property: return "property";
        case VariableType::Tag: return "tag";
        case VariableType::Custom: return "custom";
      }

      return "variable";
    }

    Error unknownFieldError(VariableType type, std::string_view name)
    {
      auto message = std::format("unknown {} field '{}'", variableDomainName(type), variableDisplayName(type, name));

      if (auto const suggestion = closestFieldSuggestion(type, name); !suggestion.empty())
      {
        message += std::format("; did you mean '{}'?", suggestion);
      }

      message += std::format("; available {} fields: {}", variableDomainName(type), fieldListText(type));
      return Error{.code = Error::Code::FormatRejected, .message = std::move(message)};
    }
  } // namespace

  Result<Field> resolveVariableField(VariableType type, std::string_view name)
  {
    switch (type)
    {
      case VariableType::Property:
      {
        if (auto const* descriptor = findQueryVariableDescriptor(type, name); descriptor != nullptr)
        {
          return descriptor->field;
        }

        return std::unexpected{unknownFieldError(type, name)};
      }
      case VariableType::Metadata:
      {
        if (auto const* descriptor = findQueryVariableDescriptor(type, name); descriptor != nullptr)
        {
          return descriptor->field;
        }

        return std::unexpected{unknownFieldError(type, name)};
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
        if (auto const* descriptor = findQueryVariableDescriptor(type, name); descriptor != nullptr)
        {
          return descriptor->field;
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
      case Field::MovementId:
      case Field::ConductorId:
      case Field::EnsembleId:
      case Field::SoloistId:
      case Field::TrackNumber:
      case Field::TrackTotal:
      case Field::DiscNumber:
      case Field::DiscTotal:
      case Field::MovementNumber:
      case Field::MovementTotal:
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
      case Field::WorkId:
      case Field::MovementId:
      case Field::ConductorId:
      case Field::EnsembleId:
      case Field::SoloistId: return true;
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
    if (auto const* const descriptor = findQueryVariableDescriptor(field); descriptor != nullptr)
    {
      return descriptor->canonicalName;
    }

    return "field";
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

  std::string variableDisplayName(VariableType const type, std::string_view const name)
  {
    if (type == VariableType::Tag || type == VariableType::Custom)
    {
      return serialize(VariableExpression{.type = type, .name = std::string{name}});
    }

    auto result = std::string{};
    result.reserve(name.size() + 1);
    result.push_back(variablePrefix(type));
    result += name;
    return result;
  }

  std::string variableDisplayName(VariableExpression const& variable)
  {
    return variableDisplayName(variable.type, variable.name);
  }

  bool isHotDataRequired(AccessProfile profile)
  {
    return profile == AccessProfile::HotOnly || profile == AccessProfile::HotAndCold;
  }

  bool isColdDataRequired(AccessProfile profile)
  {
    return profile == AccessProfile::ColdOnly || profile == AccessProfile::HotAndCold;
  }

  bool hasRequiredTrackData(AccessProfile profile, library::TrackView const& track)
  {
    if (isHotDataRequired(profile) && !track.isHotValid())
    {
      return false;
    }

    if (isColdDataRequired(profile) && !track.isColdValid())
    {
      return false;
    }

    return true;
  }

  DictionaryId dictionaryFieldId(library::TrackView const& track, Field field)
  {
    switch (field)
    {
      case Field::ArtistId: return track.metadata().artistId();
      case Field::AlbumId: return track.metadata().albumId();
      case Field::GenreId: return track.metadata().genreId();
      case Field::AlbumArtistId: return track.metadata().albumArtistId();
      case Field::ComposerId: return track.metadata().composerId();
      case Field::ConductorId: return track.classical().conductorId();
      case Field::EnsembleId: return track.classical().ensembleId();
      case Field::WorkId: return track.classical().workId();
      case Field::MovementId: return track.classical().movementId();
      case Field::SoloistId: return track.classical().soloistId();
      default: return kInvalidDictionaryId;
    }
  }

  std::string_view dictionaryFieldValue(library::TrackView const& track,
                                        Field field,
                                        library::DictionaryStore const& dictionary)
  {
    auto const dictionaryId = dictionaryFieldId(track, field);

    if (dictionaryId == kInvalidDictionaryId)
    {
      return {};
    }

    return dictionary.get(dictionaryId);
  }
} // namespace ao::query
