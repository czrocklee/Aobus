// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackFieldUi.h"

#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::gtk
{
  namespace
  {
    // ---- Formatting helpers ----

    constexpr std::int32_t kWidthArtist = 150;
    constexpr std::int32_t kWidthAlbum = 200;
    constexpr std::int32_t kWidthAlbumArtist = 180;
    constexpr std::int32_t kWidthGenre = 140;
    constexpr std::int32_t kWidthYear = 80;
    constexpr std::int32_t kWidthDisc = 70;
    constexpr std::int32_t kWidthTrack = 72;
    constexpr std::int32_t kWidthTitle = 280;
    constexpr std::int32_t kWidthDuration = 84;
    constexpr std::int32_t kWidthTechnical = 80;
    constexpr std::int32_t kWidthTechnicalSummary = 180;
    constexpr std::int32_t kWidthPath = 300;
    constexpr std::int32_t kWidthTime = 130;
    constexpr std::int32_t kWidthTags = 160;

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
      auto const disc = row.discNumber();
      auto const discTotal = row.discTotal();
      auto const track = row.trackNumber();

      if (track == 0)
      {
        return {};
      }

      if (discTotal > 1 && disc != 0)
      {
        return std::format("{}-{}", disc, track);
      }

      return std::format("{}", track);
    }

    std::string readTechnicalSummary(TrackRowObject const& row, TrackRowCache const& /*cache*/)
    {
      auto const codec = uimodel::track::formatCodec(row.codec());
      auto const rate = uimodel::track::formatSampleRateCompact(row.sampleRate());
      auto const depth = std::format("{}-bit", row.bitDepth());

      if (codec.empty())
      {
        return std::format("{} \u00b7 {}", rate, depth);
      }

      return std::format("{} \u00b7 {} \u00b7 {}", codec, rate, depth);
    }

    std::string formatDurationValue(TrackFieldRawValue const& raw)
    {
      if (auto const* duration = std::get_if<rt::TrackFieldDuration>(&raw); duration != nullptr)
      {
        return uimodel::track::formatDuration(*duration);
      }

      return {};
    }

    std::string readFilePathRowText(TrackRowObject const& row, TrackRowCache const& cache)
    {
      if (auto const optPath = cache.uriPath(row.trackId()); optPath)
      {
        return optPath->string();
      }

      return {};
    }

    // ---- Patch writers ----

    void writeStrPatch(TrackFieldEditContext const& ctx, std::optional<std::string>& optTarget)
    {
      if (auto const* str = std::get_if<std::string>(&ctx.value); str != nullptr)
      {
        optTarget = *str;
      }
    }

    void writeUint16Patch(TrackFieldEditContext const& ctx, std::optional<std::uint16_t>& optTarget)
    {
      if (auto const* val = std::get_if<std::uint16_t>(&ctx.value); val != nullptr)
      {
        optTarget = *val;
      }
    }

    void writeTitlePatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optTitle);
    }

    void writeArtistPatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optArtist);
    }

    void writeAlbumPatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optAlbum);
    }

    void writeAlbumArtistPatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optAlbumArtist);
    }

    void writeGenrePatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optGenre);
    }

    void writeComposerPatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optComposer);
    }

    void writeWorkPatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optWork);
    }

    void writeMovementPatch(TrackFieldEditContext const& ctx)
    {
      writeStrPatch(ctx, ctx.patch.optMovement);
    }

    void writeYearPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optYear);
    }

    void writeDiscNumberPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optDiscNumber);
    }

    void writeDiscTotalPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optDiscTotal);
    }

    void writeTrackNumberPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optTrackNumber);
    }

    void writeTrackTotalPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optTrackTotal);
    }

    void writeMovementNumberPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optMovementNumber);
    }

    void writeMovementTotalPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optMovementTotal);
    }
  } // namespace

  bool canInlineEdit(TrackFieldUiDefinition const& def)
  {
    return def.parseInlineEdit != nullptr && def.readRowEditValue != nullptr && def.applyRowEditValue != nullptr &&
           def.writePatch != nullptr;
  }

  namespace
  {
    using F = rt::TrackField;

    // fmt helpers for conciseness
    auto const readStr = +[](TrackFieldRawValue const& raw) -> std::string
    {
      if (auto const* str = std::get_if<std::string>(&raw); str != nullptr)
      {
        return *str;
      }

      return {};
    };

    auto const readUint16 = +[](TrackFieldRawValue const& raw) -> std::string
    {
      if (auto const* val = std::get_if<std::uint16_t>(&raw); val != nullptr)
      {
        return uimodel::track::formatUint16(*val);
      }

      return {};
    };

    // Struct field order: field, readRowText, formatValue, parseInlineEdit, writePatch.
    auto buildUiDefs()
    {
      return std::to_array<TrackFieldUiDefinition>({
        // ---- Metadata: text ----
        {
          .field = F::Title,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Title)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeTitlePatch,
        },
        {
          .field = F::Artist,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Artist)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeArtistPatch,
        },
        {
          .field = F::Album,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Album)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeAlbumPatch,
        },
        {
          .field = F::AlbumArtist,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::AlbumArtist)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeAlbumArtistPatch,
        },
        {
          .field = F::Genre,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Genre)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeGenrePatch,
        },
        {
          .field = F::Composer,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Composer)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeComposerPatch,
        },
        {
          .field = F::Work,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Work)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeWorkPatch,
        },
        {
          .field = F::Movement,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return std::string{row.stringField(rt::TrackField::Movement)->raw()}; },
          .formatValue = readStr,
          .parseInlineEdit = uimodel::track::parseTextEditValue,
          .readRowEditValue = readStringEditValue,
          .applyRowEditValue = applyStringEditValue,
          .writePatch = writeMovementPatch,
        },
        // ---- Metadata: number ----
        {
          .field = F::Year,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.year()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::year>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setYear>,
          .writePatch = writeYearPatch,
        },
        {
          .field = F::DiscNumber,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.discNumber()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::discNumber>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setDiscNumber>,
          .writePatch = writeDiscNumberPatch,
        },
        {
          .field = F::DiscTotal,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.discTotal()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::discTotal>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setDiscTotal>,
          .writePatch = writeDiscTotalPatch,
        },
        {
          .field = F::TrackNumber,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.trackNumber()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::trackNumber>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setTrackNumber>,
          .writePatch = writeTrackNumberPatch,
        },
        {
          .field = F::TrackTotal,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.trackTotal()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::trackTotal>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setTrackTotal>,
          .writePatch = writeTrackTotalPatch,
        },
        {
          .field = F::MovementNumber,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.movementNumber()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::movementNumber>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setMovementNumber>,
          .writePatch = writeMovementNumberPatch,
        },
        {
          .field = F::MovementTotal,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatUint16(row.movementTotal()); },
          .formatValue = readUint16,
          .parseInlineEdit = uimodel::track::parseUint16EditValue,
          .readRowEditValue = readUint16Field<&TrackRowObject::movementTotal>,
          .applyRowEditValue = applyUint16Field<&TrackRowObject::setMovementTotal>,
          .writePatch = writeMovementTotalPatch,
        },
        // ---- Duration ----
        {
          .field = F::Duration,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatDuration(row.duration()); },
          .formatValue = formatDurationValue,
        },
        // ---- Tags ----
        {
          .field = F::Tags,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return row.tags().raw(); },
          .formatValue = readStr,
        },
        // ---- Technical ----
        {
          .field = F::FilePath,
          .readRowText = readFilePathRowText,
          .formatValue = readStr,
        },
        {
          .field = F::Codec,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& /*cache*/) -> std::string
          { return uimodel::track::formatCodec(row.codec()); },
          .formatValue = readStr,
        },
        {
          .field = F::SampleRate,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatSampleRate(row.sampleRate()); },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw); val != nullptr)
            {
              return uimodel::track::formatSampleRate(*val);
            }

            return {};
          },
        },
        {
          .field = F::Channels,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatChannels(row.channels()); },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw); val != nullptr)
            {
              return uimodel::track::formatChannels(static_cast<std::uint8_t>(*val));
            }

            return {};
          },
        },
        {
          .field = F::BitDepth,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatBitDepth(row.bitDepth()); },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw); val != nullptr)
            {
              return uimodel::track::formatBitDepth(static_cast<std::uint8_t>(*val));
            }

            return {};
          },
        },
        {
          .field = F::Bitrate,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatBitrate(row.bitrate()); },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw); val != nullptr)
            {
              return uimodel::track::formatBitrate(*val);
            }

            return {};
          },
        },
        {
          .field = F::FileSize,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatFileSize(row.fileSize()); },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint64_t>(&raw); val != nullptr)
            {
              return uimodel::track::formatFileSize(*val);
            }

            return {};
          },
        },
        {
          .field = F::ModifiedTime,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return uimodel::track::formatTime(row.modifiedTime()); },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint64_t>(&raw); val != nullptr)
            {
              return uimodel::track::formatTime(*val);
            }

            return {};
          },
        },
        // ---- Synthetic ----
        {
          .field = F::DisplayTrackNumber,
          .readRowText = readDisplayTrackNumber,
          .formatValue = readStr,
        },
        {
          .field = F::TechnicalSummary,
          .readRowText = readTechnicalSummary,
          .formatValue = readStr,
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

  std::int32_t defaultWidthForField(rt::TrackField field)
  {
    switch (field)
    {
      case rt::TrackField::Title: return kWidthTitle;
      case rt::TrackField::Artist: return kWidthArtist;
      case rt::TrackField::Album: return kWidthAlbum;
      case rt::TrackField::AlbumArtist: return kWidthAlbumArtist;
      case rt::TrackField::Genre:
      case rt::TrackField::Composer:
      case rt::TrackField::Work:
      case rt::TrackField::Movement: return kWidthGenre;
      case rt::TrackField::Year: return kWidthYear;
      case rt::TrackField::DiscNumber: return kWidthDisc;
      case rt::TrackField::TrackNumber:
      case rt::TrackField::MovementNumber:
      case rt::TrackField::MovementTotal: return kWidthTrack;
      case rt::TrackField::Duration: return kWidthDuration;
      case rt::TrackField::Tags: return kWidthTags;
      case rt::TrackField::FilePath: return kWidthPath;
      case rt::TrackField::Codec:
      case rt::TrackField::SampleRate:
      case rt::TrackField::Channels:
      case rt::TrackField::BitDepth:
      case rt::TrackField::Bitrate:
      case rt::TrackField::FileSize:
      case rt::TrackField::Quality: return kWidthTechnical;
      case rt::TrackField::ModifiedTime: return kWidthTime;
      case rt::TrackField::DisplayTrackNumber: return kWidthTrack;
      case rt::TrackField::TechnicalSummary: return kWidthTechnicalSummary;
      default: return -1;
    }
  }

  bool fieldIsVisibleByDefault(rt::TrackField field)
  {
    return std::ranges::contains(rt::defaultTrackPresentationSpec().visibleFields, field);
  }

  std::string_view fieldColumnTitle(rt::TrackField field)
  {
    if (auto const* rtDef = rt::trackFieldDefinition(field); rtDef != nullptr)
    {
      return rtDef->label;
    }

    return {};
  }

  std::optional<rt::TrackField> redundantFieldToColumn(rt::TrackSortField sortField)
  {
    for (auto const& def : rt::trackFieldDefinitions())
    {
      if (def.optSortField == sortField && def.groupable)
      {
        return def.field;
      }
    }

    return std::nullopt;
  }
} // namespace ao::gtk
