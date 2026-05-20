// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowObject.h"

#include "ao/Type.h"
#include "runtime/TrackField.h"
#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"

#include <glibmm/objectbase.h>
#include <glibmm/propertyproxy.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace ao::gtk
{
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
    auto obj = Glib::make_refptr_for_instance<TrackRowObject>(new TrackRowObject{});
    obj->_id = id;
    obj->_provider = &provider;
    return obj;
  }

  void TrackRowObject::populate(Glib::ustring const& title,
                                DictionaryId artist,
                                DictionaryId album,
                                DictionaryId albumArtist,
                                DictionaryId genre,
                                DictionaryId composer,
                                DictionaryId work,
                                Glib::ustring const& tags,
                                std::chrono::milliseconds duration,
                                std::uint16_t year,
                                std::uint16_t discNumber,
                                std::uint16_t totalDiscs,
                                std::uint16_t trackNumber,
                                std::uint16_t totalTracks,
                                std::optional<std::uint64_t> optResourceId,
                                std::uint32_t sampleRate,
                                std::uint8_t channels,
                                std::uint8_t bitDepth,
                                std::uint16_t codecId,
                                std::uint32_t bitrate,
                                std::uint64_t fileSize,
                                std::uint64_t modifiedTime)
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
    _totalTracks = totalTracks;
    _optResourceId = optResourceId;

    _sampleRate = sampleRate;
    _channels = channels;
    _bitDepth = bitDepth;
    _codecId = codecId;
    _bitrate = bitrate;
    _fileSize = fileSize;
    _modifiedTime = modifiedTime;
  }

  Glib::ustring TrackRowObject::getFieldText(rt::TrackField field) const
  {
    auto const* uiDef = trackFieldUiDefinition(field);

    if (uiDef == nullptr || uiDef->readRowText == nullptr)
    {
      static Glib::ustring const kEmpty;
      return kEmpty;
    }

    return Glib::ustring{uiDef->readRowText(*this, *_provider)};
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
