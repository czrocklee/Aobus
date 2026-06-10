// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowObject.h"

#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackField.h>

#include <glibmm/objectbase.h>
#include <glibmm/propertyproxy.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ao::gtk
{
  namespace
  {
    bool isTextBackedField(rt::TrackField field)
    {
      switch (field)
      {
        case rt::TrackField::Title:
        case rt::TrackField::Artist:
        case rt::TrackField::Album:
        case rt::TrackField::AlbumArtist:
        case rt::TrackField::Genre:
        case rt::TrackField::Composer:
        case rt::TrackField::Work: return true;

        default: return false;
      }
    }
  } // namespace

  TrackRowObject::TrackRowObject()
    : Glib::ObjectBase{"TrackRowObject"}, _propertyPlaying{*this, "playing", false}
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

  Glib::RefPtr<TrackRowObject> TrackRowObject::create(TrackId id, TrackRowCache const& provider)
  {
    auto objPtr = Glib::make_refptr_for_instance<TrackRowObject>(new TrackRowObject{});
    objPtr->_id = id;
    objPtr->_provider = &provider;
    return objPtr;
  }

  Glib::ustring const* TrackRowObject::stringField(rt::TrackField field) const noexcept
  {
    auto const idx = static_cast<std::size_t>(field);

    if (idx >= _text.size() || !isTextBackedField(field))
    {
      return nullptr;
    }

    return &_text.at(idx);
  }

  bool TrackRowObject::setStringField(rt::TrackField field, Glib::ustring const& value)
  {
    auto const idx = static_cast<std::size_t>(field);

    if (idx >= _text.size() || !isTextBackedField(field))
    {
      return false;
    }

    _text.at(idx) = value;
    return true;
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
                                library::AudioCodec codec,
                                std::uint32_t bitrate,
                                std::uint64_t fileSize,
                                std::uint64_t modifiedTime,
                                library::FileStatus status)
  {
    _text[static_cast<std::size_t>(rt::TrackField::Title)] = title;
    _text[static_cast<std::size_t>(rt::TrackField::Artist)] = _provider->resolveDictionaryString(artist);
    _text[static_cast<std::size_t>(rt::TrackField::Album)] = _provider->resolveDictionaryString(album);
    _text[static_cast<std::size_t>(rt::TrackField::AlbumArtist)] = _provider->resolveDictionaryString(albumArtist);
    _text[static_cast<std::size_t>(rt::TrackField::Genre)] = _provider->resolveDictionaryString(genre);
    _text[static_cast<std::size_t>(rt::TrackField::Composer)] = _provider->resolveDictionaryString(composer);
    _text[static_cast<std::size_t>(rt::TrackField::Work)] = _provider->resolveDictionaryString(work);

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
    _codec = codec;
    _bitrate = bitrate;
    _fileSize = fileSize;
    _modifiedTime = modifiedTime;
    _status = status;
  }

  Glib::ustring TrackRowObject::fieldText(rt::TrackField field) const
  {
    if (auto const* text = stringField(field); text != nullptr)
    {
      return *text;
    }

    auto const* uiDef = trackFieldUiDefinition(field);

    if (uiDef == nullptr || uiDef->readRowText == nullptr)
    {
      static Glib::ustring const kEmpty;
      return kEmpty;
    }

    return Glib::ustring{uiDef->readRowText(*this, *_provider)};
  }

  void TrackRowObject::setYear(std::uint16_t year)
  {
    _year = year;
  }

  void TrackRowObject::setDiscNumber(std::uint16_t discNumber)
  {
    _discNumber = discNumber;
  }

  void TrackRowObject::setTotalDiscs(std::uint16_t totalDiscs)
  {
    _totalDiscs = totalDiscs;
  }

  void TrackRowObject::setTrackNumber(std::uint16_t trackNumber)
  {
    _trackNumber = trackNumber;
  }

  void TrackRowObject::setTotalTracks(std::uint16_t totalTracks)
  {
    _totalTracks = totalTracks;
  }
} // namespace ao::gtk
