// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailLines.h"

#include "PlaybackStatusFormatter.h"
#include <ao/AudioCodec.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/utility/Path.h>

#include <array>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr auto kTrackDetailFields = std::to_array<rt::TrackField>({
      rt::TrackField::Title,       rt::TrackField::Artist,   rt::TrackField::Album,       rt::TrackField::Year,
      rt::TrackField::TrackNumber, rt::TrackField::Duration, rt::TrackField::AlbumArtist, rt::TrackField::Composer,
      rt::TrackField::Conductor,   rt::TrackField::Ensemble, rt::TrackField::Soloist,     rt::TrackField::Work,
      rt::TrackField::Movement,    rt::TrackField::Genre,    rt::TrackField::Tags,        rt::TrackField::Codec,
      rt::TrackField::SampleRate,  rt::TrackField::BitDepth, rt::TrackField::Channels,    rt::TrackField::Bitrate,
      rt::TrackField::FileSize,
    });
  } // namespace

  std::span<rt::TrackField const> trackDetailFields()
  {
    return kTrackDetailFields;
  }

  std::vector<TrackDetailLine> trackDetailTechnicalLines(i18n::MessageCatalog const& textCatalog,
                                                         rt::TrackRow const& row)
  {
    auto lines = std::vector<TrackDetailLine>{};
    auto append = [&](rt::TrackField const field, std::string value)
    {
      if (!value.empty())
      {
        lines.push_back({.label = std::string{uimodel::trackFieldLabel(textCatalog, field)},
                         .value = std::move(value),
                         .kind = TrackDetailLine::Kind::Technical});
      }
    };

    if (row.codec != AudioCodec::Unknown)
    {
      append(rt::TrackField::Codec, uimodel::formatCodec(row.codec));
    }

    if (row.sampleRate != 0)
    {
      append(rt::TrackField::SampleRate, uimodel::formatSampleRate(row.sampleRate));
    }

    if (row.bitDepth != 0)
    {
      append(rt::TrackField::BitDepth, uimodel::formatBitDepth(row.bitDepth));
    }

    if (row.channels != 0)
    {
      append(rt::TrackField::Channels, uimodel::formatChannels(textCatalog, row.channels));
    }

    if (row.bitrate != 0)
    {
      append(rt::TrackField::Bitrate, uimodel::formatBitrate(row.bitrate));
    }

    if (row.fileSize != 0)
    {
      append(rt::TrackField::FileSize, uimodel::formatFileSize(row.fileSize));
    }

    return lines;
  }

  std::vector<TrackDetailLine> trackDetailLines(i18n::MessageCatalog const& textCatalog, rt::TrackRow const& row)
  {
    using Kind = TrackDetailLine::Kind;
    auto lines = std::vector<TrackDetailLine>{};
    auto append = [&](rt::TrackField const field, std::string value, Kind const kind = Kind::Metadata)
    {
      if (!value.empty())
      {
        lines.push_back({.label = std::string{uimodel::trackFieldLabel(textCatalog, field)},
                         .value = std::move(value),
                         .kind = kind});
      }
    };

    append(rt::TrackField::Title, row.title, Kind::Title);

    if (row.title.empty() && row.optUriPath)
    {
      if (auto const filename = row.optUriPath->filename(); !filename.empty())
      {
        lines.push_back({.label = std::string{i18n::requiredText(textCatalog, i18n::MessageId::TuiDetailFileName)},
                         .value = utility::pathToUtf8(filename),
                         .kind = Kind::Title});
      }
    }

    append(rt::TrackField::Artist, row.artist);
    append(rt::TrackField::Album, row.album);

    if (row.year != 0)
    {
      append(rt::TrackField::Year, std::format("{}", row.year));
    }

    if (row.trackNumber != 0)
    {
      auto number = uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber);

      if (row.trackTotal != 0)
      {
        number += std::format(" / {}", row.trackTotal);
      }

      append(rt::TrackField::TrackNumber, std::move(number));
    }

    if (row.duration.count() > 0)
    {
      append(rt::TrackField::Duration, formatDuration(row.duration));
    }

    if (row.albumArtist != row.artist)
    {
      append(rt::TrackField::AlbumArtist, row.albumArtist);
    }

    append(rt::TrackField::Composer, row.composer);
    append(rt::TrackField::Conductor, row.conductor);
    append(rt::TrackField::Ensemble, row.ensemble);
    append(rt::TrackField::Soloist, row.soloist);
    append(rt::TrackField::Work, row.work);
    append(rt::TrackField::Movement, row.movement);
    append(rt::TrackField::Genre, row.genre);
    append(rt::TrackField::Tags, row.tags, Kind::Tags);

    return lines;
  }
} // namespace ao::tui
