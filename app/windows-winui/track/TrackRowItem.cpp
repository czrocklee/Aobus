// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackRowItem.h"

#include "pch.h"
#include "platform/WindowsStringResources.h"
#include "track/TrackCellItem.h"

#if __has_include("TrackRowItem.g.cpp")
#include "TrackRowItem.g.cpp"
#endif

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/utility/Path.h>

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace winrt::Aobus::implementation
{
  namespace
  {
    constexpr double kDefaultTitleWidth = 320.0;
    constexpr double kDefaultMetadataWidth = 220.0;
    constexpr double kDefaultDurationWidth = 80.0;

    using TrackCellsProjection = decltype(std::declval<TrackRowItem const&>().Cells());

    static_assert(std::same_as<TrackCellsProjection,
                               Windows::Foundation::Collections::IVectorView<Windows::Foundation::IInspectable>>,
                  "TrackRowItem.Cells must expose inspectable elements for ItemsControl.ItemsSource");

    std::string trackTitle(ao::rt::TrackRow const& row)
    {
      if (!row.title.empty())
      {
        return row.title;
      }

      if (row.optUriPath)
      {
        return ao::utility::pathToUtf8(row.optUriPath->filename());
      }

      return ao::winui::formatResource("TrackFallbackFormat", row.id.raw());
    }

    std::string cellText(ao::rt::TrackRow const& row, ao::rt::TrackField const field)
    {
      using F = ao::rt::TrackField;
      using namespace ao::uimodel;

      switch (field)
      {
        case F::Title: return trackTitle(row);
        case F::Artist: return row.artist;
        case F::Album: return row.album;
        case F::AlbumArtist: return row.albumArtist;
        case F::Genre: return row.genre;
        case F::Composer: return row.composer;
        case F::Conductor: return row.conductor;
        case F::Ensemble: return row.ensemble;
        case F::Work: return row.work;
        case F::Movement: return row.movement;
        case F::Soloist: return row.soloist;
        case F::Year: return formatUint16(row.year);
        case F::DiscNumber: return formatUint16(row.discNumber);
        case F::DiscTotal: return formatUint16(row.discTotal);
        case F::TrackNumber: return formatUint16(row.trackNumber);
        case F::TrackTotal: return formatUint16(row.trackTotal);
        case F::MovementNumber: return formatUint16(row.movementNumber);
        case F::MovementTotal: return formatUint16(row.movementTotal);
        case F::Duration: return formatDuration(row.duration);
        case F::Tags: return row.tags;
        case F::FilePath: return row.optUriPath ? ao::utility::pathToGenericUtf8(*row.optUriPath) : std::string{};
        case F::Codec: return formatCodec(row.codec);
        case F::SampleRate: return formatSampleRate(row.sampleRate);
        case F::Channels: return formatChannels(row.channels);
        case F::BitDepth: return formatBitDepth(row.bitDepth);
        case F::Bitrate: return formatBitrate(row.bitrate);
        case F::FileSize: return formatFileSize(row.fileSize);
        case F::ModifiedTime: return formatTime(row.modifiedTime);
        case F::DisplayTrackNumber: return formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber);
        case F::TechnicalSummary: return formatTechnicalSummary(row.codec, row.sampleRate, row.bitDepth, row.bitrate);
        case F::Quality: return {};
      }

      return {};
    }

    void appendCell(Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable> const& cells,
                    hstring text,
                    std::string_view const fieldId,
                    double const width,
                    bool const sortable)
    {
      cells.Append(make<TrackCellItem>(std::move(text), to_hstring(std::string{fieldId}), width, sortable));
    }
  } // namespace

  TrackRowItem::TrackRowItem(std::uint32_t const index,
                             std::uint32_t const trackId,
                             std::uint32_t const coverArtId,
                             hstring title,
                             hstring artist,
                             hstring album,
                             hstring duration)
    : _displayIndex{index}
    , _index{index}
    , _trackId{trackId}
    , _coverArtId{coverArtId}
    , _title{std::move(title)}
    , _artist{std::move(artist)}
    , _album{std::move(album)}
  {
    appendCell(_cells, _title, "title", kDefaultTitleWidth, true);
    appendCell(_cells, _artist, "artist", kDefaultMetadataWidth, true);
    appendCell(_cells, _album, "album", kDefaultMetadataWidth, true);
    appendCell(_cells, std::move(duration), "duration", kDefaultDurationWidth, true);
  }

  TrackRowItem::TrackRowItem(std::uint32_t const displayIndex,
                             std::uint32_t const sourceIndex,
                             ao::rt::TrackRow const& row,
                             std::span<ao::winui::TrackColumnCellSpec const> const columns)
    : _displayIndex{displayIndex}
    , _index{sourceIndex}
    , _trackId{row.id.raw()}
    , _coverArtId{row.coverArtId.raw()}
    , _title{winrt::to_hstring(trackTitle(row))}
    , _artist{winrt::to_hstring(row.artist)}
    , _album{winrt::to_hstring(row.album)}
  {
    for (auto const& column : columns)
    {
      auto const* definition = ao::rt::trackFieldDefinition(column.field);
      appendCell(_cells,
                 to_hstring(cellText(row, column.field)),
                 ao::rt::trackFieldId(column.field),
                 column.width,
                 definition != nullptr && definition->optSortField);
    }
  }

  TrackRowItem::TrackRowItem(std::uint32_t const displayIndex,
                             std::uint32_t const sourceIndex,
                             std::uint32_t const coverArtId,
                             std::uint32_t const groupCount,
                             std::string primary,
                             std::string secondary,
                             std::string tertiary,
                             std::string coverArtMonogram)
    : _displayIndex{displayIndex}
    , _index{sourceIndex}
    , _coverArtId{coverArtId}
    , _groupCount{groupCount}
    , _isGroupHeader{true}
    , _title{winrt::to_hstring(primary)}
    , _artist{winrt::to_hstring(secondary)}
    , _album{winrt::to_hstring(tertiary)}
    , _coverArtMonogram{winrt::to_hstring(coverArtMonogram)}
  {
  }
} // namespace winrt::Aobus::implementation
