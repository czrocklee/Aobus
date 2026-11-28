// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <ao/Error.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/utility/String.h>

#include <charconv>
#include <concepts>
#include <cstdint>
#include <expected>
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
    template<typename Visitor>
    bool tryVisitScalarPatchMember(rt::TrackField field, Visitor visit)
    {
      using F = rt::TrackField;
      using Patch = rt::MetadataPatch;

      switch (field)
      {
        case F::Title: return visit(&Patch::optTitle);
        case F::Artist: return visit(&Patch::optArtist);
        case F::Album: return visit(&Patch::optAlbum);
        case F::AlbumArtist: return visit(&Patch::optAlbumArtist);
        case F::Genre: return visit(&Patch::optGenre);
        case F::Composer: return visit(&Patch::optComposer);
        case F::Work: return visit(&Patch::optWork);
        case F::Movement: return visit(&Patch::optMovement);
        case F::Year: return visit(&Patch::optYear);
        case F::DiscNumber: return visit(&Patch::optDiscNumber);
        case F::DiscTotal: return visit(&Patch::optDiscTotal);
        case F::TrackNumber: return visit(&Patch::optTrackNumber);
        case F::TrackTotal: return visit(&Patch::optTrackTotal);
        case F::MovementNumber: return visit(&Patch::optMovementNumber);
        case F::MovementTotal: return visit(&Patch::optMovementTotal);
        case F::RecordingDate: return visit(&Patch::optRecordingDate);

        case F::Conductor:
        case F::Ensemble:
        case F::Soloist:
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
    auto const trimmed = utility::trim(value);

    if (trimmed.empty())
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, std::uint16_t{0}};
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

  Result<TrackFieldEditValue> parseRecordingDateEditValue(std::string_view value)
  {
    auto const trimmed = utility::trim(value);

    if (trimmed.empty())
    {
      return TrackFieldEditValue{library::RecordingDate{}};
    }

    auto dateRes = library::parseRecordingDate(trimmed);

    if (!dateRes)
    {
      return std::unexpected{dateRes.error()};
    }

    return TrackFieldEditValue{*dateRes};
  }

  bool isEmptyTrackFieldEdit(TrackFieldEditValue const& value) noexcept
  {
    if (auto const* text = std::get_if<std::string>(&value); text != nullptr)
    {
      return text->empty();
    }

    if (auto const* number = std::get_if<std::uint16_t>(&value); number != nullptr)
    {
      return *number == 0;
    }

    if (auto const* date = std::get_if<library::RecordingDate>(&value); date != nullptr)
    {
      return !date->isPresent();
    }

    return true;
  }

  bool canWriteTrackFieldPatch(rt::TrackField field) noexcept
  {
    return tryVisitScalarPatchMember(field, [](auto) { return true; });
  }

  bool tryWriteTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value)
  {
    auto const tryWrite = [&patch, &value]<typename Value>(std::optional<Value> rt::MetadataPatch::* member)
    {
      auto const* edit = std::get_if<Value>(&value);

      if (edit == nullptr)
      {
        return false;
      }

      if constexpr (std::same_as<Value, library::RecordingDate>)
      {
        if (!edit->isValid())
        {
          return false;
        }
      }

      patch.*member = *edit;
      return true;
    };
    return tryVisitScalarPatchMember(field, tryWrite);
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
