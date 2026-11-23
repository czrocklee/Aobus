// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackFieldUi.h"

#include "track/TrackRowObject.h"
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/track/TrackAuthoring.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace ao::gtk
{
  namespace
  {
    std::string readDisplayTrackNumber(TrackRowObject const& row)
    {
      return uimodel::formatDisplayTrackNumber(row.discNumber(), row.discTotal(), row.trackNumber());
    }

    std::string readTechnicalSummary(TrackRowObject const& row)
    {
      return uimodel::formatTechnicalSummary(row.codec(), row.sampleRate(), row.bitDepth(), row.bitrate());
    }
  } // namespace

  bool canInlineEdit(TrackFieldUiDefinition const& def)
  {
    return def.parseInlineEdit != nullptr && uimodel::canWriteTrackFieldPatch(def.field);
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
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Title)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::Artist,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Artist)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::Album,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Album)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::AlbumArtist,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::AlbumArtist)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::Genre,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Genre)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::Composer,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Composer)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::Conductor,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Conductor)->raw()}; },
        },
        {
          .field = F::Ensemble,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Ensemble)->raw()}; },
        },
        {
          .field = F::Soloist,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Soloist)->raw()}; },
        },
        {
          .field = F::Work,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Work)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::Movement,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::Movement)->raw()}; },
          .parseInlineEdit = uimodel::parseTextEditValue,
        },
        {
          .field = F::RecordingDate,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return library::formatRecordingDate(row.recordingDate()); },
          .parseInlineEdit = uimodel::parseRecordingDateEditValue,
        },
        // ---- Metadata: number ----
        {
          .field = F::Year,
          .readRowText = +[](TrackRowObject const& row) -> std::string { return uimodel::formatUint16(row.year()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        {
          .field = F::DiscNumber,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatUint16(row.discNumber()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        {
          .field = F::DiscTotal,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatUint16(row.discTotal()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        {
          .field = F::TrackNumber,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatUint16(row.trackNumber()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        {
          .field = F::TrackTotal,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatUint16(row.trackTotal()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        {
          .field = F::MovementNumber,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatUint16(row.movementNumber()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        {
          .field = F::MovementTotal,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatUint16(row.movementTotal()); },
          .parseInlineEdit = uimodel::parseUint16EditValue,
        },
        // ---- Duration ----
        {
          .field = F::Duration,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatDuration(row.duration()); },
        },
        // ---- Tags ----
        {
          .field = F::Tags,
          .readRowText = +[](TrackRowObject const& row) -> std::string { return row.tags().raw(); },
        },
        // ---- Technical ----
        {
          .field = F::FilePath,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return std::string{row.stringField(rt::TrackField::FilePath)->raw()}; },
        },
        {
          .field = F::Codec,
          .readRowText = +[](TrackRowObject const& row) -> std::string { return uimodel::formatCodec(row.codec()); },
        },
        {
          .field = F::SampleRate,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatSampleRate(row.sampleRate()); },
        },
        {
          .field = F::Channels,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatChannels(row.textCatalog(), row.channels()); },
        },
        {
          .field = F::BitDepth,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatBitDepth(row.bitDepth()); },
        },
        {
          .field = F::Bitrate,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatBitrate(row.bitrate()); },
        },
        {
          .field = F::FileSize,
          .readRowText = +[](TrackRowObject const& row) -> std::string
          { return uimodel::formatFileSize(row.fileSize()); },
        },
        {
          .field = F::ModifiedTime,
          .readRowText = +[](TrackRowObject const& row) -> std::string
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
          .readRowText = +[](TrackRowObject const&) -> std::string { return ""; },
          // Future synthetic field — no readers yet.
        },

      });
    }

    auto const& uiDefinitions()
    {
      static auto const kDefs = buildUiDefs();
      return kDefs;
    }
  } // namespace

  std::span<TrackFieldUiDefinition const> trackFieldUiDefinitions()
  {
    return uiDefinitions();
  }

  TrackFieldUiDefinition const* trackFieldUiDefinition(rt::TrackField field)
  {
    if (auto const index = static_cast<std::size_t>(field); index >= rt::kTrackFieldCount)
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
