// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackFieldUi.h"

#include "runtime/TrackField.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackView.h>

#include <array>
#include <chrono>
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
    using namespace detail;

    // ---- Formatting helpers ----

    constexpr std::uint32_t kBitsPerByte = 8;
    constexpr std::uint32_t kKilo = 1000;
    constexpr double kKiloD = 1000.0;

    constexpr std::int32_t kWidthArtist = 150;
    constexpr std::int32_t kWidthAlbum = 200;
    constexpr std::int32_t kWidthAlbumArtist = 180;
    constexpr std::int32_t kWidthGenre = 140;
    constexpr std::int32_t kWidthComposer = 140;
    constexpr std::int32_t kWidthWork = 140;
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

    std::string formatDuration(Duration duration)
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

    std::string formatUint16(std::uint16_t value)
    {
      return value == 0 ? std::string{} : std::format("{}", value);
    }

    std::string formatFileSize(std::uint64_t fileSize)
    {
      if (fileSize == 0)
      {
        return {};
      }

      constexpr std::uint64_t kKB = 1024;
      constexpr std::uint64_t kMB = kKB * 1024;
      constexpr std::uint64_t kGB = kMB * 1024;

      if (fileSize >= kGB)
      {
        return std::format("{:.1f} GB", static_cast<double>(fileSize) / kGB);
      }

      if (fileSize >= kMB)
      {
        return std::format("{:.1f} MB", static_cast<double>(fileSize) / kMB);
      }

      return std::format("{:.1f} KB", static_cast<double>(fileSize) / kKB);
    }

    std::string formatTime(std::uint64_t mtime)
    {
      if (mtime == 0)
      {
        return {};
      }

      auto const sysTime = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(mtime));

      return std::format("{:%Y-%m-%d %H:%M}", sysTime);
    }

    std::string formatSampleRate(std::uint32_t sampleRate)
    {
      if (sampleRate == 0)
      {
        return {};
      }

      return std::format("{} Hz", sampleRate);
    }

    std::string formatSampleRateCompact(std::uint32_t sampleRate)
    {
      if (sampleRate == 0)
      {
        return {};
      }

      if (sampleRate % kKilo == 0)
      {
        return std::format("{} kHz", sampleRate / kKilo);
      }

      return std::format("{:.4g} kHz", static_cast<double>(sampleRate) / kKiloD);
    }

    std::string formatBitrate(std::uint32_t bitrate)
    {
      if (bitrate == 0)
      {
        return {};
      }

      return std::format("{} kbps", bitrate / kKilo);
    }

    std::string formatChannels(std::uint8_t channels)
    {
      if (channels == 0)
      {
        return {};
      }

      if (channels == 1)
      {
        return "Mono";
      }

      if (channels == 2)
      {
        return "Stereo";
      }

      return std::format("{} channels", channels);
    }

    std::string formatBitDepth(std::uint8_t bitDepth)
    {
      if (bitDepth == 0)
      {
        return {};
      }

      return std::format("{}-bit", bitDepth);
    }

    std::string formatCodec(std::uint16_t codecId, library::DictionaryStore const& dict)
    {
      if (codecId == 0)
      {
        return {};
      }

      return std::string{dict.getOrDefault(DictionaryId{codecId})};
    }

    std::string_view resolve(library::DictionaryStore const& dict, DictionaryId id)
    {
      if (id.raw() == 0)
      {
        return {};
      }

      return dict.getOrDefault(id);
    }

    std::string readDisplayTrackNumber(TrackRowObject const& row, TrackRowCache const& /*cache*/)
    {
      auto const disc = row.getDiscNumber();
      auto const totalDiscs = row.getTotalDiscs();
      auto const track = row.getTrackNumber();

      if (track == 0)
      {
        return {};
      }

      if (totalDiscs > 1 && disc != 0)
      {
        return std::format("{}-{}", disc, track);
      }

      return std::format("{}", track);
    }

    std::string readTechnicalSummary(TrackRowObject const& row, TrackRowCache const& cache)
    {
      auto const& dict = cache.dictionary();
      auto const codec = formatCodec(row.getCodecId(), dict);
      auto const rate = formatSampleRateCompact(row.getSampleRate());
      auto const depth = std::format("{}-bit", row.getBitDepth());

      if (codec.empty())
      {
        return std::format("{} \u00b7 {}", rate, depth);
      }

      return std::format("{} \u00b7 {} \u00b7 {}", codec, rate, depth);
    }

    std::string formatDurationValue(TrackFieldRawValue const& raw)
    {
      if (auto const* dur = std::get_if<Duration>(&raw))
      {
        return formatDuration(*dur);
      }

      return {};
    }

    TrackFieldRawValue readTagsViewRawValue(library::TrackView const& view,
                                            library::DictionaryStore const& dict,
                                            library::FileManifestStore::Reader const* /*unused*/)
    {
      auto const tags = view.tags();
      auto result = std::string{};

      for (auto idx = std::uint8_t{0}; idx < tags.count(); ++idx)
      {
        if (idx > 0)
        {
          result += ", ";
        }

        result += dict.getOrDefault(tags.id(idx), "?");
      }

      return TrackFieldRawValue{std::in_place_type<std::string>, std::move(result)};
    }

    std::string readFilePathRowText(TrackRowObject const& row, TrackRowCache const& cache)
    {
      if (auto const optPath = cache.getUriPath(row.getTrackId()))
      {
        return optPath->string();
      }

      return {};
    }

    // ---- Patch writers ----

    void writeStrPatch(TrackFieldEditContext const& ctx, std::optional<std::string>& optTarget)
    {
      if (auto const* str = std::get_if<std::string>(&ctx.value))
      {
        optTarget = *str;
      }
    }

    void writeUint16Patch(TrackFieldEditContext const& ctx, std::optional<std::uint16_t>& optTarget)
    {
      if (auto const* val = std::get_if<std::uint16_t>(&ctx.value))
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

    void writeYearPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optYear);
    }

    void writeDiscNumberPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optDiscNumber);
    }

    void writeTotalDiscsPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optTotalDiscs);
    }

    void writeTrackNumberPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optTrackNumber);
    }

    void writeTotalTracksPatch(TrackFieldEditContext const& ctx)
    {
      writeUint16Patch(ctx, ctx.patch.optTotalTracks);
    }

    // ---- Registry ----

    using F = rt::TrackField;

    // fmt helpers for conciseness
    auto const readStr = +[](TrackFieldRawValue const& raw) -> std::string
    {
      if (auto const* str = std::get_if<std::string>(&raw))
      {
        return *str;
      }

      return {};
    };

    auto const readUint16 = +[](TrackFieldRawValue const& raw) -> std::string
    {
      if (auto const* val = std::get_if<std::uint16_t>(&raw))
      {
        return formatUint16(*val);
      }

      return {};
    };

    // Struct field order: field, defaultColumnWidth, dragQueryPrefix,
    //   columnVisibleByDefault, columnExpands, columnNumeric, columnTagsCell,
    //   inlineEditable, propertyDialogEditable, propertyDialogReadonly,
    //   readRowText, readViewRawValue, formatValue, writePatch
    auto buildUiDefs()
    {
      return std::to_array<TrackFieldUiDefinition>({
        // ---- Metadata: text ----
        {
          .field = F::Title,
          .defaultColumnWidth = kWidthTitle,
          .dragQueryPrefix = {},
          .columnVisibleByDefault = true,
          .columnExpands = false,
          .columnNumeric = false,
          .columnTagsCell = false,
          .inlineEditable = true,
          .propertyDialogEditable = true,
          .propertyDialogReadonly = false,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return row.getTitle().raw(); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, view.metadata().title()}; },
          .formatValue = readStr,
          .writePatch = writeTitlePatch,
        },
        {
          .field = F::Artist,
          .defaultColumnWidth = kWidthArtist,
          .dragQueryPrefix = "$a=",
          .columnVisibleByDefault = true,
          .inlineEditable = true,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return row.getArtist().raw(); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().artistId())}; },
          .formatValue = readStr,
          .writePatch = writeArtistPatch,
        },
        {
          .field = F::Album,
          .defaultColumnWidth = kWidthAlbum,
          .dragQueryPrefix = "$al=",
          .columnVisibleByDefault = true,
          .inlineEditable = true,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return row.getAlbum().raw(); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().albumId())}; },
          .formatValue = readStr,
          .writePatch = writeAlbumPatch,
        },
        {
          .field = F::AlbumArtist,
          .defaultColumnWidth = kWidthAlbumArtist,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& cache) -> std::string
          {
            auto const id = row.getAlbumArtistId();

            if (id.raw() == 0)
            {
              return {};
            }

            return cache.resolveDictionaryString(id).raw();
          },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          {
            return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().albumArtistId())};
          },
          .formatValue = readStr,
          .writePatch = writeAlbumArtistPatch,
        },
        {
          .field = F::Genre,
          .defaultColumnWidth = kWidthGenre,
          .dragQueryPrefix = "$g=",
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& cache) -> std::string
          {
            auto const id = row.getGenreId();

            if (id.raw() == 0)
            {
              return {};
            }

            return cache.resolveDictionaryString(id).raw();
          },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().genreId())}; },
          .formatValue = readStr,
          .writePatch = writeGenrePatch,
        },
        {
          .field = F::Composer,
          .defaultColumnWidth = kWidthGenre,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& cache) -> std::string
          {
            auto const id = row.getComposerId();

            if (id.raw() == 0)
            {
              return {};
            }

            return cache.resolveDictionaryString(id).raw();
          },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().composerId())}; },
          .formatValue = readStr,
          .writePatch = writeComposerPatch,
        },
        {
          .field = F::Work,
          .defaultColumnWidth = kWidthGenre,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& cache) -> std::string
          {
            auto const id = row.getWorkId();

            if (id.raw() == 0)
            {
              return {};
            }

            return cache.resolveDictionaryString(id).raw();
          },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().workId())}; },
          .formatValue = readStr,
          .writePatch = writeWorkPatch,
        },
        // ---- Metadata: number ----
        {
          .field = F::Year,
          .defaultColumnWidth = kWidthYear,
          .columnNumeric = true,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatUint16(row.getYear()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().year()}; },
          .formatValue = readUint16,
          .writePatch = writeYearPatch,
        },
        {
          .field = F::DiscNumber,
          .defaultColumnWidth = kWidthDisc,
          .columnNumeric = true,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatUint16(row.getDiscNumber()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().discNumber()}; },
          .formatValue = readUint16,
          .writePatch = writeDiscNumberPatch,
        },
        {
          .field = F::TotalDiscs,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatUint16(row.getTotalDiscs()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().totalDiscs()}; },
          .formatValue = readUint16,
          .writePatch = writeTotalDiscsPatch,
        },
        {
          .field = F::TrackNumber,
          .defaultColumnWidth = kWidthTrack,
          .columnNumeric = true,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatUint16(row.getTrackNumber()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().trackNumber()}; },
          .formatValue = readUint16,
          .writePatch = writeTrackNumberPatch,
        },
        {
          .field = F::TotalTracks,
          .propertyDialogEditable = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatUint16(row.getTotalTracks()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().totalTracks()}; },
          .formatValue = readUint16,
          .writePatch = writeTotalTracksPatch,
        },
        // ---- Duration ----
        {
          .field = F::Duration,
          .defaultColumnWidth = kWidthDuration,
          .columnVisibleByDefault = true,
          .columnNumeric = true,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatDuration(row.getDuration()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          {
            return TrackFieldRawValue{
              std::in_place_type<Duration>, std::chrono::milliseconds{view.property().durationMs()}};
          },
          .formatValue = formatDurationValue,
        },
        // ---- Tags ----
        {
          .field = F::Tags,
          .defaultColumnWidth = kWidthTags,
          .columnVisibleByDefault = true,
          .columnExpands = true,
          .columnTagsCell = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return row.getTags().raw(); },
          .readViewRawValue = readTagsViewRawValue,
          .formatValue = readStr,
        },
        // ---- Technical ----
        {
          .field = F::FilePath,
          .defaultColumnWidth = kWidthPath,
          .propertyDialogReadonly = true,
          .readRowText = readFilePathRowText,
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, view.property().uri()}; },
          .formatValue = readStr,
        },
        {
          .field = F::Codec,
          .defaultColumnWidth = kWidthTechnical,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const& cache) -> std::string
          { return formatCodec(row.getCodecId(), cache.dictionary()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const& dict,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::string>, formatCodec(view.property().codecId(), dict)}; },
          .formatValue = readStr,
        },
        {
          .field = F::SampleRate,
          .defaultColumnWidth = kWidthTechnical,
          .columnNumeric = true,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatSampleRate(row.getSampleRate()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint32_t>, view.property().sampleRate()}; },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw))
            {
              return formatSampleRate(*val);
            }

            return {};
          },
        },
        {
          .field = F::Channels,
          .defaultColumnWidth = kWidthTechnical,
          .columnNumeric = true,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatChannels(row.getChannels()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          {
            return TrackFieldRawValue{
              std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(view.property().channels())};
          },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw))
            {
              return formatChannels(static_cast<std::uint8_t>(*val));
            }

            return {};
          },
        },
        {
          .field = F::BitDepth,
          .defaultColumnWidth = kWidthTechnical,
          .columnNumeric = true,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatBitDepth(row.getBitDepth()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          {
            return TrackFieldRawValue{
              std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(view.property().bitDepth())};
          },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw))
            {
              return formatBitDepth(static_cast<std::uint8_t>(*val));
            }

            return {};
          },
        },
        {
          .field = F::Bitrate,
          .defaultColumnWidth = kWidthTechnical,
          .columnNumeric = true,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatBitrate(row.getBitrate()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const*) -> TrackFieldRawValue
          { return TrackFieldRawValue{std::in_place_type<std::uint32_t>, view.property().bitrate()}; },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint32_t>(&raw))
            {
              return formatBitrate(*val);
            }

            return {};
          },
        },
        {
          .field = F::FileSize,
          .defaultColumnWidth = kWidthTechnical,
          .columnNumeric = true,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatFileSize(row.getFileSize()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const* reader) -> TrackFieldRawValue
          {
            if (reader)
            {
              if (auto const optManifestView = reader->get(view.property().uri()))
              {
                return TrackFieldRawValue{std::in_place_type<std::uint64_t>, optManifestView->fileSize()};
              }
            }

            return TrackFieldRawValue{};
          },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint64_t>(&raw))
            {
              return formatFileSize(*val);
            }

            return {};
          },
        },
        {
          .field = F::ModifiedTime,
          .defaultColumnWidth = kWidthTime,
          .propertyDialogReadonly = true,
          .readRowText = +[](TrackRowObject const& row, TrackRowCache const&) -> std::string
          { return formatTime(row.getModifiedTime()); },
          .readViewRawValue = +[](library::TrackView const& view,
                                  library::DictionaryStore const&,
                                  library::FileManifestStore::Reader const* reader) -> TrackFieldRawValue
          {
            if (reader)
            {
              if (auto const optManifestView = reader->get(view.property().uri()))
              {
                return TrackFieldRawValue{std::in_place_type<std::uint64_t>, optManifestView->mtime()};
              }
            }

            return TrackFieldRawValue{};
          },
          .formatValue = +[](TrackFieldRawValue const& raw) -> std::string
          {
            if (auto const* val = std::get_if<std::uint64_t>(&raw))
            {
              return formatTime(*val);
            }

            return {};
          },
        },
        // ---- Synthetic ----
        {
          .field = F::DisplayTrackNumber,
          .defaultColumnWidth = kWidthTrack,
          .columnNumeric = true,
          .readRowText = readDisplayTrackNumber,
          .readViewRawValue = nullptr,
          .formatValue = readStr,
        },
        {
          .field = F::TechnicalSummary,
          .defaultColumnWidth = kWidthTime,
          .readRowText = readTechnicalSummary,
          .readViewRawValue = nullptr,
          .formatValue = readStr,
        },
        {
          .field = F::Quality,
          .defaultColumnWidth = kWidthTechnical,
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
