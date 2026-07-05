// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "DryRunFlag.h"
#include "DumpUtils.h"
#include "Output.h"
#include "QueryHelp.h"
#include "TrackSelection.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/library/detail/TrackViewRawAccess.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>
#include <CLI/Option.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli
{
  namespace
  {
    bool assignStringOption(CLI::Option const* option, std::optional<std::string>& target)
    {
      if (option->count() == 0)
      {
        return false;
      }

      target = option->as<std::string>();
      return true;
    }

    bool assignUint16Option(CLI::Option const* option, std::optional<std::uint16_t>& target)
    {
      if (option->count() == 0)
      {
        return false;
      }

      target = option->as<std::uint16_t>();
      return true;
    }

    bool applyCustomSet(std::string_view assignment, rt::MetadataPatch& patch)
    {
      auto const separator = assignment.find('=');

      if (separator == std::string_view::npos || separator == 0)
      {
        throwCommandError(Error::Code::InvalidInput, "invalid --set value '{}'; expected key=value", assignment);
      }

      patch.customUpdates[std::string{assignment.substr(0, separator)}] = std::string{assignment.substr(separator + 1)};
      return true;
    }

    bool applyCustomUnset(std::string_view key, rt::MetadataPatch& patch)
    {
      if (key.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "invalid --unset value; expected a non-empty key");
      }

      patch.customUpdates[std::string{key}] = std::nullopt;
      return true;
    }

    std::vector<TrackId> resolveUpdateTargets(library::MusicLibrary& ml,
                                              rt::LibraryReader& reader,
                                              std::vector<std::uint32_t> const& rawIds,
                                              std::string const& filter)
    {
      if (!rawIds.empty() && !filter.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "track update accepts either explicit ids or --filter, not both");
      }

      if (rawIds.empty() && filter.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "track update requires track ids or --filter");
      }

      if (!filter.empty())
      {
        return queryMatchingTrackIds(ml, filter);
      }

      return requireTrackIds(reader, rawIds);
    }

    std::vector<TrackId> resolveShowTargets(library::MusicLibrary& ml,
                                            rt::LibraryReader& reader,
                                            std::vector<std::uint32_t> const& rawIds,
                                            std::string const& filter)
    {
      if (!rawIds.empty() && !filter.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "track show accepts either explicit ids or --filter, not both");
      }

      if (!rawIds.empty())
      {
        return requireTrackIds(reader, rawIds);
      }

      return queryMatchingTrackIds(ml, filter);
    }

    struct TrackUpdateReportDto final
    {
      bool dryRun = false;
      std::uint64_t matched = 0;
      std::uint64_t updated = 0;
      std::vector<TrackId> trackIds{};
      std::vector<rt::TrackChangeRecord> changes{};
    };
  } // namespace

  struct TrackCreateReportDto final
  {
    std::string action{};
    bool dryRun = false;
    std::optional<TrackId> optTrackId{};
    std::string uri{};
    std::string title{};
    std::string artist{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::TrackCreateReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optTrackId")
    {
      return "trackId";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    struct TrackDeleteReportDto final
    {
      std::string action{};
      bool dryRun = false;
      TrackId trackId{};
      std::string uri{};
      std::string title{};
      std::vector<ListId> removedFromListIds{};
    };

    void formatUpdateReply(rt::UpdateTrackMetadataReply const& reply,
                           bool dryRun,
                           std::uint64_t matched,
                           OutputFormat format,
                           std::ostream& os)
    {
      auto const updated = reply.mutatedIds.size();

      if (format != OutputFormat::Plain)
      {
        emitDocument(os,
                     format,
                     TrackUpdateReportDto{.dryRun = dryRun,
                                          .matched = matched,
                                          .updated = static_cast<std::uint64_t>(updated),
                                          .trackIds = reply.mutatedIds,
                                          .changes = reply.changes});
        return;
      }

      std::println(os, "updated {} of {} matched track(s){}", updated, matched, dryRun ? " (dry-run)" : "");
    }

    void formatTrackCreate(std::optional<TrackId> optTrackId,
                           std::string const& uri,
                           std::string const& title,
                           std::string const& artist,
                           bool dryRun,
                           OutputFormat format,
                           std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os,
                     format,
                     TrackCreateReportDto{.action = "create",
                                          .dryRun = dryRun,
                                          .optTrackId = optTrackId,
                                          .uri = uri,
                                          .title = title,
                                          .artist = artist});
        return;
      }

      if (optTrackId)
      {
        std::println(os, "added track: {}{}", *optTrackId, dryRun ? " (dry-run)" : "");
        return;
      }

      std::println(os, "added track: {}{}", uri, dryRun ? " (dry-run)" : "");
    }

    void formatTrackDelete(rt::DeleteTrackReply const& reply, bool dryRun, OutputFormat format, std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os,
                     format,
                     TrackDeleteReportDto{.action = "delete",
                                          .dryRun = dryRun,
                                          .trackId = reply.trackId,
                                          .uri = reply.uri,
                                          .title = reply.title,
                                          .removedFromListIds = reply.removedFromListIds});
        return;
      }

      std::println(os, "deleted track: {}{}", reply.trackId, dryRun ? " (dry-run)" : "");
    }

    void updateTracks(CliContext& context,
                      std::vector<std::uint32_t> const& rawIds,
                      std::string const& filter,
                      rt::MetadataPatch const& patch,
                      bool dryRun)
    {
      auto& ml = context.musicLibrary();
      auto reader = context.library().reader();
      auto const targetIds = resolveUpdateTargets(ml, reader, rawIds, filter);

      if (dryRun)
      {
        auto const replyResult = context.library().writer().previewUpdateMetadata(targetIds, patch);

        if (!replyResult)
        {
          throwCommandError(replyResult.error());
        }

        formatUpdateReply(
          *replyResult, true, static_cast<std::uint64_t>(targetIds.size()), context.options().format, context.io().out);
        return;
      }

      auto const replyResult = context.library().writer().updateMetadata(targetIds, patch);

      if (!replyResult)
      {
        throwCommandError(replyResult.error());
      }

      formatUpdateReply(
        *replyResult, false, static_cast<std::uint64_t>(targetIds.size()), context.options().format, context.io().out);
    }
  } // namespace

  struct TrackRecordDto final
  {
    TrackId id{};
    std::optional<std::string> optTitle{};
    std::optional<std::string> optArtist{};
    std::optional<std::string> optAlbum{};
    std::optional<std::string> optAlbumArtist{};
    std::optional<std::string> optGenre{};
    std::optional<std::string> optComposer{};
    std::optional<std::string> optConductor{};
    std::optional<std::string> optEnsemble{};
    std::optional<std::string> optWork{};
    std::optional<std::string> optMovement{};
    std::optional<std::string> optSoloist{};
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTrackTotal{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optDiscTotal{};
    std::optional<std::uint16_t> optMovementNumber{};
    std::optional<std::uint16_t> optMovementTotal{};
    std::optional<std::vector<std::string>> optTags{};
    std::optional<std::uint64_t> optDuration{};
    std::optional<std::uint32_t> optSampleRate{};
    std::optional<std::string> optUri{};
    std::optional<std::map<std::string, std::string>> optCustom{};
  };

  struct TrackListDto final
  {
    std::vector<TrackRecordDto> tracks{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::TrackRecordDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optTitle")
    {
      return "title";
    }

    if (memberName == "optArtist")
    {
      return "artist";
    }

    if (memberName == "optAlbum")
    {
      return "album";
    }

    if (memberName == "optAlbumArtist")
    {
      return "albumArtist";
    }

    if (memberName == "optGenre")
    {
      return "genre";
    }

    if (memberName == "optComposer")
    {
      return "composer";
    }

    if (memberName == "optConductor")
    {
      return "conductor";
    }

    if (memberName == "optEnsemble")
    {
      return "ensemble";
    }

    if (memberName == "optWork")
    {
      return "work";
    }

    if (memberName == "optMovement")
    {
      return "movement";
    }

    if (memberName == "optSoloist")
    {
      return "soloist";
    }

    if (memberName == "optYear")
    {
      return "year";
    }

    if (memberName == "optTrackNumber")
    {
      return "trackNumber";
    }

    if (memberName == "optTrackTotal")
    {
      return "trackTotal";
    }

    if (memberName == "optDiscNumber")
    {
      return "discNumber";
    }

    if (memberName == "optDiscTotal")
    {
      return "discTotal";
    }

    if (memberName == "optMovementNumber")
    {
      return "movementNumber";
    }

    if (memberName == "optMovementTotal")
    {
      return "movementTotal";
    }

    if (memberName == "optTags")
    {
      return "tags";
    }

    if (memberName == "optDuration")
    {
      return "duration";
    }

    if (memberName == "optSampleRate")
    {
      return "sampleRate";
    }

    if (memberName == "optUri")
    {
      return "uri";
    }

    if (memberName == "optCustom")
    {
      return "custom";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    std::vector<std::string> tagNames(library::TrackView const& view, library::DictionaryStore const& dict)
    {
      auto names = std::vector<std::string>{};

      for (auto const tagId : view.tags())
      {
        names.emplace_back(resolveDict(dict, tagId));
      }

      return names;
    }

    std::map<std::string, std::string> customMap(library::TrackView const& view, library::DictionaryStore const& dict)
    {
      auto result = std::map<std::string, std::string>{};

      for (auto const& [customId, val] : view.customMetadata())
      {
        result.emplace(std::string{resolveDict(dict, customId)}, val);
      }

      return result;
    }

    std::optional<std::string> optionalDictName(library::DictionaryStore const& dict, DictionaryId id)
    {
      if (id == kInvalidDictionaryId)
      {
        return std::nullopt;
      }

      return std::string{resolveDict(dict, id)};
    }

    std::optional<std::string> optionalString(std::string_view value)
    {
      if (value.empty())
      {
        return std::nullopt;
      }

      return std::string{value};
    }

    std::optional<std::uint16_t> optionalNumber(std::uint16_t value)
    {
      if (value == 0)
      {
        return std::nullopt;
      }

      return value;
    }

    std::optional<std::uint64_t> optionalDuration(library::TrackDuration duration)
    {
      if (duration.count() <= 0)
      {
        return std::nullopt;
      }

      return static_cast<std::uint64_t>(duration.count());
    }

    std::optional<std::uint32_t> optionalSampleRate(SampleRate value)
    {
      if (value.raw() == 0)
      {
        return std::nullopt;
      }

      return value.raw();
    }

    TrackRecordDto trackRecordDto(TrackId id, library::TrackView const& view, library::DictionaryStore const& dict)
    {
      auto dto = TrackRecordDto{.id = id};

      if (view.isHotValid())
      {
        dto.optTitle = optionalString(view.metadata().title());
        dto.optArtist = optionalDictName(dict, view.metadata().artistId());
        dto.optAlbum = optionalDictName(dict, view.metadata().albumId());
        dto.optAlbumArtist = optionalDictName(dict, view.metadata().albumArtistId());
        dto.optGenre = optionalDictName(dict, view.metadata().genreId());
        dto.optComposer = optionalDictName(dict, view.metadata().composerId());
        dto.optYear = optionalNumber(view.metadata().year());
        dto.optTags = tagNames(view, dict);
      }

      if (view.isColdValid())
      {
        dto.optConductor = optionalDictName(dict, view.classical().conductorId());
        dto.optEnsemble = optionalDictName(dict, view.classical().ensembleId());
        dto.optWork = optionalDictName(dict, view.classical().workId());
        dto.optMovement = optionalDictName(dict, view.classical().movementId());
        dto.optSoloist = optionalDictName(dict, view.classical().soloistId());
        dto.optTrackNumber = optionalNumber(view.metadata().trackNumber());
        dto.optTrackTotal = optionalNumber(view.metadata().trackTotal());
        dto.optDiscNumber = optionalNumber(view.metadata().discNumber());
        dto.optDiscTotal = optionalNumber(view.metadata().discTotal());
        dto.optMovementNumber = optionalNumber(view.classical().movementNumber());
        dto.optMovementTotal = optionalNumber(view.classical().movementTotal());
        dto.optDuration = optionalDuration(view.property().duration());
        dto.optSampleRate = optionalSampleRate(view.property().sampleRate());
        dto.optUri = optionalString(view.property().uri());
        dto.optCustom = customMap(view, dict);
      }

      return dto;
    }

    void emitJsonTrackRecord(std::ostream& os,
                             TrackId id,
                             library::TrackView const& view,
                             library::DictionaryStore const& dict)
    {
      emitDocument(os, OutputFormat::Json, trackRecordDto(id, view, dict));
    }

    void formatStructuredTracks(std::vector<TrackId> const& trackIds,
                                std::size_t offset,
                                std::size_t limit,
                                library::MusicLibrary& ml,
                                OutputFormat format,
                                std::ostream& os)
    {
      if (offset >= trackIds.size())
      {
        if (format == OutputFormat::Yaml)
        {
          emitDocument(os, format, TrackListDto{});
        }

        return;
      }

      std::size_t const end = (limit == 0) ? trackIds.size() : std::min(offset + limit, trackIds.size());
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);
      auto const& dict = ml.dictionary();

      if (format == OutputFormat::Yaml)
      {
        auto dto = TrackListDto{};

        for (std::size_t i = offset; i < end; ++i)
        {
          auto const id = trackIds[i];
          auto const optView = rt::storageValueOrNullopt(
            reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to read track");

          if (optView)
          {
            dto.tracks.push_back(trackRecordDto(id, *optView, dict));
          }
        }

        emitDocument(os, format, dto);
        return;
      }

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const id = trackIds[i];
        auto const optView = rt::storageValueOrNullopt(
          reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to read track");

        if (optView)
        {
          emitJsonTrackRecord(os, id, *optView, dict);
        }
      }
    }

    void formatPlain(library::MusicLibrary& ml,
                     std::vector<TrackId> const& trackIds,
                     std::size_t offset,
                     std::size_t limit,
                     std::ostream& os)
    {
      if (offset >= trackIds.size())
      {
        return;
      }

      std::size_t const end = (limit == 0) ? trackIds.size() : std::min(offset + limit, trackIds.size());
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const id = trackIds[i];
        auto const optView =
          rt::storageValueOrNullopt(reader.get(id, library::TrackStore::Reader::LoadMode::Hot), "Failed to read track");

        if (optView)
        {
          std::println(os, "{:>5} {}", id, optView->metadata().title());
        }
      }

      if (limit > 0 && offset + limit < trackIds.size())
      {
        std::println(os, "... ({} more)", trackIds.size() - offset - limit);
      }
    }

    void show(library::MusicLibrary& ml,
              std::vector<TrackId> const& trackIds,
              OutputFormat format,
              std::string const& formatExpression,
              std::size_t limit,
              std::size_t offset,
              std::ostream& os)
    {
      if (!formatExpression.empty())
      {
        if (format != OutputFormat::Plain)
        {
          throwCommandError(Error::Code::InvalidInput, "track show --format supports only plain output");
        }

        auto const expr = query::parse(formatExpression);

        if (!expr)
        {
          auto const& error = expr.error();
          throwCommandError(error, "format error: {}{}", error.message, formatExpressionUsageHint());
        }

        auto plan = query::compileFormat(*expr, &ml.dictionary());

        if (!plan)
        {
          auto const& error = plan.error();
          throwCommandError(error, "format error: {}{}", error.message, formatExpressionUsageHint());
        }

        if (offset >= trackIds.size())
        {
          return;
        }

        auto evaluator = query::FormatEvaluator{};
        std::size_t const end = (limit == 0) ? trackIds.size() : std::min(offset + limit, trackIds.size());
        auto const txn = ml.readTransaction();
        auto const reader = ml.tracks().reader(txn);

        for (std::size_t i = offset; i < end; ++i)
        {
          auto const id = trackIds[i];
          auto const optView = rt::storageValueOrNullopt(
            reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to read track");

          if (optView)
          {
            std::println(os, "{}", evaluator.evaluate(*plan, *optView));
          }
        }

        return;
      }

      if (format == OutputFormat::Plain)
      {
        formatPlain(ml, trackIds, offset, limit, os);
      }
      else
      {
        formatStructuredTracks(trackIds, offset, limit, ml, format, os);
      }
    }

    void dumpRawHot(library::TrackView const& view, std::span<std::byte const> hotData, std::ostream& os)
    {
      if (!view.isHotValid())
      {
        return;
      }

      std::println(os, "Hot Header:");
      hexDump(hotData.subspan(0, sizeof(library::TrackHotHeader)), os);
      std::println(os, "Hot Payload:");

      if (hotData.size() > sizeof(library::TrackHotHeader))
      {
        hexDump(hotData.subspan(sizeof(library::TrackHotHeader)), os);
      }
    }

    void dumpRawColdSections(std::span<std::byte const> coldData,
                             library::detail::TrackColdReader const& coldReader,
                             std::ostream& os)
    {
      auto const& header = coldReader.header();
      auto const blockOffset = sizeof(library::TrackColdHeader);
      auto const uriOffset = static_cast<std::size_t>(header.uriOffset);
      auto const blockLength = uriOffset - blockOffset;
      auto const uriLength = static_cast<std::size_t>(header.uriLength);
      auto const paddingOffset = uriOffset + uriLength;

      std::println(os, "Cold Blocks:");

      if (blockLength > 0)
      {
        hexDump(coldData.subspan(blockOffset, blockLength), os);
      }

      std::println(os, "Cold URI:");

      if (uriLength > 0)
      {
        hexDump(coldData.subspan(uriOffset, uriLength), os);
      }

      std::println(os, "Cold Padding:");

      if (paddingOffset < coldData.size())
      {
        hexDump(coldData.subspan(paddingOffset), os);
      }
    }

    void dumpRawCold(library::TrackView const& view, std::span<std::byte const> coldData, std::ostream& os)
    {
      if (!view.isColdValid())
      {
        return;
      }

      auto const coldReader = library::detail::TrackColdReader{coldData};

      std::println(os, "Cold Header:");
      hexDump(coldData.subspan(0, sizeof(library::TrackColdHeader)), os);

      if (coldReader.valid())
      {
        dumpRawColdSections(coldData, coldReader, os);
        return;
      }

      if (coldData.size() > sizeof(library::TrackColdHeader))
      {
        std::println(os, "Cold Blocks/URI/Padding (invalid layout):");
        hexDump(coldData.subspan(sizeof(library::TrackColdHeader)), os);
      }
    }

    void dumpRawTrack(TrackId id, library::TrackView const& view, std::ostream& os)
    {
      std::println(os, "Track ID: {}", id);

      auto const hotData = library::detail::TrackViewRawAccess::hotData(view);
      auto const coldData = library::detail::TrackViewRawAccess::coldData(view);

      dumpRawHot(view, hotData, os);
      dumpRawCold(view, coldData, os);
    }

    void dumpPlainHot(library::TrackView const& view, library::DictionaryStore const& dict, std::ostream& os)
    {
      if (!view.isHotValid())
      {
        return;
      }

      std::println(os, "  Title: {}", view.metadata().title());
      std::println(
        os, "  Artist: {} (ID: {})", resolveDict(dict, view.metadata().artistId()), view.metadata().artistId());
      std::println(os, "  Album: {} (ID: {})", resolveDict(dict, view.metadata().albumId()), view.metadata().albumId());
      std::println(os, "  Tag Bloom: 0x{:08x}", view.tags().bloom());
      std::print(os, "  Tags: ");

      for (auto const tagId : view.tags())
      {
        std::print(os, "{} (ID: {}) ", resolveDict(dict, tagId), tagId);
      }

      std::println(os);
    }

    void dumpPlainCold(library::TrackView const& view, library::DictionaryStore const& dict, std::ostream& os)
    {
      if (!view.isColdValid())
      {
        return;
      }

      std::println(os, "  Duration: {}ms", view.property().duration().count());
      std::println(os, "  Sample Rate: {}Hz", view.property().sampleRate());
      std::println(os, "  URI: {}", view.property().uri());

      for (auto const& [customId, val] : view.customMetadata())
      {
        std::println(os, "  Custom [{}]: {}", resolveDict(dict, customId), val);
      }
    }

    void dumpPlainTrack(TrackId id,
                        library::TrackView const& view,
                        library::DictionaryStore const& dict,
                        std::ostream& os)
    {
      std::println(os, "Track ID: {}", id);

      dumpPlainHot(view, dict, os);
      dumpPlainCold(view, dict, os);
    }

    void processTrackDump(TrackId id,
                          library::TrackView const& view,
                          library::DictionaryStore const& dict,
                          bool raw,
                          std::ostream& os)
    {
      if (raw)
      {
        dumpRawTrack(id, view, os);
        return;
      }

      dumpPlainTrack(id, view, dict, os);
    }

    void dumpTracks(library::MusicLibrary& ml, std::uint32_t targetId, bool raw, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);

      if (auto const& dict = ml.dictionary(); targetId > 0)
      {
        auto const id = TrackId{targetId};

        if (auto const optView = rt::storageValueOrNullopt(
              reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to dump track");
            optView)
        {
          processTrackDump(id, *optView, dict, raw, os);
        }
        else
        {
          throwCommandError(Error::Code::NotFound, "track not found: {}", targetId);
        }
      }
      else
      {
        for (auto const& [id, view] : reader)
        {
          processTrackDump(id, view, dict, raw, os);
        }
      }
    }

    void setupTrackShowCommand(CLI::App& track, CliContext& context)
    {
      auto* showCmd = track.add_subcommand("show", "Show tracks by id or filter");
      auto* ids = showCmd->add_option("id", "track ids to show")->expected(0, -1);
      auto* filter = showCmd->add_option("-f,--filter", "track filter expression");
      auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
      auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);
      auto* formatExpression = showCmd->add_option("--format", "format expression, e.g. '$artist + \" - \" + $title'");
      showCmd->footer(trackShowHelpFooter());

      showCmd->callback(
        [&context, ids, filter, limit, offset, formatExpression]
        {
          auto reader = context.library().reader();
          auto const rawIds = ids->count() > 0 ? ids->as<std::vector<std::uint32_t>>() : std::vector<std::uint32_t>{};
          auto const targetIds = resolveShowTargets(
            context.musicLibrary(), reader, rawIds, filter->count() > 0 ? filter->as<std::string>() : std::string{});
          show(context.musicLibrary(),
               targetIds,
               context.options().format,
               formatExpression->count() > 0 ? formatExpression->as<std::string>() : std::string{},
               limit->as<std::size_t>(),
               offset->as<std::size_t>(),
               context.io().out);
        });
    }

    void setupTrackCreateCommand(CLI::App& track, CliContext& context)
    {
      auto* create = track.add_subcommand("create", "Create a track from a file");
      auto* path = create->add_option("path", "audio file path")->required();
      auto* dryRun = addDryRunFlag(*create);
      create->callback(
        [&context, path, dryRun]
        {
          auto const pathValue = path->as<std::string>();

          if (isDryRun(dryRun))
          {
            auto const trackResult = context.library().writer().previewCreateTrackFromFile(pathValue);

            if (!trackResult)
            {
              auto const& error = trackResult.error();
              throwCommandError(error, "error adding track from: {}: {}", pathValue, error.message);
            }

            formatTrackCreate(std::nullopt,
                              trackResult->uri,
                              trackResult->title,
                              trackResult->artist,
                              true,
                              context.options().format,
                              context.io().out);
            return;
          }

          auto const trackResult = context.library().writer().createTrackFromFile(pathValue);

          if (trackResult)
          {
            formatTrackCreate(std::optional<TrackId>{trackResult->trackId},
                              trackResult->uri,
                              trackResult->title,
                              trackResult->artist,
                              false,
                              context.options().format,
                              context.io().out);
          }
          else
          {
            auto const& error = trackResult.error();
            throwCommandError(error, "error adding track from: {}: {}", pathValue, error.message);
          }
        });
    }

    struct TrackUpdateCliOptions final
    {
      CLI::Option* ids = nullptr;
      CLI::Option* filter = nullptr;
      CLI::Option* title = nullptr;
      CLI::Option* artist = nullptr;
      CLI::Option* album = nullptr;
      CLI::Option* albumArtist = nullptr;
      CLI::Option* genre = nullptr;
      CLI::Option* composer = nullptr;
      CLI::Option* conductor = nullptr;
      CLI::Option* ensemble = nullptr;
      CLI::Option* work = nullptr;
      CLI::Option* movement = nullptr;
      CLI::Option* soloist = nullptr;
      CLI::Option* year = nullptr;
      CLI::Option* trackNumber = nullptr;
      CLI::Option* trackTotal = nullptr;
      CLI::Option* discNumber = nullptr;
      CLI::Option* discTotal = nullptr;
      CLI::Option* movementNumber = nullptr;
      CLI::Option* movementTotal = nullptr;
      CLI::Option* set = nullptr;
      CLI::Option* unset = nullptr;
      CLI::Option* dryRun = nullptr;
      std::shared_ptr<std::vector<std::string>> setsPtr;
      std::shared_ptr<std::vector<std::string>> unsetsPtr;
    };

    bool applyTrackUpdateFieldOptions(TrackUpdateCliOptions const& options, rt::MetadataPatch& patch)
    {
      bool hasPatch = false;
      hasPatch = assignStringOption(options.title, patch.optTitle) || hasPatch;
      hasPatch = assignStringOption(options.artist, patch.optArtist) || hasPatch;
      hasPatch = assignStringOption(options.album, patch.optAlbum) || hasPatch;
      hasPatch = assignStringOption(options.albumArtist, patch.optAlbumArtist) || hasPatch;
      hasPatch = assignStringOption(options.genre, patch.optGenre) || hasPatch;
      hasPatch = assignStringOption(options.composer, patch.optComposer) || hasPatch;
      hasPatch = assignStringOption(options.conductor, patch.optConductor) || hasPatch;
      hasPatch = assignStringOption(options.ensemble, patch.optEnsemble) || hasPatch;
      hasPatch = assignStringOption(options.work, patch.optWork) || hasPatch;
      hasPatch = assignStringOption(options.movement, patch.optMovement) || hasPatch;
      hasPatch = assignStringOption(options.soloist, patch.optSoloist) || hasPatch;
      hasPatch = assignUint16Option(options.year, patch.optYear) || hasPatch;
      hasPatch = assignUint16Option(options.trackNumber, patch.optTrackNumber) || hasPatch;
      hasPatch = assignUint16Option(options.trackTotal, patch.optTrackTotal) || hasPatch;
      hasPatch = assignUint16Option(options.discNumber, patch.optDiscNumber) || hasPatch;
      hasPatch = assignUint16Option(options.discTotal, patch.optDiscTotal) || hasPatch;
      hasPatch = assignUint16Option(options.movementNumber, patch.optMovementNumber) || hasPatch;
      hasPatch = assignUint16Option(options.movementTotal, patch.optMovementTotal) || hasPatch;
      return hasPatch;
    }

    bool applyTrackUpdateCustomOptions(TrackUpdateCliOptions const& options, rt::MetadataPatch& patch)
    {
      bool hasPatch = false;

      if (options.set->count() > 0)
      {
        for (auto const& assignment : *options.setsPtr)
        {
          hasPatch = applyCustomSet(assignment, patch) || hasPatch;
        }
      }

      if (options.unset->count() > 0)
      {
        for (auto const& key : *options.unsetsPtr)
        {
          hasPatch = applyCustomUnset(key, patch) || hasPatch;
        }
      }

      return hasPatch;
    }

    void runTrackUpdateCommand(CliContext& context, TrackUpdateCliOptions const& options)
    {
      auto patch = rt::MetadataPatch{};
      bool hasPatch = applyTrackUpdateFieldOptions(options, patch);
      hasPatch = applyTrackUpdateCustomOptions(options, patch) || hasPatch;

      if (!hasPatch)
      {
        throwCommandError(Error::Code::InvalidInput, "track update requires at least one field option");
      }

      auto const rawIds =
        options.ids->count() > 0 ? options.ids->as<std::vector<std::uint32_t>>() : std::vector<std::uint32_t>{};
      auto const filter = options.filter->count() > 0 ? options.filter->as<std::string>() : std::string{};
      updateTracks(context, rawIds, filter, patch, isDryRun(options.dryRun));
    }

    void setupTrackUpdateCommand(CLI::App& track, CliContext& context)
    {
      auto* update = track.add_subcommand("update", "Update track metadata");
      update->footer(trackUpdateHelpFooter());
      auto updateSetsPtr = std::make_shared<std::vector<std::string>>();
      auto updateUnsetsPtr = std::make_shared<std::vector<std::string>>();
      auto options = TrackUpdateCliOptions{
        .ids = update->add_option("id", "track id to update")->expected(0, -1),
        .filter = update->add_option("-f,--filter", "track filter expression"),
        .title = update->add_option("--title", "title"),
        .artist = update->add_option("--artist", "artist"),
        .album = update->add_option("--album", "album"),
        .albumArtist = update->add_option("--album-artist", "album artist"),
        .genre = update->add_option("--genre", "genre"),
        .composer = update->add_option("--composer", "composer"),
        .conductor = update->add_option("--conductor", "conductor"),
        .ensemble = update->add_option("--ensemble", "ensemble"),
        .work = update->add_option("--work", "work"),
        .movement = update->add_option("--movement", "movement"),
        .soloist = update->add_option("--soloist", "soloist"),
        .year = update->add_option("--year", "year"),
        .trackNumber = update->add_option("--track-number", "track number"),
        .trackTotal = update->add_option("--track-total", "track total"),
        .discNumber = update->add_option("--disc-number", "disc number"),
        .discTotal = update->add_option("--disc-total", "disc total"),
        .movementNumber = update->add_option("--movement-number", "movement number"),
        .movementTotal = update->add_option("--movement-total", "movement total"),
        .set = update->add_option("--set", *updateSetsPtr, "set custom metadata key=value"),
        .unset = update->add_option("--unset", *updateUnsetsPtr, "unset custom metadata key"),
        .dryRun = addDryRunFlag(*update),
        .setsPtr = updateSetsPtr,
        .unsetsPtr = updateUnsetsPtr,
      };

      update->callback([&context, options] { runTrackUpdateCommand(context, options); });
    }

    void setupTrackDeleteCommand(CLI::App& track, CliContext& context)
    {
      auto* del = track.add_subcommand("delete", "Delete a track by id");
      auto* id = del->add_option("id", "track id")->required();
      auto* dryRun = addDryRunFlag(*del);
      del->callback(
        [&context, id, dryRun]
        {
          auto const trackId = TrackId{id->as<std::uint32_t>()};

          if (isDryRun(dryRun))
          {
            auto const deleteResult = context.library().writer().previewDeleteTrack(trackId);

            if (!deleteResult)
            {
              throwCommandError(deleteResult.error());
            }

            formatTrackDelete(*deleteResult, true, context.options().format, context.io().out);
            return;
          }

          auto const deleteResult = context.library().writer().deleteTrack(trackId);

          if (deleteResult)
          {
            formatTrackDelete(*deleteResult, false, context.options().format, context.io().out);
          }
          else
          {
            throwCommandError(deleteResult.error());
          }
        });
    }

    void setupTrackDumpCommand(CLI::App& track, CliContext& context)
    {
      auto* dumpCmd = track.add_subcommand("dump", "Dump tracks from database");
      auto* dumpId = dumpCmd->add_option("--id", "track id to dump");
      auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");

      dumpCmd->callback(
        [&context, dumpId, dumpRaw]
        {
          if (context.options().format != OutputFormat::Plain)
          {
            throwCommandError(Error::Code::InvalidInput,
                              "track dump supports only plain output; use track show -O yaml/json for structured data");
          }

          dumpTracks(context.musicLibrary(),
                     dumpId->count() > 0 ? dumpId->as<std::uint32_t>() : 0,
                     dumpRaw->count() > 0,
                     context.io().out);
        });
    }
  } // namespace

  void setupTrackCommand(CLI::App& app, CliContext& context)
  {
    auto* track = app.add_subcommand("track", "Track management commands");
    track->footer(trackHelpFooter());
    track->require_subcommand(1);
    setupTrackShowCommand(*track, context);
    setupTrackCreateCommand(*track, context);
    setupTrackUpdateCommand(*track, context);
    setupTrackDeleteCommand(*track, context);
    setupTrackDumpCommand(*track, context);
  }
} // namespace ao::cli
