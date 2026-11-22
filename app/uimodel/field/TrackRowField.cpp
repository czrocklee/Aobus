// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/field/TrackRowField.h>

#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/utility/Path.h>

#include <optional>
#include <string>

namespace ao::uimodel
{
  std::string formatTrackRowField(i18n::MessageCatalog const& textCatalog,
                                  rt::TrackRow const& row,
                                  rt::TrackField const field,
                                  TrackRowPathFormat const pathFormat)
  {
    using F = rt::TrackField;

    switch (field)
    {
      case F::Title: return row.title;
      case F::Artist: return row.artist;
      case F::Album: return row.album;
      case F::AlbumArtist: return row.albumArtist;
      case F::Genre: return row.genre;
      case F::Composer: return row.composer;
      case F::Conductor: return formatTrackCreditSummary(textCatalog, row.conductor, row.conductorCount);
      case F::Ensemble: return formatTrackCreditSummary(textCatalog, row.ensemble, row.ensembleCount);
      case F::Work: return row.work;
      case F::Movement: return row.movement;
      case F::Soloist: return formatTrackCreditSummary(textCatalog, row.soloist, row.soloistCount);
      case F::RecordingDate: return library::formatRecordingDate(row.recordingDate);
      case F::Year: return formatUint16(row.year);
      case F::DiscNumber: return formatUint16(row.discNumber);
      case F::DiscTotal: return formatUint16(row.discTotal);
      case F::TrackNumber: return formatUint16(row.trackNumber);
      case F::TrackTotal: return formatUint16(row.trackTotal);
      case F::MovementNumber: return formatUint16(row.movementNumber);
      case F::MovementTotal: return formatUint16(row.movementTotal);
      case F::Duration: return formatDuration(row.duration);
      case F::Tags: return row.tags;
      case F::FilePath:
        if (!row.optUriPath)
        {
          return {};
        }

        return pathFormat == TrackRowPathFormat::Generic ? utility::pathToGenericUtf8(*row.optUriPath)
                                                         : utility::pathToUtf8(*row.optUriPath);
      case F::Codec: return formatCodec(row.codec);
      case F::SampleRate: return formatSampleRate(row.sampleRate);
      case F::Channels: return formatChannels(textCatalog, row.channels);
      case F::BitDepth: return formatBitDepth(row.bitDepth);
      case F::Bitrate: return formatBitrate(row.bitrate);
      case F::FileSize: return formatFileSize(row.fileSize);
      case F::ModifiedTime: return formatTime(row.optModifiedTime);
      case F::DisplayTrackNumber: return formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber);
      case F::TechnicalSummary: return formatTechnicalSummary(row.codec, row.sampleRate, row.bitDepth, row.bitrate);
      case F::Quality: return {};
    }

    return {};
  }

  std::optional<std::string> trackTitleOrFilename(rt::TrackRow const& row)
  {
    if (!row.title.empty())
    {
      return row.title;
    }

    if (row.optUriPath)
    {
      return utility::pathToUtf8(row.optUriPath->filename());
    }

    return std::nullopt;
  }
} // namespace ao::uimodel
