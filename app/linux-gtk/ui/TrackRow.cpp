// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackRow.h"
#include "TrackRowDataProvider.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace ao::gtk
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
    : Glib::ObjectBase{"TrackRow"}
    , _propertyPlaying{*this, "playing", false}
    , _propertyTitle{*this, "title", ""}
    , _propertyArtist{*this, "artist", ""}
    , _propertyAlbum{*this, "album", ""}
  {
  }

  Glib::PropertyProxy<bool> TrackRow::property_playing()
  {
    return _propertyPlaying.get_proxy();
  }

  bool TrackRow::isPlaying() const
  {
    return _propertyPlaying.get_value();
  }

  void TrackRow::setPlaying(bool playing)
  {
    _propertyPlaying.set_value(playing);
  }

  Glib::PropertyProxy<Glib::ustring> TrackRow::property_title()
  {
    return _propertyTitle.get_proxy();
  }

  void TrackRow::setTitle(Glib::ustring const& title)
  {
    _propertyTitle.set_value(title);
  }

  Glib::PropertyProxy<Glib::ustring> TrackRow::property_artist()
  {
    return _propertyArtist.get_proxy();
  }

  void TrackRow::setArtist(Glib::ustring const& artist)
  {
    _propertyArtist.set_value(artist);
  }

  Glib::PropertyProxy<Glib::ustring> TrackRow::property_album()
  {
    return _propertyAlbum.get_proxy();
  }

  void TrackRow::setAlbum(Glib::ustring const& album)
  {
    _propertyAlbum.set_value(album);
  }

  Glib::RefPtr<TrackRow> TrackRow::create(TrackId id, TrackRowDataProvider const& provider)
  {
    auto obj = Glib::make_refptr_for_instance<TrackRow>(new TrackRow()); // NOLINT(cppcoreguidelines-owning-memory)
    obj->_id = id;
    obj->_provider = &provider;
    return obj;
  }

  void TrackRow::populate(Glib::ustring title,
                          ao::DictionaryId artist,
                          ao::DictionaryId album,
                          ao::DictionaryId albumArtist,
                          ao::DictionaryId genre,
                          ao::DictionaryId composer,
                          ao::DictionaryId work,
                          Glib::ustring tags,
                          std::chrono::milliseconds duration,
                          std::uint16_t year,
                          std::uint16_t discNumber,
                          std::uint16_t totalDiscs,
                          std::uint16_t trackNumber,
                          std::optional<std::uint64_t> resourceId,
                          std::uint32_t sampleRate,
                          std::uint8_t channels,
                          std::uint8_t bitDepth,
                          std::uint16_t codecId)
  {
    _propertyTitle.set_value(std::move(title));
    _propertyArtist.set_value(_provider->resolveDictionaryString(artist));
    _propertyAlbum.set_value(_provider->resolveDictionaryString(album));

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

    _sampleRate = sampleRate;
    _channels = channels;
    _bitDepth = bitDepth;
    _codecId = codecId;

    // Pre-format numeric strings
    _yearStr = _year == 0 ? Glib::ustring{} : Glib::ustring{std::format("{}", _year)};
    _discNumberStr = _discNumber == 0 ? Glib::ustring{} : Glib::ustring{std::format("{}", _discNumber)};
    _trackNumberStr = _trackNumber == 0 ? Glib::ustring{} : Glib::ustring{std::format("{}", _trackNumber)};
    _durationStr = formatDuration(_duration);

    if (_trackNumber != 0)
    {
      if (_totalDiscs > 1 && _discNumber != 0)
      {
        _displayNumberStr = std::format("{}-{}", _discNumber, _trackNumber);
      }
      else
      {
        _displayNumberStr = std::format("{}", _trackNumber);
      }
    }
    else
    {
      _displayNumberStr.clear();
    }
  }

  Glib::ustring TrackRow::getColumnText(TrackColumn column) const
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
      case TrackColumn::Title: return _propertyTitle.get_value();
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

  void TrackRow::setTags(Glib::ustring const& tags)
  {
    _tags = tags;
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
      .title = _propertyTitle.get_value().raw(),
      .durationMs = static_cast<std::uint32_t>(_duration.count()),
      .year = _year,
      .discNumber = _discNumber,
      .trackNumber = _trackNumber,
      .trackId = _id,
    };
  }
} // namespace ao::gtk
