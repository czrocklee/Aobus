// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowObject.h"

#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"
#include <ao/AudioCodec.h>
#include <ao/Type.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackField.h>

#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

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
        case rt::TrackField::Work:
        case rt::TrackField::Movement: return true;

        default: return false;
      }
    }
  } // namespace

  TrackRowObject::TrackRowObject()
    : Glib::ObjectBase{"TrackRowObject"}
  {
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
                                Glib::ustring const& artist,
                                Glib::ustring const& album,
                                Glib::ustring const& albumArtist,
                                Glib::ustring const& genre,
                                Glib::ustring const& composer,
                                Glib::ustring const& work,
                                Glib::ustring const& movement,
                                Glib::ustring const& tags,
                                std::chrono::milliseconds duration,
                                std::uint16_t year,
                                std::uint16_t discNumber,
                                std::uint16_t discTotal,
                                std::uint16_t trackNumber,
                                std::uint16_t trackTotal,
                                std::uint16_t movementNumber,
                                std::uint16_t movementTotal,
                                ResourceId resourceId,
                                std::uint32_t sampleRate,
                                std::uint8_t channels,
                                std::uint8_t bitDepth,
                                AudioCodec codec,
                                std::uint32_t bitrate,
                                std::uint64_t fileSize,
                                std::uint64_t modifiedTime,
                                library::FileStatus status)
  {
    _text[static_cast<std::size_t>(rt::TrackField::Title)] = title;
    _text[static_cast<std::size_t>(rt::TrackField::Artist)] = artist;
    _text[static_cast<std::size_t>(rt::TrackField::Album)] = album;
    _text[static_cast<std::size_t>(rt::TrackField::AlbumArtist)] = albumArtist;
    _text[static_cast<std::size_t>(rt::TrackField::Genre)] = genre;
    _text[static_cast<std::size_t>(rt::TrackField::Composer)] = composer;
    _text[static_cast<std::size_t>(rt::TrackField::Work)] = work;
    _text[static_cast<std::size_t>(rt::TrackField::Movement)] = movement;

    _tags = tags;
    _duration = duration;
    _year = year;
    _discNumber = discNumber;
    _discTotal = discTotal;
    _trackNumber = trackNumber;
    _trackTotal = trackTotal;
    _movementNumber = movementNumber;
    _movementTotal = movementTotal;
    _resourceId = resourceId;

    _sampleRate = sampleRate;
    _channels = channels;
    _bitDepth = bitDepth;
    _codec = codec;
    _bitrate = bitrate;
    _fileSize = fileSize;
    _modifiedTime = modifiedTime;
    _status = status;

    // Fresh metadata: drop any computed strings memoized from a prior populate
    // (rows are re-materialized in place after an invalidate).
    invalidateComputedCache();
  }

  Glib::ustring const* TrackRowObject::displayText(rt::TrackField field) const
  {
    static_assert(rt::kTrackFieldCount <= 32, "_computedFilled bitmask only covers up to 32 fields");

    auto const idx = static_cast<std::size_t>(field);

    if (idx >= _text.size())
    {
      return nullptr;
    }

    // Text-backed fields are materialized at populate(); the stored slot is the
    // source of truth and never goes through the computed cache.
    if (isTextBackedField(field))
    {
      return &_text.at(idx);
    }

    // UI-thread only. Computed field: format once into the slot and remember it
    // via the filled bit (so an empty formatter result is cached too, not re-run
    // every bind).
    if (auto const bit = std::uint32_t{1} << idx; (_computedFilled & bit) == 0)
    {
      auto const* uiDef = trackFieldUiDefinition(field);
      _text.at(idx) = (uiDef != nullptr && uiDef->readRowText != nullptr)
                        ? Glib::ustring{uiDef->readRowText(*this, *_provider)}
                        : Glib::ustring{};
      _computedFilled |= bit;
    }

    return &_text.at(idx);
  }

  Glib::ustring TrackRowObject::fieldText(rt::TrackField field) const
  {
    if (auto const* const text = displayText(field); text != nullptr)
    {
      return *text;
    }

    static Glib::ustring const kEmpty;
    return kEmpty;
  }

  void TrackRowObject::setYear(std::uint16_t year)
  {
    _year = year;
    invalidateComputedCache();
  }

  void TrackRowObject::setDiscNumber(std::uint16_t discNumber)
  {
    _discNumber = discNumber;
    invalidateComputedCache();
  }

  void TrackRowObject::setDiscTotal(std::uint16_t discTotal)
  {
    _discTotal = discTotal;
    invalidateComputedCache();
  }

  void TrackRowObject::setTrackNumber(std::uint16_t trackNumber)
  {
    _trackNumber = trackNumber;
    invalidateComputedCache();
  }

  void TrackRowObject::setTrackTotal(std::uint16_t trackTotal)
  {
    _trackTotal = trackTotal;
    invalidateComputedCache();
  }

  void TrackRowObject::setMovementNumber(std::uint16_t movementNumber)
  {
    _movementNumber = movementNumber;
    invalidateComputedCache();
  }

  void TrackRowObject::setMovementTotal(std::uint16_t movementTotal)
  {
    _movementTotal = movementTotal;
    invalidateComputedCache();
  }
} // namespace ao::gtk
