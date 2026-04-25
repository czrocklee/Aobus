// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/TrackRow.h"

#include "platform/linux/ui/TrackRowDataProvider.h"

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

  Glib::RefPtr<TrackRow> TrackRow::create(TrackId id, TrackRowDataProvider const& provider)
  {
    auto obj = Glib::make_refptr_for_instance<TrackRow>(new TrackRow());
    obj->_id = id;
    obj->_provider = &provider;
    return obj;
  }

  void TrackRow::populate(Glib::ustring title,
                          rs::core::DictionaryId artist,
                          rs::core::DictionaryId album,
                          rs::core::DictionaryId albumArtist,
                          rs::core::DictionaryId genre,
                          rs::core::DictionaryId composer,
                          rs::core::DictionaryId work,
                          Glib::ustring tags,
                          std::chrono::milliseconds duration,
                          std::uint16_t year,
                          std::uint16_t discNumber,
                          std::uint16_t totalDiscs,
                          std::uint16_t trackNumber,
                          std::optional<std::uint64_t> resourceId)
  {
    _title = std::move(title);
    _artistId = artist;
    _albumId = album;
    _albumArtistId = albumArtist;
    _genreId = genre;
    _composerId = composer;
    _workId = work;

    _tags = std::move(tags);
    _duration = duration;
    _year = year;
    _discNumber = discNumber;
    _totalDiscs = totalDiscs;
    _trackNumber = trackNumber;
    _resourceId = resourceId;

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

  Glib::ustring const& TrackRow::getArtist() const
  {
    return _provider->resolveDictionaryString(_artistId);
  }

  Glib::ustring const& TrackRow::getAlbum() const
  {
    return _provider->resolveDictionaryString(_albumId);
  }

  Glib::ustring const& TrackRow::getColumnText(TrackColumn column) const
  {
    switch (column)
    {
      case TrackColumn::Artist: return getArtist();
      case TrackColumn::Album: return getAlbum();
      case TrackColumn::AlbumArtist: return _provider->resolveDictionaryString(_albumArtistId);
      case TrackColumn::Genre: return _provider->resolveDictionaryString(_genreId);
      case TrackColumn::Composer: return _provider->resolveDictionaryString(_composerId);
      case TrackColumn::Work: return _provider->resolveDictionaryString(_workId);
      case TrackColumn::Year: return _yearStr;
      case TrackColumn::DiscNumber: return _discNumberStr;
      case TrackColumn::TrackNumber: return _trackNumberStr;
      case TrackColumn::Title: return _title;
      case TrackColumn::Duration: return _durationStr;
      case TrackColumn::Tags: return _tags;
    }

    static Glib::ustring const kEmpty;
    return kEmpty;
  }

  Glib::ustring const& TrackRow::getDisplayNumber() const
  {
    return _displayNumberStr;
  }

  Glib::ustring const& TrackRow::getTags() const
  {
    return _tags;
  }

  TrackPresentationKeysView TrackRow::getPresentationKeys() const
  {
    return TrackPresentationKeysView{
      .artist = getArtist().raw(),
      .album = getAlbum().raw(),
      .albumArtist = _provider->resolveDictionaryString(_albumArtistId).raw(),
      .genre = _provider->resolveDictionaryString(_genreId).raw(),
      .composer = _provider->resolveDictionaryString(_composerId).raw(),
      .work = _provider->resolveDictionaryString(_workId).raw(),
      .title = _title.raw(),
      .durationMs = static_cast<std::uint32_t>(_duration.count()),
      .year = _year,
      .discNumber = _discNumber,
      .trackNumber = _trackNumber,
      .trackId = _id,
    };
  }

} // namespace app::ui
