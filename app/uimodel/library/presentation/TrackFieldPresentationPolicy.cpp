// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

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
    constexpr std::int32_t kMinimumFixedWidth = 40;
    constexpr double kWeightTitle = 3.0;
    constexpr double kWeightPrimaryText = 2.0;
    constexpr double kWeightAlbumArtist = 1.8;
    constexpr double kWeightTags = 1.5;
    constexpr double kWeightSecondaryText = 1.2;
    constexpr double kWeightFallback = 1.0;
  } // namespace

  std::int32_t defaultTrackFieldColumnWidth(rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Title: return kWidthTitle;
      case rt::TrackField::Artist: return kWidthArtist;
      case rt::TrackField::Album: return kWidthAlbum;
      case rt::TrackField::AlbumArtist: return kWidthAlbumArtist;
      case rt::TrackField::Genre:
      case rt::TrackField::Composer:
      case rt::TrackField::Conductor:
      case rt::TrackField::Ensemble:
      case rt::TrackField::Work:
      case rt::TrackField::Movement:
      case rt::TrackField::Soloist: return kWidthGenre;
      case rt::TrackField::Year: return kWidthYear;
      case rt::TrackField::DiscNumber:
      case rt::TrackField::DiscTotal: return kWidthDisc;
      case rt::TrackField::TrackNumber:
      case rt::TrackField::TrackTotal:
      case rt::TrackField::MovementNumber:
      case rt::TrackField::MovementTotal: return kWidthTrack;
      case rt::TrackField::Duration: return kWidthDuration;
      case rt::TrackField::Tags: return kWidthTags;
      case rt::TrackField::FilePath: return kWidthPath;
      case rt::TrackField::Codec:
      case rt::TrackField::SampleRate:
      case rt::TrackField::Channels:
      case rt::TrackField::BitDepth:
      case rt::TrackField::Bitrate:
      case rt::TrackField::FileSize:
      case rt::TrackField::Quality: return kWidthTechnical;
      case rt::TrackField::ModifiedTime: return kWidthTime;
      case rt::TrackField::DisplayTrackNumber: return kWidthTrack;
      case rt::TrackField::TechnicalSummary: return kWidthTechnicalSummary;
      default: return -1;
    }
  }

  std::int32_t minimumTrackFieldColumnWidth(rt::TrackField field)
  {
    return trackFieldColumnSizing(field) == TrackColumnSizing::Flexible ? kMinimumFlexibleWidth : kMinimumFixedWidth;
  }

  double defaultTrackFieldColumnWeight(rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Title: return kWeightTitle;
      case rt::TrackField::Artist:
      case rt::TrackField::Album:
      case rt::TrackField::FilePath: return kWeightPrimaryText;
      case rt::TrackField::AlbumArtist: return kWeightAlbumArtist;
      case rt::TrackField::Tags: return kWeightTags;
      case rt::TrackField::Genre:
      case rt::TrackField::Composer:
      case rt::TrackField::Conductor:
      case rt::TrackField::Ensemble:
      case rt::TrackField::Work:
      case rt::TrackField::Movement:
      case rt::TrackField::Soloist: return kWeightSecondaryText;
      default: return kWeightFallback;
    }
  }

  TrackColumnSizing trackFieldColumnSizing(rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Title:
      case rt::TrackField::Artist:
      case rt::TrackField::Album:
      case rt::TrackField::AlbumArtist:
      case rt::TrackField::Genre:
      case rt::TrackField::Composer:
      case rt::TrackField::Conductor:
      case rt::TrackField::Ensemble:
      case rt::TrackField::Work:
      case rt::TrackField::Movement:
      case rt::TrackField::Soloist:
      case rt::TrackField::Tags:
      case rt::TrackField::FilePath: return TrackColumnSizing::Flexible;
      default: return TrackColumnSizing::Fixed;
    }
  }

  TrackColumnAlignment trackFieldColumnAlignment(rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Year:
      case rt::TrackField::DiscNumber:
      case rt::TrackField::DiscTotal:
      case rt::TrackField::TrackNumber:
      case rt::TrackField::TrackTotal:
      case rt::TrackField::MovementNumber:
      case rt::TrackField::MovementTotal:
      case rt::TrackField::Duration:
      case rt::TrackField::SampleRate:
      case rt::TrackField::Channels:
      case rt::TrackField::BitDepth:
      case rt::TrackField::Bitrate:
      case rt::TrackField::FileSize:
      case rt::TrackField::ModifiedTime:
      case rt::TrackField::DisplayTrackNumber: return TrackColumnAlignment::End;
      default: return TrackColumnAlignment::Start;
    }
  }

  std::string_view trackFieldColumnTitle(rt::TrackField field)
  {
    return PresentationTextCatalog{}.trackFieldLabel(field);
  }
} // namespace ao::uimodel
