// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    bool writeStringPatch(TrackFieldEditValue const& value, std::optional<std::string>& optTarget)
    {
      if (auto const* str = std::get_if<std::string>(&value); str != nullptr)
      {
        optTarget = *str;
        return true;
      }

      return false;
    }

    bool writeUint16Patch(TrackFieldEditValue const& value, std::optional<std::uint16_t>& optTarget)
    {
      if (auto const* val = std::get_if<std::uint16_t>(&value); val != nullptr)
      {
        optTarget = *val;
        return true;
      }

      return false;
    }
  } // namespace

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

  bool writeTrackFieldPatch(rt::MetadataPatch& patch, rt::TrackField field, TrackFieldEditValue const& value)
  {
    using F = rt::TrackField;

    switch (field)
    {
      case F::Title: return writeStringPatch(value, patch.optTitle);
      case F::Artist: return writeStringPatch(value, patch.optArtist);
      case F::Album: return writeStringPatch(value, patch.optAlbum);
      case F::AlbumArtist: return writeStringPatch(value, patch.optAlbumArtist);
      case F::Genre: return writeStringPatch(value, patch.optGenre);
      case F::Composer: return writeStringPatch(value, patch.optComposer);
      case F::Conductor: return writeStringPatch(value, patch.optConductor);
      case F::Ensemble: return writeStringPatch(value, patch.optEnsemble);
      case F::Work: return writeStringPatch(value, patch.optWork);
      case F::Movement: return writeStringPatch(value, patch.optMovement);
      case F::Soloist: return writeStringPatch(value, patch.optSoloist);

      case F::Year: return writeUint16Patch(value, patch.optYear);
      case F::DiscNumber: return writeUint16Patch(value, patch.optDiscNumber);
      case F::DiscTotal: return writeUint16Patch(value, patch.optDiscTotal);
      case F::TrackNumber: return writeUint16Patch(value, patch.optTrackNumber);
      case F::TrackTotal: return writeUint16Patch(value, patch.optTrackTotal);
      case F::MovementNumber: return writeUint16Patch(value, patch.optMovementNumber);
      case F::MovementTotal: return writeUint16Patch(value, patch.optMovementTotal);

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
} // namespace ao::uimodel
