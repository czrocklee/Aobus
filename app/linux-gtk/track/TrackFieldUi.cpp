// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackFieldUi.h"

#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace ao::gtk
{
  namespace
  {
    // ---- Row edit value helpers ----

    TrackFieldEditValue readStringEditValue(TrackRowObject const& row, rt::TrackField field)
    {
      if (auto const* stringValue = row.stringField(field); stringValue != nullptr)
      {
        return TrackFieldEditValue{std::in_place_type<std::string>, std::string{stringValue->raw()}};
      }

      return TrackFieldEditValue{};
    }

    bool applyStringEditValue(TrackRowObject& row, TrackFieldEditValue const& value, rt::TrackField field)
    {
      if (auto const* str = std::get_if<std::string>(&value); str != nullptr)
      {
        return row.setStringField(field, *str);
      }

      return false;
    }

    template<auto Getter>
    TrackFieldEditValue readUint16Field(TrackRowObject const& row, rt::TrackField /*field*/)
    {
      return TrackFieldEditValue{std::in_place_type<std::uint16_t>, (row.*Getter)()};
    }

    template<auto Setter>
    bool applyUint16Field(TrackRowObject& row, TrackFieldEditValue const& value, rt::TrackField /*field*/)
    {
      if (auto const* val = std::get_if<std::uint16_t>(&value); val != nullptr)
      {
        (row.*Setter)(*val);
        return true;
      }

      return false;
    }

    std::string readDisplayTrackNumber(TrackRowObject const& row, TrackRowCache const& /*cache*/)
    {
      return uimodel::formatDisplayTrackNumber(row.discNumber(), row.discTotal(), row.trackNumber());
    }

    std::string readTechnicalSummary(TrackRowObject const& row, TrackRowCache const& /*cache*/)
    {
      return uimodel::formatTechnicalSummary(row.codec(), row.sampleRate(), row.bitDepth());
    }

    std::string readFilePathRowText(TrackRowObject const& row, TrackRowCache const& cache)
    {
      if (auto const optPath = cache.uriPath(row.trackId()); optPath)
      {
        return optPath->string();
      }

      return {};
    }
  } // namespace

  bool canInlineEdit(TrackFieldUiDefinition const& def)
  {
    return def.parseInlineEdit != nullptr && def.readRowEditValue != nullptr && def.applyRowEditValue != nullptr &&
           uimodel::trackFieldCanWritePatch(def.field);
  }

  namespace
  {
    using F = rt::TrackField;

    auto buildUiDefs()
    {
      return std::to_array<TrackFieldUiDefinition>({
        // ---- Metadata: text ----
        {
          .field = F::Title,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Title)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::Artist,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Artist)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::Album,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Album)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::AlbumArtist,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::AlbumArtist)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::Genre,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Genre)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::Composer,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Composer)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::Work,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Work)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        {
          .field = F::Movement,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Movement)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
        },
        // ---- Metadata: number ----
        {
          .field = F::Year,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.year()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::year>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setYear>,
        },
        {
          .field = F::DiscNumber,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.discNumber()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::discNumber>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setDiscNumber>,
        },
        {
          .field = F::DiscTotal,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.discTotal()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::discTotal>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setDiscTotal>,
        },
        {
          .field = F::TrackNumber,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.trackNumber()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::trackNumber>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setTrackNumber>,
        },
        {
          .field = F::TrackTotal,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.trackTotal()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::trackTotal>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setTrackTotal>,
        },
        {
          .field = F::MovementNumber,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.movementNumber()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::movementNumber>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setMovementNumber>,
        },
        {
          .field = F::MovementTotal,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatUint16(row.movementTotal()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::movementTotal>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setMovementTotal>,
        },
        // ---- Duration ----
        {
          .field = F::Duration,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatDuration(row.duration()); },
        },
        // ---- Tags ----
        {
          .field = F::Tags,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return row.tags().raw(); },
        },
        // ---- Technical ----
        {
          .field = F::FilePath,
          .readRowText = readFilePathRowText,
        },
        {
          .field = F::Codec,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& /*cache*/) -> std::string
          { return uimodel::formatCodec(row.codec()); },
        },
        {
          .field = F::SampleRate,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatSampleRate(row.sampleRate()); },
        },
        {
          .field = F::Channels,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatChannels(row.channels()); },
        },
        {
          .field = F::BitDepth,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatBitDepth(row.bitDepth()); },
        },
        {
          .field = F::Bitrate,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatBitrate(row.bitrate()); },
        },
        {
          .field = F::FileSize,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatFileSize(row.fileSize()); },
        },
        {
          .field = F::ModifiedTime,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::formatTime(row.modifiedTime()); },
        },
        // ---- Synthetic ----
        {
          .field = F::DisplayTrackNumber,
          .readRowText = readDisplayTrackNumber,
        },
        {
          .field = F::TechnicalSummary,
          .readRowText = readTechnicalSummary,
        },
        {
          .field = F::Quality,
          .readRowText = +[](TrackRowObject const&, TrackRowCache const&) -> std::string { return ""; },
          // Future synthetic field — no readers yet.
        },

      });
    }

    auto const& uiDefinitions()
    {
      static auto const defs = buildUiDefs();
      return defs;
    }
  } // namespace

  std::span<TrackFieldUiDefinition const> trackFieldUiDefinitions()
  {
    return uiDefinitions();
  }

  TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field)
  {
    if (auto const idx = static_cast<std::size_t>(field); idx >= rt::kTrackFieldCount)
    {
      return nullptr;
    }

    for (auto const& def : uiDefinitions())
    {
      if (def.field == field)
      {
        return &def;
      }
    }

    return nullptr;
  }
} // namespace ao::gtk
