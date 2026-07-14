// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowObject.h"

#include "track/TrackFieldUi.h"
#include "track/TrackRowCache.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/rt/TrackField.h>

#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

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
        case rt::TrackField::Conductor:
        case rt::TrackField::Ensemble:
        case rt::TrackField::Work:
        case rt::TrackField::Movement:
        case rt::TrackField::Soloist: return true;

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

  ::GType TrackRowObject::objectType()
  {
    static auto const type = []
    {
      auto const objPtr = Glib::make_refptr_for_instance<TrackRowObject>(new TrackRowObject{});
      // G_OBJECT_TYPE is a GLib function-like macro with an unavoidable C cast.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
      return G_OBJECT_TYPE(objPtr->gobj());
    }();
    return type;
  }

  Glib::ustring const* TrackRowObject::stringField(rt::TrackField field) const noexcept
  {
    auto const index = static_cast<std::size_t>(field);

    if (index >= _text.size() || !isTextBackedField(field))
    {
      return nullptr;
    }

    return &_text.at(index);
  }

  bool TrackRowObject::setStringField(rt::TrackField field, Glib::ustring const& value)
  {
    auto const index = static_cast<std::size_t>(field);

    if (index >= _text.size() || !isTextBackedField(field))
    {
      return false;
    }

    _text.at(index) = value;
    return true;
  }

  void TrackRowObject::populate(Glib::ustring title,
                                Glib::ustring artist,
                                Glib::ustring album,
                                Glib::ustring albumArtist,
                                Glib::ustring genre,
                                Glib::ustring composer,
                                Glib::ustring conductor,
                                Glib::ustring ensemble,
                                Glib::ustring work,
                                Glib::ustring movement,
                                Glib::ustring soloist,
                                Glib::ustring tags,
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
    _text[static_cast<std::size_t>(rt::TrackField::Title)] = std::move(title);
    _text[static_cast<std::size_t>(rt::TrackField::Artist)] = std::move(artist);
    _text[static_cast<std::size_t>(rt::TrackField::Album)] = std::move(album);
    _text[static_cast<std::size_t>(rt::TrackField::AlbumArtist)] = std::move(albumArtist);
    _text[static_cast<std::size_t>(rt::TrackField::Genre)] = std::move(genre);
    _text[static_cast<std::size_t>(rt::TrackField::Composer)] = std::move(composer);
    _text[static_cast<std::size_t>(rt::TrackField::Conductor)] = std::move(conductor);
    _text[static_cast<std::size_t>(rt::TrackField::Ensemble)] = std::move(ensemble);
    _text[static_cast<std::size_t>(rt::TrackField::Work)] = std::move(work);
    _text[static_cast<std::size_t>(rt::TrackField::Movement)] = std::move(movement);
    _text[static_cast<std::size_t>(rt::TrackField::Soloist)] = std::move(soloist);

    _tags = std::move(tags);
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

    auto const index = static_cast<std::size_t>(field);

    if (index >= _text.size())
    {
      return nullptr;
    }

    // Text-backed fields are materialized at populate(); the stored slot is the
    // source of truth and never goes through the computed cache.
    if (isTextBackedField(field))
    {
      return &_text.at(index);
    }

    // UI-thread only. Computed field: format once into the slot and remember it
    // via the filled bit (so an empty formatter result is cached too, not re-run
    // every bind).
    if (auto const bit = std::uint32_t{1} << index; (_computedFilled & bit) == 0)
    {
      auto const* uiDef = trackFieldUiDefinition(field);
      _text.at(index) = (uiDef != nullptr && uiDef->readRowText != nullptr)
                          ? Glib::ustring{uiDef->readRowText(*this, *_provider)}
                          : Glib::ustring{};
      _computedFilled |= bit;
    }

    return &_text.at(index);
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
