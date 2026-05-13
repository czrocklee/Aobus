// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowObject.h"
#include "track/TrackRowCache.h"

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

  TrackRowObject::TrackRowObject()
    : Glib::ObjectBase{"TrackRowObject"}
    , _propertyPlaying{*this, "playing", false}
    , _propertyTitle{*this, "title", ""}
    , _propertyArtist{*this, "artist", ""}
    , _propertyAlbum{*this, "album", ""}
  {
  }

  Glib::PropertyProxy<bool> TrackRowObject::property_playing()
  {
    return _propertyPlaying.get_proxy();
  }

  bool TrackRowObject::isPlaying() const
  {
    return _propertyPlaying.get_value();
  }

  void TrackRowObject::setPlaying(bool playing)
  {
    _propertyPlaying.set_value(playing);
  }

  Glib::PropertyProxy<Glib::ustring> TrackRowObject::property_title()
  {
    return _propertyTitle.get_proxy();
  }

  void TrackRowObject::setTitle(Glib::ustring const& title)
  {
    _propertyTitle.set_value(title);
  }

  Glib::PropertyProxy<Glib::ustring> TrackRowObject::property_artist()
  {
    return _propertyArtist.get_proxy();
  }

  void TrackRowObject::setArtist(Glib::ustring const& artist)
  {
    _propertyArtist.set_value(artist);
  }

  Glib::PropertyProxy<Glib::ustring> TrackRowObject::property_album()
  {
    return _propertyAlbum.get_proxy();
  }

  void TrackRowObject::setAlbum(Glib::ustring const& album)
  {
    _propertyAlbum.set_value(album);
  }

  Glib::RefPtr<TrackRowObject> TrackRowObject::create(TrackId id, TrackRowCache const& provider)
  {
    auto obj = Glib::make_refptr_for_instance<TrackRowObject>(new TrackRowObject{}); // NOLINT(cppcoreguidelines-owning-memory)
    obj->_id = id;
    obj->_provider = &provider;
    return obj;
  }

  void TrackRowObject::populate(Glib::ustring const& title,
                          ao::DictionaryId artist,
                          ao::DictionaryId album,
                          ao::DictionaryId albumArtist,
                          ao::DictionaryId genre,
                          ao::DictionaryId composer,
                          ao::DictionaryId work,
                          Glib::ustring const& tags,
                          std::chrono::milliseconds duration,
                          std::uint16_t year,
                          std::uint16_t discNumber,
                          std::uint16_t totalDiscs,
                          std::uint16_t trackNumber,
                          std::optional<std::uint64_t> optResourceId,
                          std::uint32_t sampleRate,
                          std::uint8_t channels,
                          std::uint8_t bitDepth,
                          std::uint16_t codecId)
  {
    _propertyTitle.set_value(title);
    _propertyArtist.set_value(_provider->resolveDictionaryString(artist));
    _propertyAlbum.set_value(_provider->resolveDictionaryString(album));

    _albumArtistId = albumArtist;
    _genreId = genre;
    _composerId = composer;
    _workId = work;

    _tags = tags;
    _duration = duration;
    _year = year;
    _discNumber = discNumber;
    _totalDiscs = totalDiscs;
    _trackNumber = trackNumber;
    _optResourceId = optResourceId;

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

  Glib::ustring TrackRowObject::getColumnText(TrackColumn column) const
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

  Glib::ustring const& TrackRowObject::getDisplayNumber() const
  {
    return _displayNumberStr;
  }

  void TrackRowObject::setTags(Glib::ustring const& tags)
  {
    _tags = tags;
  }

  Glib::ustring const& TrackRowObject::getTags() const
  {
    return _tags;
  }
} // namespace ao::gtk
