// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/TrackRow.h"

#include "core/model/TrackRowDataProvider.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace app::ui
{

  namespace
  {
    std::string formatDuration(std::chrono::milliseconds duration)
    {
      if (duration.count() <= 0)
      {
        return {};
      }

      auto const totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
      auto const hours = totalSeconds / 3600;
      auto const minutes = (totalSeconds % 3600) / 60;
      auto const seconds = totalSeconds % 60;

      if (hours > 0)
      {
        return std::format("{}:{}:{:02}", hours, minutes, seconds);
      }

      return std::format("{}:{:02}", minutes, seconds);
    }
  }

  TrackRow::TrackRow()
  {
  }

  Glib::RefPtr<TrackRow> TrackRow::create(TrackId id, std::shared_ptr<app::core::model::TrackRowDataProvider> provider)
  {
    auto obj = Glib::make_refptr_for_instance<TrackRow>(new TrackRow());
    obj->_id = id;
    obj->_provider = std::move(provider);
    return obj;
  }

  void TrackRow::ensureLoaded() const
  {
    if (_loaded)
    {
      return;
    }

    if (!_provider)
    {
      return;
    }

    auto optRow = _provider->getRow(_id);

    if (optRow)
    {
      _artist = std::move(optRow->artist);
      _album = std::move(optRow->album);
      _albumArtist = std::move(optRow->albumArtist);
      _genre = std::move(optRow->genre);
      _title = std::move(optRow->title);
      _tags = std::move(optRow->tags);
      _duration = optRow->duration;
      _year = optRow->year;
      _discNumber = optRow->discNumber;
      _totalDiscs = optRow->totalDiscs;
      _trackNumber = optRow->trackNumber;
      _resourceId = optRow->coverArtId ? std::optional<std::uint64_t>{optRow->coverArtId.value()} : std::nullopt;
    }
    _loaded = true;
  }

  Glib::ustring TrackRow::getArtist() const
  {
    ensureLoaded();
    return _artist;
  }

  Glib::ustring TrackRow::getAlbum() const
  {
    ensureLoaded();
    return _album;
  }

  Glib::ustring TrackRow::getTitle() const
  {
    ensureLoaded();
    return _title;
  }

  Glib::ustring TrackRow::getColumnText(TrackColumn column) const
  {
    ensureLoaded();

    switch (column)
    {
      case TrackColumn::Artist: return _artist;
      case TrackColumn::Album: return _album;
      case TrackColumn::AlbumArtist: return _albumArtist;
      case TrackColumn::Genre: return _genre;
      case TrackColumn::Year: return _year == 0 ? Glib::ustring{} : Glib::ustring{std::to_string(_year)};
      case TrackColumn::DiscNumber:
        return _discNumber == 0 ? Glib::ustring{} : Glib::ustring{std::to_string(_discNumber)};
      case TrackColumn::TrackNumber:
        return _trackNumber == 0 ? Glib::ustring{} : Glib::ustring{std::to_string(_trackNumber)};
      case TrackColumn::Title: return _title;
      case TrackColumn::Duration: return formatDuration(_duration);
      case TrackColumn::Tags: return _tags;
    }

    return {};
  }

  Glib::ustring TrackRow::getDisplayNumber() const
  {
    ensureLoaded();

    if (_trackNumber == 0)
    {
      return {};
    }

    if (_totalDiscs > 1 && _discNumber != 0)
    {
      return Glib::ustring{std::to_string(_discNumber) + "-" + std::to_string(_trackNumber)};
    }

    return Glib::ustring{std::to_string(_trackNumber)};
  }

  Glib::ustring TrackRow::getTags() const
  {
    ensureLoaded();
    return _tags;
  }

  std::chrono::milliseconds TrackRow::getDuration() const
  {
    ensureLoaded();
    return _duration;
  }

  TrackPresentationKeysView TrackRow::getPresentationKeys() const
  {
    ensureLoaded();

    return TrackPresentationKeysView{
      .artist = _artist,
      .album = _album,
      .albumArtist = _albumArtist,
      .genre = _genre,
      .title = _title,
      .durationMs = static_cast<std::uint32_t>(_duration.count()),
      .year = _year,
      .discNumber = _discNumber,
      .trackNumber = _trackNumber,
      .trackId = _id,
    };
  }

  std::uint64_t TrackRow::getResourceId() const
  {
    ensureLoaded();
    return _resourceId.value_or(0);
  }

} // namespace app::ui
