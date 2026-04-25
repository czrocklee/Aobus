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

    if (auto optRow = _provider->getRow(_id); optRow)
    {
      _artist = optRow->artist;
      _album = optRow->album;
      _albumArtist = optRow->albumArtist;
      _genre = optRow->genre;
      _composer = optRow->composer;
      _work = optRow->work;
      _title = optRow->title;
      _tags = optRow->tags;
      _duration = optRow->duration;
      _year = optRow->year;
      _discNumber = optRow->discNumber;
      _totalDiscs = optRow->totalDiscs;
      _trackNumber = optRow->trackNumber;
      _resourceId = optRow->coverArtId ? std::optional<std::uint64_t>{optRow->coverArtId.value()} : std::nullopt;

      // Pre-format numeric strings
      _yearStr = _year == 0 ? Glib::ustring{} : Glib::ustring{std::to_string(_year)};
      _discNumberStr = _discNumber == 0 ? Glib::ustring{} : Glib::ustring{std::to_string(_discNumber)};
      _trackNumberStr = _trackNumber == 0 ? Glib::ustring{} : Glib::ustring{std::to_string(_trackNumber)};
      _durationStr = formatDuration(_duration);

      if (_trackNumber != 0)
      {
        if (_totalDiscs > 1 && _discNumber != 0)
        {
          _displayNumberStr = std::to_string(_discNumber) + "-" + std::to_string(_trackNumber);
        }
        else
        {
          _displayNumberStr = std::to_string(_trackNumber);
        }
      }
      else
      {
        _displayNumberStr.clear();
      }
    }

    _loaded = true;
  }

  const Glib::ustring& TrackRow::getArtist() const
  {
    ensureLoaded();
    return _artist;
  }

  const Glib::ustring& TrackRow::getAlbum() const
  {
    ensureLoaded();
    return _album;
  }

  const Glib::ustring& TrackRow::getTitle() const
  {
    ensureLoaded();
    return _title;
  }

  const Glib::ustring& TrackRow::getColumnText(TrackColumn column) const
  {
    ensureLoaded();

    switch (column)
    {
      case TrackColumn::Artist: return _artist;
      case TrackColumn::Album: return _album;
      case TrackColumn::AlbumArtist: return _albumArtist;
      case TrackColumn::Genre: return _genre;
      case TrackColumn::Composer: return _composer;
      case TrackColumn::Work: return _work;
      case TrackColumn::Year: return _yearStr;
      case TrackColumn::DiscNumber: return _discNumberStr;
      case TrackColumn::TrackNumber: return _trackNumberStr;
      case TrackColumn::Title: return _title;
      case TrackColumn::Duration: return _durationStr;
      case TrackColumn::Tags: return _tags;
    }

    static const Glib::ustring kEmpty;
    return kEmpty;
  }

  const Glib::ustring& TrackRow::getDisplayNumber() const
  {
    ensureLoaded();
    return _displayNumberStr;
  }

  const Glib::ustring& TrackRow::getTags() const
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
      .artist = _artist.raw(),
      .album = _album.raw(),
      .albumArtist = _albumArtist.raw(),
      .genre = _genre.raw(),
      .composer = _composer.raw(),
      .work = _work.raw(),
      .title = _title.raw(),
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
