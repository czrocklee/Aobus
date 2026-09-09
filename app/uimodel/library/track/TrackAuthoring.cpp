// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    std::string_view trimAsciiWhitespace(std::string_view value)
    {
      auto const first = value.find_first_not_of(" \t\n\r\f\v");

      if (first == std::string_view::npos)
      {
        return {};
      }

      auto const last = value.find_last_not_of(" \t\n\r\f\v");
      return value.substr(first, last - first + 1);
    }

    bool tryWriteStringPatch(TrackFieldEditValue const& value, std::optional<std::string>& optTarget)
    {
      if (auto const* str = std::get_if<std::string>(&value); str != nullptr)
      {
        optTarget = *str;
        return true;
      }

      return false;
    }

    bool tryWriteUint16Patch(TrackFieldEditValue const& value, std::optional<std::uint16_t>& optTarget)
    {
      if (auto const* val = std::get_if<std::uint16_t>(&value); val != nullptr)
      {
        optTarget = *val;
        return true;
      }

      return false;
    }
  } // namespace

  TrackFieldEditValue makeTextEditValue(std::string_view value)
  {
    return TrackFieldEditValue{std::in_place_type<std::string>, std::string{value}};
  }

  Result<TrackFieldEditValue> parseTextEditValue(std::string_view value)
  {
    return makeTextEditValue(value);
  }

  Result<TrackFieldEditValue> parseUint16EditValue(std::string_view value)
  {
    auto const trimmed = trimAsciiWhitespace(value);

    if (trimmed.empty())
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(0)};
    }

    std::uint32_t parsed = 0;
    auto const* const begin = trimmed.data();
    auto const* const end = trimmed.data() + trimmed.size();
    auto const [ptr, ec] = std::from_chars(begin, end, parsed);

    if (ec != std::errc{} || ptr != end || parsed > std::numeric_limits<std::uint16_t>::max())
    {
      return makeError(Error::Code::FormatRejected, "Enter a whole number from 0 to 65535.");
    }

    return TrackFieldEditValue{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(parsed)};
  }

  bool canWriteTrackFieldPatch(rt::TrackField field) noexcept
  {
    using F = rt::TrackField;

    switch (field)
    {
      case F::Title:
      case F::Artist:
      case F::Album:
      case F::AlbumArtist:
      case F::Genre:
      case F::Composer:
      case F::Conductor:
      case F::Ensemble:
      case F::Work:
      case F::Movement:
      case F::Soloist:
      case F::Year:
      case F::DiscNumber:
      case F::DiscTotal:
      case F::TrackNumber:
      case F::TrackTotal:
      case F::MovementNumber:
      case F::MovementTotal: return true;

      case F::Duration:
      case F::Tags:
      case F::FilePath:
      case F::Codec:
      case F::SampleRate:
      case F::Channels:
      case F::BitDepth:
      case F::Bitrate:
      case F::FileSize:
      case F::ModifiedTime:
      case F::DisplayTrackNumber:
      case F::TechnicalSummary:
      case F::Quality: return false;
    }

    return false;
  }

  bool tryWriteTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value)
  {
    using F = rt::TrackField;

    switch (field)
    {
      case F::Title: return tryWriteStringPatch(value, patch.optTitle);
      case F::Artist: return tryWriteStringPatch(value, patch.optArtist);
      case F::Album: return tryWriteStringPatch(value, patch.optAlbum);
      case F::AlbumArtist: return tryWriteStringPatch(value, patch.optAlbumArtist);
      case F::Genre: return tryWriteStringPatch(value, patch.optGenre);
      case F::Composer: return tryWriteStringPatch(value, patch.optComposer);
      case F::Conductor: return tryWriteStringPatch(value, patch.optConductor);
      case F::Ensemble: return tryWriteStringPatch(value, patch.optEnsemble);
      case F::Work: return tryWriteStringPatch(value, patch.optWork);
      case F::Movement: return tryWriteStringPatch(value, patch.optMovement);
      case F::Soloist: return tryWriteStringPatch(value, patch.optSoloist);

      case F::Year: return tryWriteUint16Patch(value, patch.optYear);
      case F::DiscNumber: return tryWriteUint16Patch(value, patch.optDiscNumber);
      case F::DiscTotal: return tryWriteUint16Patch(value, patch.optDiscTotal);
      case F::TrackNumber: return tryWriteUint16Patch(value, patch.optTrackNumber);
      case F::TrackTotal: return tryWriteUint16Patch(value, patch.optTrackTotal);
      case F::MovementNumber: return tryWriteUint16Patch(value, patch.optMovementNumber);
      case F::MovementTotal: return tryWriteUint16Patch(value, patch.optMovementTotal);

      case F::Duration:
      case F::Tags:
      case F::FilePath:
      case F::Codec:
      case F::SampleRate:
      case F::Channels:
      case F::BitDepth:
      case F::Bitrate:
      case F::FileSize:
      case F::ModifiedTime:
      case F::DisplayTrackNumber:
      case F::TechnicalSummary:
      case F::Quality: return false;
    }

    return false;
  }

  bool isProtectedInlineEditText(rt::TrackField const field,
                                 rt::TrackDetailSnapshot const& snap,
                                 std::string_view const newText,
                                 std::string_view const mixedText,
                                 bool const requireMixedField)
  {
    if (newText != mixedText)
    {
      return false;
    }

    auto const& value = rt::trackFieldArrayAt(snap.fields, field);
    return !requireMixedField || value.mixed;
  }
} // namespace ao::uimodel
