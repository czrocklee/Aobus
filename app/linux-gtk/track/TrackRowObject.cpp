// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowObject.h"

#include "track/TrackFieldUi.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/utility/Path.h>

#include <glibmm/objectbase.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>

#include <cstddef>
#include <cstdint>
#include <string>
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
        case rt::TrackField::Soloist:
        // Resolved by the row cache when requested and never editable.
        case rt::TrackField::FilePath: return true;

        default: return false;
      }
    }
  } // namespace

  TrackRowObject::TrackRowObject(i18n::MessageCatalog textCatalog)
    : Glib::ObjectBase{"TrackRowObject"}, _textCatalog{std::move(textCatalog)}
  {
  }

  Glib::RefPtr<TrackRowObject> TrackRowObject::create(TrackId id, i18n::MessageCatalog const& textCatalog)
  {
    auto objPtr = Glib::make_refptr_for_instance<TrackRowObject>(new TrackRowObject{textCatalog});
    objPtr->_id = id;
    return objPtr;
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

  void TrackRowObject::populate(rt::TrackRow row)
  {
    AO_EXPECTS(row.id == trackId(), "Cannot populate a row with another track's metadata");

    auto const& catalog = textCatalog();
    _creditSummaries = {
      Glib::ustring{uimodel::formatTrackCreditSummary(catalog, row.conductor, row.conductorCount)},
      Glib::ustring{uimodel::formatTrackCreditSummary(catalog, row.ensemble, row.ensembleCount)},
      Glib::ustring{uimodel::formatTrackCreditSummary(catalog, row.soloist, row.soloistCount)},
    };

    _text[static_cast<std::size_t>(rt::TrackField::Title)] = Glib::ustring{std::move(row.title)};
    _text[static_cast<std::size_t>(rt::TrackField::Artist)] = Glib::ustring{std::move(row.artist)};
    _text[static_cast<std::size_t>(rt::TrackField::Album)] = Glib::ustring{std::move(row.album)};
    _text[static_cast<std::size_t>(rt::TrackField::AlbumArtist)] = Glib::ustring{std::move(row.albumArtist)};
    _text[static_cast<std::size_t>(rt::TrackField::Genre)] = Glib::ustring{std::move(row.genre)};
    _text[static_cast<std::size_t>(rt::TrackField::Composer)] = Glib::ustring{std::move(row.composer)};
    _text[static_cast<std::size_t>(rt::TrackField::Conductor)] = Glib::ustring{std::move(row.conductor)};
    _text[static_cast<std::size_t>(rt::TrackField::Ensemble)] = Glib::ustring{std::move(row.ensemble)};
    _text[static_cast<std::size_t>(rt::TrackField::Work)] = Glib::ustring{std::move(row.work)};
    _text[static_cast<std::size_t>(rt::TrackField::Movement)] = Glib::ustring{std::move(row.movement)};
    _text[static_cast<std::size_t>(rt::TrackField::Soloist)] = Glib::ustring{std::move(row.soloist)};
    setFilePathText(row);

    _tags = Glib::ustring{std::move(row.tags)};
    _duration = row.duration;
    _year = row.year;
    _recordingDate = row.recordingDate;
    _discNumber = row.discNumber;
    _discTotal = row.discTotal;
    _trackNumber = row.trackNumber;
    _trackTotal = row.trackTotal;
    _movementNumber = row.movementNumber;
    _movementTotal = row.movementTotal;

    _sampleRate = row.sampleRate;
    _channels = row.channels;
    _bitDepth = row.bitDepth;
    _codec = row.codec;
    _bitrate = row.bitrate;
    _fileSize = row.fileSize;
    _optModifiedTime = row.optModifiedTime;
    _status = row.status;

    _computedFilled = 0;
    _fieldsChanged.emit();
    _filePathChanged.emit();
  }

  void TrackRowObject::setFilePathText(rt::TrackRow const& row)
  {
    _text[static_cast<std::size_t>(rt::TrackField::FilePath)] =
      Glib::ustring{row.optUriPath ? utility::pathToUtf8(*row.optUriPath) : std::string{}};
  }

  void TrackRowObject::updateFilePath(rt::TrackRow const& row)
  {
    AO_EXPECTS(row.id == trackId(), "Cannot update a row with another track's path");
    setFilePathText(row);
    _filePathChanged.emit();
  }

  Glib::ustring const* TrackRowObject::displayText(rt::TrackField field) const
  {
    static_assert(rt::kTrackFieldCount <= 32, "_computedFilled bitmask only covers up to 32 fields");

    auto const index = static_cast<std::size_t>(field);

    if (index >= _text.size())
    {
      return nullptr;
    }

    switch (field)
    {
      case rt::TrackField::Conductor: return _creditSummaries.data();
      case rt::TrackField::Ensemble: return &_creditSummaries[1];
      case rt::TrackField::Soloist: return &_creditSummaries[2];
      default: break;
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
      _text.at(index) = (uiDef != nullptr && uiDef->readRowText != nullptr) ? Glib::ustring{uiDef->readRowText(*this)}
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

  i18n::MessageCatalog const& TrackRowObject::textCatalog() const
  {
    return _textCatalog;
  }
} // namespace ao::gtk
