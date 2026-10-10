// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliRuntime.h"
#include "CommandError.h"
#include "CommandRegistrations.h"
#include "DryRunFlag.h"
#include "DumpOutput.h"
#include "Output.h"
#include "QueryHelp.h"
#include "TrackSelection.h"
#include <ao/AudioScalars.h>
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/library/detail/TrackColdReader.h>
#include <ao/library/detail/TrackViewRawAccess.h>
#include <ao/query/Field.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/utility/Path.h>
#include <ao/utility/String.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>
#include <CLI/Error.hpp>
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
#include <utility>
#include <vector>

namespace ao::cli
{
  namespace
  {
    bool tryAssignStringOption(CLI::Option const* option, std::optional<std::string>& optTarget)
    {
      if (option->count() == 0)
      {
        return false;
      }

      optTarget = option->as<std::string>();
      return true;
    }

    bool tryAssignUint16Option(CLI::Option const* option, std::optional<std::uint16_t>& optTarget)
    {
      if (option->count() == 0)
      {
        return false;
      }

      optTarget = option->as<std::uint16_t>();
      return true;
    }

    bool tryApplyCustomSet(std::string_view assignment, rt::MetadataPatch& patch)
    {
      auto const separator = assignment.find('=');

      if (separator == std::string_view::npos || separator == 0)
      {
        throwCommandError(Error::Code::InvalidInput, "invalid --set value '{}'; expected key=value", assignment);
      }

      auto const key = assignment.substr(0, separator);

      // Reserved metadata keys are rejected instead of being stored as custom
      // metadata. This command keeps its own diagnostic.
      if (rt::isReservedCustomMetadataKey(key))
      {
        throwCommandError(Error::Code::InvalidInput, "invalid --set key '{}': reserved metadata key", key);
      }

      patch.customUpdates[std::string{key}] = std::string{assignment.substr(separator + 1)};
      return true;
    }

    bool tryApplyCustomUnset(std::string_view key, rt::MetadataPatch& patch)
    {
      if (key.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "invalid --unset value; expected a non-empty key");
      }

      patch.customUpdates[std::string{key}] = std::nullopt;
      return true;
    }

    std::vector<TrackId> resolveUpdateTargets(library::MusicLibrary const& ml,
                                              rt::LibrarySnapshot& snapshot,
                                              std::vector<std::uint32_t> const& rawIds,
                                              std::optional<std::string> const& optFilter)
    {
      if (!rawIds.empty() && optFilter)
      {
        throwCommandError(Error::Code::InvalidInput, "track update accepts either explicit ids or --filter, not both");
      }

      if (!rawIds.empty())
      {
        return requireTrackIds(snapshot, rawIds);
      }

      if (!optFilter)
      {
        throwCommandError(Error::Code::InvalidInput, "track update requires track ids or --filter");
      }

      if (optFilter->empty())
      {
        throwCommandError(Error::Code::InvalidInput, "track update requires a non-empty --filter expression");
      }

      return queryMatchingTrackIds(ml, *optFilter);
    }

    std::vector<TrackId> resolveShowTargets(library::MusicLibrary const& ml,
                                            rt::LibrarySnapshot& snapshot,
                                            std::vector<std::uint32_t> const& rawIds,
                                            std::optional<std::string> const& optFilter)
    {
      if (!rawIds.empty() && optFilter)
      {
        throwCommandError(Error::Code::InvalidInput, "track show accepts either explicit ids or --filter, not both");
      }

      if (!rawIds.empty())
      {
        return requireTrackIds(snapshot, rawIds);
      }

      if (optFilter && optFilter->empty())
      {
        throwCommandError(Error::Code::InvalidInput, "track show requires a non-empty --filter expression");
      }

      // Without ids or a filter, show walks every track.
      return queryMatchingTrackIds(ml, optFilter.value_or(std::string{}));
    }
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

  // Structured credits preserve kind, order and duplicates; absent roles are omitted.
  struct TrackCreditRecordDto final
  {
    std::string name{};
    std::string kind{};
    std::optional<std::string> optRole{};
  };

  struct TrackUpdateReportDto final
  {
    bool dryRun = false;
    std::uint64_t matched = 0;
    std::uint64_t updated = 0;
    std::vector<TrackId> trackIds{};
    std::vector<rt::TrackChangeRecord> changes{};
    std::optional<std::vector<rt::TrackTagsChange>> optTagChanges{};
  };

  struct TrackDeleteReportDto final
  {
    std::string action{};
    bool dryRun = false;
    TrackId trackId{};
    std::string uri{};
    std::string title{};
    std::vector<ListId> removedFromListIds{};
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

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::TrackCreditRecordDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optRole")
    {
      return "role";
    }

    return memberName;
  }
};

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::TrackUpdateReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optTagChanges")
    {
      return "tagChanges";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    void printPlainTagChanges(rt::EditTrackTagsReply const& reply, std::ostream& os)
    {
      auto addedCounts = std::map<std::string, std::uint64_t>{};
      auto removedCounts = std::map<std::string, std::uint64_t>{};

      for (auto const& change : reply.changes)
      {
        for (auto const& tag : change.addedTags)
        {
          ++addedCounts[tag];
        }

        for (auto const& tag : change.removedTags)
        {
          ++removedCounts[tag];
        }
      }

      for (auto const& [name, count] : addedCounts)
      {
        std::println(os, "added tag: {} to {} track(s)", name, count);
      }

      for (auto const& [name, count] : removedCounts)
      {
        std::println(os, "removed tag: {} from {} track(s)", name, count);
      }
    }

    void formatUpdateReply(rt::UpdateTrackMetadataReply const& reply,
                           bool dryRun,
                           std::uint64_t matched,
                           OutputFormat format,
                           std::ostream& os,
                           rt::EditTrackTagsReply const* tagsReply = nullptr)
    {
      // When a tag edit is part of the mutation, updated and trackIds cover the
      // union of tracks mutated by metadata or tag changes; changes stays
      // metadata-only and tagChanges carries the per-track tag records.
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(reply.changes.size());

      for (auto const& change : reply.changes)
      {
        trackIds.push_back(change.trackId);
      }

      if (tagsReply != nullptr)
      {
        for (auto const& change : tagsReply->changes)
        {
          trackIds.push_back(change.trackId);
        }

        std::ranges::sort(trackIds);
        trackIds.erase(std::ranges::unique(trackIds).begin(), trackIds.end());
      }

      auto const updated = static_cast<std::uint64_t>(trackIds.size());

      if (format != OutputFormat::Plain)
      {
        auto report = TrackUpdateReportDto{.dryRun = dryRun,
                                           .matched = matched,
                                           .updated = updated,
                                           .trackIds = std::move(trackIds),
                                           .changes = reply.changes};

        if (tagsReply != nullptr)
        {
          report.optTagChanges = tagsReply->changes;
        }

        emitDocument(os, format, report);
        return;
      }

      std::println(os, "updated {} of {} matched track(s){}", updated, matched, dryRun ? " (dry-run)" : "");

      if (tagsReply != nullptr)
      {
        printPlainTagChanges(*tagsReply, os);
      }
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

    void updateTracks(CliRuntime& cli,
                      std::vector<std::uint32_t> const& rawIds,
                      std::optional<std::string> const& optFilter,
                      rt::MetadataPatch const& patch,
                      bool dryRun)
    {
      auto const& ml = cli.musicLibrary();
      auto snapshot = cli.library().snapshot();
      auto const targetIds = resolveUpdateTargets(ml, snapshot, rawIds, optFilter);

      // A filter that matches nothing is a successful no-op, not an authoring
      // error: the runtime refuses to bind an empty target set, so report zero
      // counts without submitting a mutation.
      if (targetIds.empty())
      {
        formatUpdateReply(rt::UpdateTrackMetadataReply{}, dryRun, 0, cli.options().format, cli.io().out);
        return;
      }

      if (dryRun)
      {
        auto const replyRes = cli.runTask(cli.library().commands().previewUpdateMetadataAsync(targetIds, patch));

        if (!replyRes)
        {
          throwCommandError(replyRes.error());
        }

        formatUpdateReply(
          *replyRes, true, static_cast<std::uint64_t>(targetIds.size()), cli.options().format, cli.io().out);
        return;
      }

      auto const bindingRes = cli.library().bindTrackTargets(targetIds);

      if (!bindingRes)
      {
        throwCommandError(bindingRes.error());
      }

      auto const replyRes = cli.runTask(cli.library().commands().updateMetadataAsync(*bindingRes, patch));

      if (!replyRes)
      {
        throwCommandError(replyRes.error());
      }

      switch (replyRes->status)
      {
        case rt::AuthoringStatus::Applied:
        case rt::AuthoringStatus::NoOp: break;
        case rt::AuthoringStatus::Stale:
          throwCommandError(Error::Code::Conflict, "library changed while preparing the track update");
        case rt::AuthoringStatus::Busy:
        case rt::AuthoringStatus::Unavailable:
          throwCommandError(Error::Code::Conflict, "library authoring is unavailable");
      }

      formatUpdateReply(
        replyRes->reply, false, static_cast<std::uint64_t>(targetIds.size()), cli.options().format, cli.io().out);
    }

    // Applies one atomic metadata-and-tag edit. There is no preview form for
    // the combined properties mutation, so the caller rejects --dry-run first.
    void updateTrackProperties(CliRuntime& cli,
                               std::vector<std::uint32_t> const& rawIds,
                               std::optional<std::string> const& optFilter,
                               rt::TrackPropertiesPatch const& patch)
    {
      auto const& ml = cli.musicLibrary();
      auto snapshot = cli.library().snapshot();
      auto const targetIds = resolveUpdateTargets(ml, snapshot, rawIds, optFilter);

      // Same zero-match rule as the metadata path; the tag side reports an
      // empty-but-present tagChanges because tag options were requested.
      if (targetIds.empty())
      {
        auto const noTagChanges = rt::EditTrackTagsReply{};
        formatUpdateReply(rt::UpdateTrackMetadataReply{}, false, 0, cli.options().format, cli.io().out, &noTagChanges);
        return;
      }

      auto const bindingRes = cli.library().bindTrackTargets(targetIds);

      if (!bindingRes)
      {
        throwCommandError(bindingRes.error());
      }

      auto const replyRes = cli.runTask(cli.library().commands().updatePropertiesAsync(*bindingRes, patch));

      if (!replyRes)
      {
        throwCommandError(replyRes.error());
      }

      switch (replyRes->status)
      {
        case rt::AuthoringStatus::Applied:
        case rt::AuthoringStatus::NoOp: break;
        case rt::AuthoringStatus::Stale:
          throwCommandError(Error::Code::Conflict, "library changed while preparing the track update");
        case rt::AuthoringStatus::Busy:
        case rt::AuthoringStatus::Unavailable:
          throwCommandError(Error::Code::Conflict, "library authoring is unavailable");
      }

      formatUpdateReply(replyRes->reply.metadata,
                        false,
                        static_cast<std::uint64_t>(targetIds.size()),
                        cli.options().format,
                        cli.io().out,
                        &replyRes->reply.tags);
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
    std::optional<std::string> optWork{};
    std::optional<std::string> optMovement{};
    std::optional<std::string> optRecordingDate{};
    std::vector<TrackCreditRecordDto> credits{};
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

    if (memberName == "optWork")
    {
      return "work";
    }

    if (memberName == "optMovement")
    {
      return "movement";
    }

    if (memberName == "optRecordingDate")
    {
      return "recordingDate";
    }

    if (memberName == "credits")
    {
      return rt::kCreditsMetadataKey;
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
    std::vector<std::string> tagNames(library::TrackView const& view, library::DictionaryStore const& dictionary)
    {
      auto names = std::vector<std::string>{};

      for (auto const tagId : view.tags())
      {
        names.emplace_back(dictionaryText(dictionary, tagId));
      }

      return names;
    }

    std::map<std::string, std::string> customMetadataByName(library::TrackView const& view,
                                                            library::DictionaryStore const& dictionary)
    {
      auto result = std::map<std::string, std::string>{};

      for (auto const& [customId, val] : view.customMetadata())
      {
        result.emplace(std::string{dictionaryText(dictionary, customId)}, val);
      }

      return result;
    }

    std::optional<std::string> dictionaryNameWhenPresent(library::DictionaryStore const& dictionary, DictionaryId id)
    {
      if (id == kInvalidDictionaryId)
      {
        return std::nullopt;
      }

      return std::string{dictionaryText(dictionary, id)};
    }

    std::optional<std::string> nonEmptyString(std::string_view value)
    {
      if (value.empty())
      {
        return std::nullopt;
      }

      return std::string{value};
    }

    std::optional<std::uint16_t> nonZeroNumber(std::uint16_t value)
    {
      if (value == 0)
      {
        return std::nullopt;
      }

      return value;
    }

    std::optional<std::uint64_t> positiveDurationMillis(library::TrackDuration duration)
    {
      if (duration.count() <= 0)
      {
        return std::nullopt;
      }

      return static_cast<std::uint64_t>(duration.count());
    }

    std::optional<std::uint32_t> nonZeroSampleRate(SampleRate value)
    {
      if (value.raw() == 0)
      {
        return std::nullopt;
      }

      return value.raw();
    }

    TrackRecordDto toTrackRecordDto(TrackId id,
                                    library::TrackView const& view,
                                    library::DictionaryStore const& dictionary)
    {
      auto dto = TrackRecordDto{.id = id};

      if (view.isHotValid())
      {
        dto.optTitle = nonEmptyString(view.metadata().title());
        dto.optArtist = dictionaryNameWhenPresent(dictionary, view.metadata().artistId());
        dto.optAlbum = dictionaryNameWhenPresent(dictionary, view.metadata().albumId());
        dto.optAlbumArtist = dictionaryNameWhenPresent(dictionary, view.metadata().albumArtistId());
        dto.optGenre = dictionaryNameWhenPresent(dictionary, view.metadata().genreId());
        dto.optComposer = dictionaryNameWhenPresent(dictionary, view.metadata().composerId());
        dto.optYear = nonZeroNumber(view.metadata().year());
        dto.optSampleRate = nonZeroSampleRate(view.property().sampleRate());
        dto.optTags = tagNames(view, dictionary);
      }

      if (view.isColdValid())
      {
        auto const work = view.work();
        auto const performance = view.performance();

        dto.optWork = dictionaryNameWhenPresent(dictionary, work.workId());
        dto.optMovement = dictionaryNameWhenPresent(dictionary, work.movementId());

        if (auto const recordingDate = performance.recordingDate(); recordingDate.isPresent())
        {
          dto.optRecordingDate = library::formatRecordingDate(recordingDate);
        }

        dto.credits.reserve(performance.credits().size());

        for (std::size_t kindIndex = 0; kindIndex < library::kCreditKindCount; ++kindIndex)
        {
          auto const kind = static_cast<library::CreditKind>(kindIndex);

          for (auto const& entry : performance.credits(kind))
          {
            dto.credits.push_back(TrackCreditRecordDto{.name = std::string{dictionaryText(dictionary, entry.nameId)},
                                                       .kind = std::string{library::creditKindToken(kind)},
                                                       .optRole = dictionaryNameWhenPresent(dictionary, entry.roleId)});
          }
        }

        dto.optTrackNumber = nonZeroNumber(view.metadata().trackNumber());
        dto.optTrackTotal = nonZeroNumber(view.metadata().trackTotal());
        dto.optDiscNumber = nonZeroNumber(view.metadata().discNumber());
        dto.optDiscTotal = nonZeroNumber(view.metadata().discTotal());
        dto.optMovementNumber = nonZeroNumber(work.movementNumber());
        dto.optMovementTotal = nonZeroNumber(work.movementTotal());
        dto.optDuration = positiveDurationMillis(view.property().duration());
        dto.optUri = nonEmptyString(view.property().uri());
        dto.optCustom = customMetadataByName(view, dictionary);
      }

      return dto;
    }

    void emitJsonTrackRecord(std::ostream& os,
                             TrackId id,
                             library::TrackView const& view,
                             library::DictionaryStore const& dictionary)
    {
      emitDocument(os, OutputFormat::Json, toTrackRecordDto(id, view, dictionary));
    }

    void formatStructuredTracks(std::vector<TrackId> const& trackIds,
                                std::size_t offset,
                                std::size_t limit,
                                library::MusicLibrary const& ml,
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
      auto const transaction = ml.readTransaction();
      auto const reader = ml.tracks().reader(transaction);
      auto const& dictionary = ml.dictionary();

      if (format == OutputFormat::Yaml)
      {
        auto dto = TrackListDto{};

        for (std::size_t i = offset; i < end; ++i)
        {
          auto const id = trackIds[i];
          auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

          if (optView)
          {
            dto.tracks.push_back(toTrackRecordDto(id, *optView, dictionary));
          }
        }

        emitDocument(os, format, dto);
        return;
      }

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const id = trackIds[i];
        auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

        if (optView)
        {
          emitJsonTrackRecord(os, id, *optView, dictionary);
        }
      }
    }

    void formatPlain(library::MusicLibrary const& ml,
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
      auto const transaction = ml.readTransaction();
      auto const reader = ml.tracks().reader(transaction);

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const id = trackIds[i];
        auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Hot);

        if (optView && optView->isHotValid())
        {
          std::println(os, "{:>5} {}", id, optView->metadata().title());
        }
      }

      if (limit > 0 && offset + limit < trackIds.size())
      {
        std::println(os, "... ({} more)", trackIds.size() - offset - limit);
      }
    }

    void show(library::MusicLibrary const& ml,
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

        auto const exprRes = query::parse(formatExpression);

        if (!exprRes)
        {
          auto const& error = exprRes.error();
          throwCommandError(error, "format error: {}{}", error.message, formatExpressionUsageHint());
        }

        auto planRes = query::compileFormat(*exprRes);

        if (!planRes)
        {
          auto const& error = planRes.error();
          throwCommandError(error, "format error: {}{}", error.message, formatExpressionUsageHint());
        }

        if (offset >= trackIds.size())
        {
          return;
        }

        auto evaluator = query::FormatEvaluator{};
        auto formattedTrack = std::string{};
        auto const transaction = ml.readTransaction();
        auto const reader = ml.tracks().reader(transaction);
        auto dictionaryCache = library::DictionaryReadCache{ml.dictionary()};
        auto dictionaryContext = library::DictionaryReadContext{dictionaryCache};
        auto binding = query::FormatBinding{*planRes, dictionaryContext};
        std::size_t const end = (limit == 0) ? trackIds.size() : std::min(offset + limit, trackIds.size());

        for (std::size_t i = offset; i < end; ++i)
        {
          auto const id = trackIds[i];
          auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

          if (optView)
          {
            if (!query::hasRequiredTrackData(planRes->accessProfile, *optView))
            {
              throwCommandError(Error::Code::CorruptData, "track {} contains invalid data required by the format", id);
            }

            evaluator.evaluate(binding, *optView, formattedTrack);
            std::println(os, "{}", formattedTrack);
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

      if (coldReader.isValid())
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

    void dumpPlainHot(library::TrackView const& view, library::DictionaryStore const& dictionary, std::ostream& os)
    {
      if (!view.isHotValid())
      {
        return;
      }

      std::println(os, "  Title: {}", view.metadata().title());
      std::println(os,
                   "  Artist: {} (ID: {})",
                   dictionaryText(dictionary, view.metadata().artistId()),
                   view.metadata().artistId());
      std::println(
        os, "  Album: {} (ID: {})", dictionaryText(dictionary, view.metadata().albumId()), view.metadata().albumId());
      std::println(os, "  Sample Rate: {}Hz", view.property().sampleRate());
      std::println(os, "  Tag Bloom: 0x{:08x}", view.tags().bloom());
      std::print(os, "  Tags: ");

      for (auto const tagId : view.tags())
      {
        std::print(os, "{} (ID: {}) ", dictionaryText(dictionary, tagId), tagId);
      }

      std::println(os);
    }

    void dumpPlainCold(library::TrackView const& view, library::DictionaryStore const& dictionary, std::ostream& os)
    {
      if (!view.isColdValid())
      {
        return;
      }

      std::println(os, "  Duration: {}ms", view.property().duration().count());
      std::println(os, "  URI: {}", view.property().uri());

      for (auto const& [customId, val] : view.customMetadata())
      {
        std::println(os, "  Custom [{}]: {}", dictionaryText(dictionary, customId), val);
      }
    }

    void dumpPlainTrack(TrackId id,
                        library::TrackView const& view,
                        library::DictionaryStore const& dictionary,
                        std::ostream& os)
    {
      std::println(os, "Track ID: {}", id);

      dumpPlainHot(view, dictionary, os);
      dumpPlainCold(view, dictionary, os);
    }

    void dumpTrack(TrackId id,
                   library::TrackView const& view,
                   library::DictionaryStore const& dictionary,
                   bool raw,
                   std::ostream& os)
    {
      if (raw)
      {
        dumpRawTrack(id, view, os);
        return;
      }

      dumpPlainTrack(id, view, dictionary, os);
    }

    void dumpTracks(library::MusicLibrary const& ml, std::uint32_t targetId, bool raw, std::ostream& os)
    {
      auto const transaction = ml.readTransaction();
      auto const reader = ml.tracks().reader(transaction);

      if (auto const& dictionary = ml.dictionary(); targetId > 0)
      {
        auto const id = TrackId{targetId};

        if (auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both); optView)
        {
          dumpTrack(id, *optView, dictionary, raw, os);
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
          dumpTrack(id, view, dictionary, raw, os);
        }
      }
    }

    void configureTrackShowCommand(CLI::App& track, CliRuntime& cli)
    {
      auto* showCmd = track.add_subcommand("show", "Show tracks by id or filter");
      auto* ids = showCmd->add_option("id", "track ids to show")->expected(0, -1);
      auto* filter = showCmd->add_option("-f,--filter", "track filter expression");
      auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
      auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);
      auto* formatExpression = showCmd->add_option("--format", "format expression, e.g. '$artist + \" - \" + $title'");
      showCmd->footer(trackShowHelpFooter());

      showCmd->callback(
        [&cli, ids, filter, limit, offset, formatExpression]
        {
          auto snapshot = cli.library().snapshot();
          auto const rawIds = ids->count() > 0 ? ids->as<std::vector<std::uint32_t>>() : std::vector<std::uint32_t>{};
          auto const optFilter = filter->count() > 0 ? std::optional{filter->as<std::string>()} : std::nullopt;
          auto const targetIds = resolveShowTargets(cli.musicLibrary(), snapshot, rawIds, optFilter);
          show(cli.musicLibrary(),
               targetIds,
               cli.options().format,
               formatExpression->count() > 0 ? formatExpression->as<std::string>() : std::string{},
               limit->as<std::size_t>(),
               offset->as<std::size_t>(),
               cli.io().out);
        });
    }

    void configureTrackCreateCommand(CLI::App& track, CliRuntime& cli)
    {
      auto* create = track.add_subcommand("create", "Create a track from a file");
      auto* path = create->add_option("path", "audio file path")->required();
      auto* dryRun = addDryRunFlag(*create);
      create->callback(
        [&cli, path, dryRun]
        {
          auto const pathValue = path->as<std::string>();
          auto const trackPath = utility::pathFromUtf8(pathValue);

          if (isDryRun(dryRun))
          {
            auto const trackRes = cli.runTask(cli.library().commands().previewCreateTrackFromFileAsync(trackPath));

            if (!trackRes)
            {
              auto const& error = trackRes.error();
              throwCommandError(error, "error adding track from: {}: {}", pathValue, error.message);
            }

            formatTrackCreate(
              std::nullopt, trackRes->uri, trackRes->title, trackRes->artist, true, cli.options().format, cli.io().out);
            return;
          }

          auto const trackRes = cli.runTask(cli.library().commands().createTrackFromFileAsync(trackPath));

          if (trackRes)
          {
            formatTrackCreate(std::optional<TrackId>{trackRes->trackId},
                              trackRes->uri,
                              trackRes->title,
                              trackRes->artist,
                              false,
                              cli.options().format,
                              cli.io().out);
          }
          else
          {
            auto const& error = trackRes.error();
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
      CLI::Option* work = nullptr;
      CLI::Option* movement = nullptr;
      CLI::Option* recordingDate = nullptr;
      CLI::Option* credit = nullptr;
      CLI::Option* creditScope = nullptr;
      CLI::Option* clearCredits = nullptr;
      CLI::Option* year = nullptr;
      CLI::Option* trackNumber = nullptr;
      CLI::Option* trackTotal = nullptr;
      CLI::Option* discNumber = nullptr;
      CLI::Option* discTotal = nullptr;
      CLI::Option* movementNumber = nullptr;
      CLI::Option* movementTotal = nullptr;
      CLI::Option* set = nullptr;
      CLI::Option* unset = nullptr;
      CLI::Option* addTag = nullptr;
      CLI::Option* removeTag = nullptr;
      CLI::Option* dryRun = nullptr;
      std::shared_ptr<std::vector<std::string>> creditValuesPtr;
      std::shared_ptr<std::vector<std::string>> setsPtr;
      std::shared_ptr<std::vector<std::string>> unsetsPtr;
      std::shared_ptr<std::vector<std::string>> addTagsPtr;
      std::shared_ptr<std::vector<std::string>> removeTagsPtr;
    };

    // Surrounding ASCII whitespace is trimmed exactly like the shared edit
    // paths, so the strict recording-date parser only ever sees canonical
    // text, and an empty option value is an explicit clear.
    void applyRecordingDateOption(CLI::Option const* option, rt::MetadataPatch& patch)
    {
      auto const value = option->as<std::string>();
      auto const trimmed = utility::trim(value);

      if (trimmed.empty())
      {
        patch.optRecordingDate = library::RecordingDate{};
        return;
      }

      auto const dateRes = library::parseRecordingDate(trimmed);

      if (!dateRes)
      {
        throwCommandError(
          Error::Code::InvalidInput, "invalid --recording-date value '{}': {}", value, dateRes.error().message);
      }

      patch.optRecordingDate = *dateRes;
    }

    void applyCreditsOptions(TrackUpdateCliOptions const& options, rt::MetadataPatch& patch)
    {
      bool const clearRequested = options.clearCredits->count() > 0;
      bool const replacementRequested = options.credit->count() > 0;
      bool const hasScope = options.creditScope->count() > 0;

      if (clearRequested && replacementRequested)
      {
        throwCommandError(Error::Code::InvalidInput, "--credit and --clear-credits cannot be combined");
      }

      if (!clearRequested && !replacementRequested)
      {
        if (hasScope)
        {
          throwCommandError(Error::Code::InvalidInput, "--credit-scope requires --credit or --clear-credits");
        }

        return;
      }

      auto replacement = rt::CreditReplacement{};

      if (!hasScope)
      {
        replacement.kinds.set();
      }

      for (auto const& token : options.creditScope->results())
      {
        auto const optKind = library::parseCreditKind(token);

        if (!optKind)
        {
          throwCommandError(Error::Code::InvalidInput, "invalid --credit-scope kind '{}'", token);
        }

        replacement.kinds.set(static_cast<std::size_t>(*optKind));
      }

      // These values bypass CLI11's list and doubled-bracket decoding.
      auto const& values = *options.creditValuesPtr;
      auto entries = std::vector<library::CreditView>{};

      for (std::size_t index = 0; index < values.size(); index += 3)
      {
        auto const optKind = library::parseCreditKind(values[index]);

        if (!optKind)
        {
          throwCommandError(Error::Code::InvalidInput, "invalid --credit kind '{}'", values[index]);
        }

        if (!replacement.kinds.test(static_cast<std::size_t>(*optKind)))
        {
          throwCommandError(Error::Code::InvalidInput, "--credit kind '{}' is outside --credit-scope", values[index]);
        }

        entries.push_back(library::CreditView{.name = values[index + 1], .kind = *optKind, .role = values[index + 2]});
      }

      auto creditsRes = library::normalizeCredits(entries);

      if (!creditsRes)
      {
        throwCommandError(Error::Code::InvalidInput, "invalid --credit value: {}", creditsRes.error().message);
      }

      replacement.entries = std::move(*creditsRes);
      patch.optCredits = std::move(replacement);
    }

    bool tryApplyTrackUpdateFieldOptions(TrackUpdateCliOptions const& options, rt::MetadataPatch& patch)
    {
      bool hasPatch = false;
      bool const hasRecordingDate = options.recordingDate->count() > 0;
      bool const hasCredits = options.credit->count() > 0 || options.clearCredits->count() > 0;

      if (hasRecordingDate)
      {
        applyRecordingDateOption(options.recordingDate, patch);
      }

      applyCreditsOptions(options, patch);
      hasPatch = hasRecordingDate || hasCredits || hasPatch;
      hasPatch = tryAssignStringOption(options.title, patch.optTitle) || hasPatch;
      hasPatch = tryAssignStringOption(options.artist, patch.optArtist) || hasPatch;
      hasPatch = tryAssignStringOption(options.album, patch.optAlbum) || hasPatch;
      hasPatch = tryAssignStringOption(options.albumArtist, patch.optAlbumArtist) || hasPatch;
      hasPatch = tryAssignStringOption(options.genre, patch.optGenre) || hasPatch;
      hasPatch = tryAssignStringOption(options.composer, patch.optComposer) || hasPatch;
      hasPatch = tryAssignStringOption(options.work, patch.optWork) || hasPatch;
      hasPatch = tryAssignStringOption(options.movement, patch.optMovement) || hasPatch;
      hasPatch = tryAssignUint16Option(options.year, patch.optYear) || hasPatch;
      hasPatch = tryAssignUint16Option(options.trackNumber, patch.optTrackNumber) || hasPatch;
      hasPatch = tryAssignUint16Option(options.trackTotal, patch.optTrackTotal) || hasPatch;
      hasPatch = tryAssignUint16Option(options.discNumber, patch.optDiscNumber) || hasPatch;
      hasPatch = tryAssignUint16Option(options.discTotal, patch.optDiscTotal) || hasPatch;
      hasPatch = tryAssignUint16Option(options.movementNumber, patch.optMovementNumber) || hasPatch;
      hasPatch = tryAssignUint16Option(options.movementTotal, patch.optMovementTotal) || hasPatch;
      return hasPatch;
    }

    bool tryApplyTrackUpdateCustomOptions(TrackUpdateCliOptions const& options, rt::MetadataPatch& patch)
    {
      bool hasPatch = false;

      if (options.set->count() > 0)
      {
        for (auto const& assignment : *options.setsPtr)
        {
          hasPatch = tryApplyCustomSet(assignment, patch) || hasPatch;
        }
      }

      if (options.unset->count() > 0)
      {
        for (auto const& key : *options.unsetsPtr)
        {
          hasPatch = tryApplyCustomUnset(key, patch) || hasPatch;
        }
      }

      return hasPatch;
    }

    void runTrackUpdateCommand(CliRuntime& cli, TrackUpdateCliOptions const& options)
    {
      auto patch = rt::MetadataPatch{};
      bool hasPatch = tryApplyTrackUpdateFieldOptions(options, patch);
      hasPatch = tryApplyTrackUpdateCustomOptions(options, patch) || hasPatch;

      auto tagsToAdd = std::vector<std::string>{};
      auto tagsToRemove = std::vector<std::string>{};

      if (options.addTag->count() > 0)
      {
        tagsToAdd = *options.addTagsPtr;
      }

      if (options.removeTag->count() > 0)
      {
        tagsToRemove = *options.removeTagsPtr;
      }

      bool const hasTags = !tagsToAdd.empty() || !tagsToRemove.empty();

      if (!hasPatch && !hasTags)
      {
        throwCommandError(Error::Code::InvalidInput, "track update requires at least one field or tag option");
      }

      auto const dryRun = isDryRun(options.dryRun);

      if (hasTags && dryRun)
      {
        throwCommandError(Error::Code::InvalidInput, "--dry-run does not support tag changes yet");
      }

      auto const rawIds =
        options.ids->count() > 0 ? options.ids->as<std::vector<std::uint32_t>>() : std::vector<std::uint32_t>{};
      auto const optFilter =
        options.filter->count() > 0 ? std::optional{options.filter->as<std::string>()} : std::nullopt;

      if (!hasTags)
      {
        updateTracks(cli, rawIds, optFilter, patch, dryRun);
        return;
      }

      updateTrackProperties(
        cli,
        rawIds,
        optFilter,
        rt::TrackPropertiesPatch{
          .metadata = patch, .tagsToAdd = std::move(tagsToAdd), .tagsToRemove = std::move(tagsToRemove)});
    }

    // CLI11's protected subclassing surface keeps optional vector operands on
    // the native classifier, including numeric short-option and parent context.
    class TrackUpdateCommand final : public CLI::App
    {
    public:
      explicit TrackUpdateCommand(CLI::App& track)
        : CLI::App{"Update track metadata and tags", "update", &track}
      {
      }

      CLI::detail::Classifier classifyOperand(std::string const& argument) const { return _recognize(argument, false); }
    };

    [[noreturn]] void throwLiteralArgumentError(std::string message)
    {
      AO_EXCEPTION_CARRIER(ForeignCallbackAdapter);
      throw CLI::ArgumentMismatch{std::move(message)};
    }

    std::string_view recognizedLiteralToken(std::vector<std::string> const& args,
                                            std::span<std::string const> originalArgs)
    {
      // CLI11 2.6.2 App::_parse_arg pops the recognized long token before its
      // immediate zero-arity callback. Child parsing uses the same vector.
      // Short-option remainders are processed before the next long token;
      // none of these literal options has a short spelling. Thus the untouched
      // remaining prefix ends immediately before this original argv slot.
      AO_INVARIANT(args.size() < originalArgs.size(), "recognized literal option must have an original argv slot");
      return originalArgs[args.size()];
    }

    CLI::Option* addCreditOption(CLI::App& update,
                                 std::vector<std::string>& args,
                                 std::span<std::string const> originalArgs,
                                 std::shared_ptr<std::vector<std::string>> const& creditValuesPtr)
    {
      // Zero arity delegates recognition to CLI11, not operand transport. The
      // raw callback deliberately ignores flag results and boolean conversion.
      return update
        .add_option(
          "--credit",
          CLI::callback_t{[&args, originalArgs, creditValuesPtr](CLI::results_t const&)
                          {
                            if (recognizedLiteralToken(args, originalArgs).contains('='))
                            {
                              throwLiteralArgumentError("--credit does not accept attached values; use KIND NAME ROLE");
                            }

                            if (args.size() < 3)
                            {
                              throwLiteralArgumentError("--credit requires exactly three arguments: KIND NAME ROLE");
                            }

                            for (std::size_t index = 0; index < 3; ++index)
                            {
                              creditValuesPtr->push_back(std::move(args.back()));
                              args.pop_back();
                            }

                            return true;
                          }},
          "KIND NAME ROLE; repeatable scoped replacement; empty role means absent")
        ->expected(0)
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->trigger_on_parse();
    }

    CLI::Option* addLiteralVectorOption(TrackUpdateCommand& update,
                                        std::string name,
                                        std::string description,
                                        std::vector<std::string>& args,
                                        std::span<std::string const> originalArgs,
                                        std::shared_ptr<std::vector<std::string>> const& valuesPtr)
    {
      // The command tree owns update. Callbacks borrow it and invocation-owned
      // argv storage; no callback retains its owning App through shared_ptr.
      return update
        .add_option(name,
                    CLI::callback_t{[&update, name, &args, originalArgs, valuesPtr](CLI::results_t const&)
                                    {
                                      auto const token = recognizedLiteralToken(args, originalArgs);

                                      if (auto const separator = token.find('='); separator != std::string_view::npos)
                                      {
                                        valuesPtr->emplace_back(token.substr(separator + 1));
                                      }
                                      else
                                      {
                                        if (args.empty())
                                        {
                                          throwLiteralArgumentError(name + " requires at least one argument");
                                        }

                                        // Like CLI11's vector minimum, the first operand is mandatory
                                        // even if option-like. Only additional operands are classified.
                                        valuesPtr->push_back(std::move(args.back()));
                                        args.pop_back();
                                      }

                                      // Track update has no required positionals, optional validators or
                                      // operand limit. This is CLI11's native optional-vector loop.
                                      while (!args.empty() &&
                                             update.classifyOperand(args.back()) == CLI::detail::Classifier::NONE)
                                      {
                                        valuesPtr->push_back(std::move(args.back()));
                                        args.pop_back();
                                      }

                                      // CLI11 consumes the delimiter ending an unlimited vector, then
                                      // resumes ordinary parsing; it is not a literal optional operand.
                                      if (!args.empty() && update.classifyOperand(args.back()) ==
                                                             CLI::detail::Classifier::POSITIONAL_MARK)
                                      {
                                        args.pop_back();
                                      }

                                      return true;
                                    }},
                    std::move(description))
        ->expected(0)
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->trigger_on_parse();
    }

    void configureTrackUpdateCommand(CLI::App& track,
                                     CliRuntime& cli,
                                     std::vector<std::string>& args,
                                     std::span<std::string const> originalArgs)
    {
      auto updatePtr = std::make_shared<TrackUpdateCommand>(track);
      auto* update = updatePtr.get();
      track.add_subcommand(std::move(updatePtr));
      update->footer(trackUpdateHelpFooter() +
                     "\nWarning: --dry-run in NAME or ROLE is literal text, not a preview flag. "
                     "Place --dry-run before --credit, or after an explicit empty role.\n"
                     "  aobus track update 42 --dry-run --credit performer 'Preview only' ''\n"
                     "Attached --credit=VALUE syntax is not supported.");
      auto creditValuesPtr = std::make_shared<std::vector<std::string>>();
      auto updateSetsPtr = std::make_shared<std::vector<std::string>>();
      auto updateUnsetsPtr = std::make_shared<std::vector<std::string>>();
      auto updateAddTagsPtr = std::make_shared<std::vector<std::string>>();
      auto updateRemoveTagsPtr = std::make_shared<std::vector<std::string>>();
      auto options = TrackUpdateCliOptions{
        .ids = update->add_option("id", "track id to update")->expected(0, -1),
        .filter = update->add_option("-f,--filter", "track filter expression"),
        .title = update->add_option("--title", "title"),
        .artist = update->add_option("--artist", "artist"),
        .album = update->add_option("--album", "album"),
        .albumArtist = update->add_option("--album-artist", "album artist"),
        .genre = update->add_option("--genre", "genre"),
        .composer = update->add_option("--composer", "composer"),
        .work = update->add_option("--work", "work"),
        .movement = update->add_option("--movement", "movement"),
        .recordingDate =
          update->add_option("--recording-date", "recording date (YYYY, YYYY-MM, or YYYY-MM-DD; empty clears)"),
        .credit = addCreditOption(*update, args, originalArgs, creditValuesPtr),
        .creditScope = update
                         ->add_option("--credit-scope",
                                      "conductor, ensemble, soloist, or performer; repeatable union; default all kinds")
                         ->expected(1)
                         ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll),
        .clearCredits = update->add_flag("--clear-credits", "clear credits in the selected scope"),
        .year = update->add_option("--year", "year"),
        .trackNumber = update->add_option("--track-number", "track number"),
        .trackTotal = update->add_option("--track-total", "track total"),
        .discNumber = update->add_option("--disc-number", "disc number"),
        .discTotal = update->add_option("--disc-total", "disc total"),
        .movementNumber = update->add_option("--movement-number", "movement number"),
        .movementTotal = update->add_option("--movement-total", "movement total"),
        .set = addLiteralVectorOption(
          *update, "--set", "key=value ...; repeatable literal assignments", args, originalArgs, updateSetsPtr),
        .unset = addLiteralVectorOption(
          *update, "--unset", "key ...; repeatable literal keys", args, originalArgs, updateUnsetsPtr),
        .addTag = addLiteralVectorOption(
          *update, "--add-tag", "tag ...; repeatable literal tags to add", args, originalArgs, updateAddTagsPtr),
        .removeTag = addLiteralVectorOption(*update,
                                            "--remove-tag",
                                            "tag ...; repeatable literal tags to remove",
                                            args,
                                            originalArgs,
                                            updateRemoveTagsPtr),
        .dryRun = addDryRunFlag(*update),
        .creditValuesPtr = creditValuesPtr,
        .setsPtr = updateSetsPtr,
        .unsetsPtr = updateUnsetsPtr,
        .addTagsPtr = updateAddTagsPtr,
        .removeTagsPtr = updateRemoveTagsPtr,
      };

      update->callback([&cli, options] { runTrackUpdateCommand(cli, options); });
    }

    void configureTrackDeleteCommand(CLI::App& track, CliRuntime& cli)
    {
      auto* del = track.add_subcommand("delete", "Delete a track by id");
      auto* id = del->add_option("id", "track id")->required();
      auto* dryRun = addDryRunFlag(*del);
      del->callback(
        [&cli, id, dryRun]
        {
          auto const trackId = TrackId{id->as<std::uint32_t>()};

          if (isDryRun(dryRun))
          {
            auto const deleteRes = cli.runTask(cli.library().commands().previewDeleteTrackAsync(trackId));

            if (!deleteRes)
            {
              throwCommandError(deleteRes.error());
            }

            formatTrackDelete(*deleteRes, true, cli.options().format, cli.io().out);
            return;
          }

          auto const deleteRes = cli.runTask(cli.library().commands().deleteTrackAsync(trackId));

          if (deleteRes)
          {
            formatTrackDelete(*deleteRes, false, cli.options().format, cli.io().out);
          }
          else
          {
            throwCommandError(deleteRes.error());
          }
        });
    }

    void configureTrackDumpCommand(CLI::App& track, CliRuntime& cli)
    {
      auto* dumpCmd = track.add_subcommand("dump", "Dump tracks from database");
      auto* dumpId = dumpCmd->add_option("--id", "track id to dump");
      auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");

      dumpCmd->callback(
        [&cli, dumpId, dumpRaw]
        {
          if (cli.options().format != OutputFormat::Plain)
          {
            throwCommandError(Error::Code::InvalidInput,
                              "track dump supports only plain output; use track show -O yaml/json for structured data");
          }

          dumpTracks(cli.musicLibrary(),
                     dumpId->count() > 0 ? dumpId->as<std::uint32_t>() : 0,
                     dumpRaw->count() > 0,
                     cli.io().out);
        });
    }
  } // namespace

  void configureTrackCommand(CLI::App& app,
                             CliRuntime& cli,
                             std::vector<std::string>& args,
                             std::span<std::string const> originalArgs)
  {
    auto* track = app.add_subcommand("track", "Track management commands");
    track->footer(trackHelpFooter());
    track->require_subcommand(1);
    configureTrackShowCommand(*track, cli);
    configureTrackCreateCommand(*track, cli);
    configureTrackUpdateCommand(*track, cli, args, originalArgs);
    configureTrackDeleteCommand(*track, cli);
    configureTrackDumpCommand(*track, cli);
  }
} // namespace ao::cli
