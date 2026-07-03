// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "DumpUtils.h"
#include "Output.h"
#include "TrackSelection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>

#include <CLI/App.hpp>
#include <CLI/Option.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
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

    void formatUpdateReply(rt::UpdateTrackMetadataReply const& reply, OutputFormat format, std::ostream& os)
    {
      auto const updated = reply.mutatedIds.size();

      if (format == OutputFormat::Yaml)
      {
        std::println(os, "updated: {}", updated);

        if (reply.mutatedIds.empty())
        {
          std::println(os, "trackIds: []");
          return;
        }

        std::println(os, "trackIds:");

        for (auto const trackId : reply.mutatedIds)
        {
          std::println(os, "  - {}", trackId.raw());
        }

        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.uintField("updated", static_cast<std::uint64_t>(updated));
        object.field("trackIds");
        auto array = JsonArray{os};

        for (auto const mutatedId : reply.mutatedIds)
        {
          array.element();
          std::print(os, "{}", mutatedId.raw());
        }

        array.close();
        object.close();
        std::println(os);
        return;
      }

      std::println(os, "updated {} track(s)", updated);
    }

    void formatTrackMutation(std::string_view action,
                             TrackId trackId,
                             std::string_view path,
                             OutputFormat format,
                             std::ostream& os)
    {
      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "action", action);
        yamlKeyValue(os, 0, "trackId", static_cast<std::uint64_t>(trackId.raw()));

        if (!path.empty())
        {
          yamlKeyValue(os, 0, "path", path);
        }

        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.stringField("action", action);
        object.uintField("trackId", trackId.raw());

        if (!path.empty())
        {
          object.stringField("path", path);
        }

        object.close();
        std::println(os);
        return;
      }

      if (action == "create")
      {
        std::println(os, "added track: {}", trackId);
      }
      else
      {
        std::println(os, "deleted track: {}", trackId);
      }
    }

    void updateTracks(CliContext& context,
                      std::vector<std::uint32_t> const& rawIds,
                      std::string const& filter,
                      rt::MetadataPatch const& patch)
    {
      auto& ml = context.musicLibrary();
      auto reader = context.library().reader();
      auto const targetIds = resolveUpdateTargets(ml, reader, rawIds, filter);
      auto const replyResult = context.library().writer().updateMetadata(targetIds, patch);

      if (!replyResult)
      {
        throwCommandError(replyResult.error());
      }

      formatUpdateReply(*replyResult, context.options().format, context.io().out);
    }

    template<typename TRange>
    void formatYamlStringList(std::ostream& os, std::int32_t indent, std::string_view key, TRange const& values)
    {
      if (values.empty())
      {
        std::println(os, "{}{}: []", std::string(static_cast<std::size_t>(indent), ' '), key);
        return;
      }

      std::println(os, "{}{}:", std::string(static_cast<std::size_t>(indent), ' '), key);

      for (auto const& value : values)
      {
        std::println(os, "{}- {}", std::string(static_cast<std::size_t>(indent + 2), ' '), yamlQuote(value));
      }
    }

    std::vector<std::string_view> tagNames(library::TrackView const& view, library::DictionaryStore const& dict)
    {
      auto names = std::vector<std::string_view>{};

      for (auto const tagId : view.tags())
      {
        names.emplace_back(resolveDict(dict, tagId));
      }

      return names;
    }

    void formatYamlCustomMap(std::ostream& os,
                             std::int32_t indent,
                             library::TrackView const& view,
                             library::DictionaryStore const& dict)
    {
      if (view.customMetadata().empty())
      {
        std::println(os, "{}custom: {{}}", std::string(static_cast<std::size_t>(indent), ' '));
        return;
      }

      std::println(os, "{}custom:", std::string(static_cast<std::size_t>(indent), ' '));

      for (auto const& [customId, val] : view.customMetadata())
      {
        std::println(os,
                     "{}{}: {}",
                     std::string(static_cast<std::size_t>(indent + 2), ' '),
                     yamlQuote(resolveDict(dict, customId)),
                     yamlQuote(val));
      }
    }

    void formatYamlTrackRecord(std::ostream& os,
                               TrackId id,
                               library::TrackView const& view,
                               library::DictionaryStore const& dict)
    {
      std::println(os, "  - id: {}", id);

      if (view.isHotValid())
      {
        yamlKeyValue(os, 4, "title", view.metadata().title());
        yamlKeyValue(os, 4, "artist", resolveDict(dict, view.metadata().artistId()));
        yamlKeyValue(os, 4, "album", resolveDict(dict, view.metadata().albumId()));
        formatYamlStringList(os, 4, "tags", tagNames(view, dict));
      }

      if (view.isColdValid())
      {
        yamlKeyValue(os, 4, "duration", static_cast<std::uint64_t>(view.property().duration().count()));
        yamlKeyValue(os, 4, "sampleRate", static_cast<std::uint64_t>(view.property().sampleRate().raw()));
        yamlKeyValue(os, 4, "uri", view.property().uri());
        formatYamlCustomMap(os, 4, view, dict);
      }
    }

    void jsonStringArray(std::ostream& os, std::vector<std::string_view> const& values)
    {
      auto array = JsonArray{os};

      for (auto const value : values)
      {
        array.element();
        std::print(os, "{}", jsonQuote(value));
      }
    }

    void formatJsonCustomMap(std::ostream& os, library::TrackView const& view, library::DictionaryStore const& dict)
    {
      auto object = JsonObject{os};

      for (auto const& [customId, val] : view.customMetadata())
      {
        object.field(resolveDict(dict, customId));
        std::print(os, "{}", jsonQuote(val));
      }
    }

    void formatJsonTrackRecord(std::ostream& os,
                               TrackId id,
                               library::TrackView const& view,
                               library::DictionaryStore const& dict)
    {
      auto object = JsonObject{os};
      object.uintField("id", id.raw());

      if (view.isHotValid())
      {
        object.stringField("title", view.metadata().title());
        object.stringField("artist", resolveDict(dict, view.metadata().artistId()));
        object.stringField("album", resolveDict(dict, view.metadata().albumId()));
        object.field("tags");
        jsonStringArray(os, tagNames(view, dict));
      }

      if (view.isColdValid())
      {
        object.uintField("duration", static_cast<std::uint64_t>(view.property().duration().count()));
        object.uintField("sampleRate", view.property().sampleRate().raw());
        object.stringField("uri", view.property().uri());
        object.field("custom");
        formatJsonCustomMap(os, view, dict);
      }

      object.close();
      std::println(os);
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
          std::println(os, "tracks: []");
        }

        return;
      }

      std::size_t const end = (limit == 0) ? trackIds.size() : std::min(offset + limit, trackIds.size());
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);
      auto const& dict = ml.dictionary();

      if (format == OutputFormat::Yaml)
      {
        std::println(os, "tracks:");
      }

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const id = trackIds[i];
        auto const optView = rt::storageValueOrNullopt(
          reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to read track");

        if (!optView)
        {
          continue;
        }

        if (format == OutputFormat::Yaml)
        {
          formatYamlTrackRecord(os, id, *optView, dict);
        }
        else
        {
          formatJsonTrackRecord(os, id, *optView, dict);
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
              std::string const& filter,
              OutputFormat format,
              std::string const& formatExpression,
              std::size_t limit,
              std::size_t offset,
              std::ostream& os)
    {
      auto const trackIds = queryMatchingTrackIds(ml, filter);

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
          throwCommandError(error, "format error: {}", error.message);
        }

        auto plan = query::compileFormat(*expr, &ml.dictionary());

        if (!plan)
        {
          auto const& error = plan.error();
          throwCommandError(error, "format error: {}", error.message);
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

    void processTrackDump(TrackId id,
                          library::TrackView const& view,
                          library::DictionaryStore const& dict,
                          bool raw,
                          std::ostream& os)
    {
      if (raw)
      {
        std::println(os, "Track ID: {}", id);

        if (view.isHotValid())
        {
          std::println(os, "Hot Header:");
          hexDump(view.hotData().subspan(0, sizeof(library::TrackHotHeader)), os);
          std::println(os, "Hot Payload:");

          if (view.hotData().size() > sizeof(library::TrackHotHeader))
          {
            hexDump(view.hotData().subspan(sizeof(library::TrackHotHeader)), os);
          }
        }

        if (view.isColdValid())
        {
          std::println(os, "Cold Header:");
          hexDump(view.coldData().subspan(0, sizeof(library::TrackColdHeader)), os);
          std::println(os, "Cold Payload:");

          if (view.coldData().size() > sizeof(library::TrackColdHeader))
          {
            hexDump(view.coldData().subspan(sizeof(library::TrackColdHeader)), os);
          }
        }
      }
      else
      {
        std::println(os, "Track ID: {}", id);

        if (view.isHotValid())
        {
          std::println(os, "  Title: {}", view.metadata().title());
          std::println(
            os, "  Artist: {} (ID: {})", resolveDict(dict, view.metadata().artistId()), view.metadata().artistId());
          std::println(
            os, "  Album: {} (ID: {})", resolveDict(dict, view.metadata().albumId()), view.metadata().albumId());
          std::println(os, "  Tag Bloom: 0x{:08x}", view.tags().bloom());
          std::print(os, "  Tags: ");

          for (auto const tagId : view.tags())
          {
            std::print(os, "{} (ID: {}) ", resolveDict(dict, tagId), tagId);
          }

          std::println(os);
        }

        if (view.isColdValid())
        {
          std::println(os, "  Duration: {}ms", view.property().duration().count());
          std::println(os, "  Sample Rate: {}Hz", view.property().sampleRate());
          std::println(os, "  URI: {}", view.property().uri());

          for (auto const& [customId, val] : view.customMetadata())
          {
            std::println(os, "  Custom [{}]: {}", resolveDict(dict, customId), val);
          }
        }
      }
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
      auto* showCmd = track.add_subcommand("show", "Show tracks matching a filter");
      auto* filter = showCmd->add_option("filter,-f,--filter", "track filter expression");
      auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
      auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);
      auto* formatExpression = showCmd->add_option("--format", "format expression");

      showCmd->callback(
        [&context, filter, limit, offset, formatExpression]
        {
          show(context.musicLibrary(),
               filter->as<std::string>(),
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
      create->callback(
        [&context, path]
        {
          auto const trackResult = context.library().writer().createTrackFromFile(path->as<std::string>());

          if (trackResult)
          {
            formatTrackMutation(
              "create", *trackResult, path->as<std::string>(), context.options().format, context.io().out);
          }
          else
          {
            auto const& error = trackResult.error();
            throwCommandError(error, "error adding track from: {}: {}", path->as<std::string>(), error.message);
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
      CLI::Option* year = nullptr;
      CLI::Option* trackNumber = nullptr;
      CLI::Option* trackTotal = nullptr;
      CLI::Option* discNumber = nullptr;
      CLI::Option* discTotal = nullptr;
      CLI::Option* movementNumber = nullptr;
      CLI::Option* movementTotal = nullptr;
      CLI::Option* set = nullptr;
      CLI::Option* unset = nullptr;
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
      hasPatch = assignStringOption(options.work, patch.optWork) || hasPatch;
      hasPatch = assignStringOption(options.movement, patch.optMovement) || hasPatch;
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
      updateTracks(context, rawIds, filter, patch);
    }

    void setupTrackUpdateCommand(CLI::App& track, CliContext& context)
    {
      auto* update = track.add_subcommand("update", "Update track metadata");
      auto updateSetsPtr = std::make_shared<std::vector<std::string>>();
      auto updateUnsetsPtr = std::make_shared<std::vector<std::string>>();
      auto options = TrackUpdateCliOptions{
        .ids = update->add_option("id", "track id to update"),
        .filter = update->add_option("-f,--filter", "track filter expression"),
        .title = update->add_option("--title", "title"),
        .artist = update->add_option("--artist", "artist"),
        .album = update->add_option("--album", "album"),
        .albumArtist = update->add_option("--album-artist", "album artist"),
        .genre = update->add_option("--genre", "genre"),
        .composer = update->add_option("--composer", "composer"),
        .work = update->add_option("--work", "work"),
        .movement = update->add_option("--movement", "movement"),
        .year = update->add_option("--year", "year"),
        .trackNumber = update->add_option("--track-number", "track number"),
        .trackTotal = update->add_option("--track-total", "track total"),
        .discNumber = update->add_option("--disc-number", "disc number"),
        .discTotal = update->add_option("--disc-total", "disc total"),
        .movementNumber = update->add_option("--movement-number", "movement number"),
        .movementTotal = update->add_option("--movement-total", "movement total"),
        .set = update->add_option("--set", *updateSetsPtr, "set custom metadata key=value"),
        .unset = update->add_option("--unset", *updateUnsetsPtr, "unset custom metadata key"),
        .setsPtr = updateSetsPtr,
        .unsetsPtr = updateUnsetsPtr,
      };

      update->callback([&context, options] { runTrackUpdateCommand(context, options); });
    }

    void setupTrackDeleteCommand(CLI::App& track, CliContext& context)
    {
      auto* del = track.add_subcommand("delete", "Delete a track by id");
      auto* id = del->add_option("id", "track id")->required();
      del->callback(
        [&context, id]
        {
          if (auto const trackId = TrackId{id->as<std::uint32_t>()}; context.library().writer().deleteTrack(trackId))
          {
            formatTrackMutation("delete", trackId, {}, context.options().format, context.io().out);
          }
          else
          {
            throwCommandError(Error::Code::NotFound, "track not found: {}", trackId);
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
    track->require_subcommand(1);
    setupTrackShowCommand(*track, context);
    setupTrackCreateCommand(*track, context);
    setupTrackUpdateCommand(*track, context);
    setupTrackDeleteCommand(*track, context);
    setupTrackDumpCommand(*track, context);
  }
} // namespace ao::cli
