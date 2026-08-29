// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <cstdint>
#include <string_view>

namespace ao::uimodel
{
  namespace
  {
    constexpr std::int32_t kWidthArtist = 150;
    constexpr std::int32_t kWidthAlbum = 200;
    constexpr std::int32_t kWidthAlbumArtist = 180;
    constexpr std::int32_t kWidthGenre = 140;
    constexpr std::int32_t kWidthYear = 80;
    constexpr std::int32_t kWidthDisc = 70;
    constexpr std::int32_t kWidthTrack = 72;
    constexpr std::int32_t kWidthTitle = 280;
    constexpr std::int32_t kWidthDuration = 84;
    constexpr std::int32_t kWidthTechnical = 80;
    constexpr std::int32_t kWidthTechnicalSummary = 180;
    constexpr std::int32_t kWidthPath = 300;
    constexpr std::int32_t kWidthTime = 130;
    constexpr std::int32_t kWidthTags = 160;
    constexpr std::int32_t kMinimumFlexibleWidth = 72;
    constexpr double kWeightTitle = 3.0;
    constexpr double kWeightPrimaryText = 2.0;
    constexpr double kWeightAlbumArtist = 1.8;
    constexpr double kWeightTags = 1.5;
    constexpr double kWeightSecondaryText = 1.2;
  } // namespace

  TrackColumnDefaults trackColumnDefaults(rt::TrackField const field) noexcept
  {
    using enum TrackColumnAlignment;
    using enum TrackColumnSizing;

    switch (field)
    {
      case rt::TrackField::Title:
        return {.width = kWidthTitle,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightTitle,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::Artist:
        return {.width = kWidthArtist,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightPrimaryText,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::Album:
        return {.width = kWidthAlbum,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightPrimaryText,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::AlbumArtist:
        return {.width = kWidthAlbumArtist,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightAlbumArtist,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::Genre:
      case rt::TrackField::Composer:
      case rt::TrackField::Conductor:
      case rt::TrackField::Ensemble:
      case rt::TrackField::Work:
      case rt::TrackField::Movement:
      case rt::TrackField::Soloist:
        return {.width = kWidthGenre,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightSecondaryText,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::Tags:
        return {.width = kWidthTags,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightTags,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::FilePath:
        return {.width = kWidthPath,
                .minimumWidth = kMinimumFlexibleWidth,
                .weight = kWeightPrimaryText,
                .sizing = Flexible,
                .alignment = Start};
      case rt::TrackField::Year: return {.width = kWidthYear, .alignment = End};
      case rt::TrackField::DiscNumber:
      case rt::TrackField::DiscTotal: return {.width = kWidthDisc, .alignment = End};
      case rt::TrackField::TrackNumber:
      case rt::TrackField::TrackTotal:
      case rt::TrackField::MovementNumber:
      case rt::TrackField::MovementTotal:
      case rt::TrackField::DisplayTrackNumber: return {.width = kWidthTrack, .alignment = End};
      case rt::TrackField::Duration: return {.width = kWidthDuration, .alignment = End};
      case rt::TrackField::SampleRate:
      case rt::TrackField::Channels:
      case rt::TrackField::BitDepth:
      case rt::TrackField::Bitrate:
      case rt::TrackField::FileSize: return {.width = kWidthTechnical, .alignment = End};
      case rt::TrackField::Codec:
      case rt::TrackField::Quality: return {.width = kWidthTechnical};
      case rt::TrackField::ModifiedTime: return {.width = kWidthTime, .alignment = End};
      case rt::TrackField::TechnicalSummary: return {.width = kWidthTechnicalSummary};
      default: return {};
    }
  }

  std::string_view trackFieldColumnTitle(i18n::MessageCatalog const& textCatalog, rt::TrackField const field)
  {
    return trackFieldLabel(textCatalog, field);
  }
} // namespace ao::uimodel
