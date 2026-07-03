// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackCommand.h"

#include "DumpUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/query/QueryCompiler.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/StorageResult.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <CLI/App.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace ao::cli
{
  namespace
  {
    std::vector<std::pair<TrackId, library::TrackView>> collectTracks(library::MusicLibrary& ml,
                                                                      std::string const& filter)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);
      auto matches = std::vector<std::pair<TrackId, library::TrackView>>{};

      if (filter.empty())
      {
        for (auto const& [id, view] : reader)
        {
          matches.emplace_back(id, view);
        }

        return matches;
      }

      auto const expr = query::parse(filter);

      if (!expr)
      {
        std::println(stderr, "filter error: {}", expr.error().message);
        return matches;
      }

      auto const plan = query::compileQuery(*expr, &ml.dictionary());

      if (!plan)
      {
        std::println(stderr, "filter error: {}", plan.error().message);
        return matches;
      }

      auto evaluator = query::PlanEvaluator{};

      for (auto const& [id, view] : reader)
      {
        if (evaluator.matches(*plan, view))
        {
          matches.emplace_back(id, view);
        }
      }

      return matches;
    }

    void formatYaml(std::vector<std::pair<TrackId, library::TrackView>> const& matches,
                    std::size_t offset,
                    std::size_t limit,
                    library::MusicLibrary& ml,
                    std::ostream& os)
    {
      if (offset >= matches.size())
      {
        std::println(os, "tracks: []");
        return;
      }

      std::size_t const end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());
      std::println(os, "tracks:");

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        std::println(os, "  - id: {}", id);
        std::println(os, "    title: \"{}\"", view.metadata().title());

        if (view.metadata().artistId() > 0)
        {
          std::println(os, "    artist: \"{}\"", ml.dictionary().get(view.metadata().artistId()));
        }

        if (view.metadata().albumId() > 0)
        {
          std::println(os, "    album: \"{}\"", ml.dictionary().get(view.metadata().albumId()));
        }
      }
    }

    void formatPlain(std::vector<std::pair<TrackId, library::TrackView>> const& matches,
                     std::size_t offset,
                     std::size_t limit,
                     std::ostream& os)
    {
      if (offset >= matches.size())
      {
        return;
      }

      std::size_t const end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        std::println(os, "{:>5} {}", id, view.metadata().title());
      }

      if (limit > 0 && offset + limit < matches.size())
      {
        std::println(os, "... ({} more)", matches.size() - offset - limit);
      }
    }

    void show(library::MusicLibrary& ml,
              std::string const& filter,
              bool yaml,
              std::size_t limit,
              std::size_t offset,
              std::ostream& os)
    {
      if (auto const matches = collectTracks(ml, filter); yaml)
      {
        formatYaml(matches, offset, limit, ml, os);
      }
      else
      {
        formatPlain(matches, offset, limit, os);
      }
    }

    void processTrackDump(TrackId id,
                          library::TrackView const& view,
                          library::DictionaryStore const& dict,
                          bool raw,
                          bool yaml,
                          std::ostream& os)
    {
      if (yaml)
      {
        std::println(os, "  - id: {}", id);

        if (view.isHotValid())
        {
          std::println(os, "    title: \"{}\"", view.metadata().title());
          std::println(os, "    artist: \"{}\"", resolveDict(dict, view.metadata().artistId()));
          std::println(os, "    album: \"{}\"", resolveDict(dict, view.metadata().albumId()));
        }

        if (view.isColdValid())
        {
          std::println(os, "    duration: {}", view.property().duration().count());
          std::println(os, "    sampleRate: {}", view.property().sampleRate());
          std::println(os, "    uri: \"{}\"", view.property().uri());
        }
      }
      else if (raw)
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

          std::println(os, "");
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

    void dumpTracks(library::MusicLibrary& ml, std::uint32_t targetId, bool raw, bool yaml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.tracks().reader(txn);
      auto const& dict = ml.dictionary();

      if (yaml)
      {
        std::println(os, "tracks:");
      }

      if (targetId > 0)
      {
        auto const id = TrackId{targetId};

        if (auto const optView = rt::storageValueOrNullopt(
              reader.get(id, library::TrackStore::Reader::LoadMode::Both), "Failed to dump track");
            optView)
        {
          processTrackDump(id, *optView, dict, raw, yaml, os);
        }
        else
        {
          if (!yaml)
          {
            std::println(os, "Track {} not found.", targetId);
          }
        }
      }
      else
      {
        for (auto const& [id, view] : reader)
        {
          processTrackDump(id, view, dict, raw, yaml, os);
        }
      }
    }
  } // namespace

  void setupTrackCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* track = app.add_subcommand("track", "Track management commands");
    auto* showCmd = track->add_subcommand("show", "Show tracks matching a filter");
    auto* filter = showCmd->add_option("filter,-f,--filter", "track filter expression");
    auto* yaml = showCmd->add_flag("-y,--yaml", "output as YAML");
    auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
    auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);

    showCmd->callback(
      [&runtime, filter, yaml, limit, offset]
      {
        show(runtime.musicLibrary(),
             filter->as<std::string>(),
             yaml->count() > 0,
             limit->as<std::size_t>(),
             offset->as<std::size_t>(),
             std::cout);
      });

    auto* create = track->add_subcommand("create", "Create a track from a file");
    auto* path = create->add_option("path", "audio file path")->required();
    create->callback(
      [&runtime, path]
      {
        if (auto const optTrackId = runtime.library().writer().createTrackFromFile(path->as<std::string>()); optTrackId)
        {
          std::println("added track: {}", *optTrackId);
        }
        else
        {
          std::println("error adding track from: {}", path->as<std::string>());
        }
      });

    auto* del = track->add_subcommand("delete", "Delete a track by id");
    auto* id = del->add_option("id", "track id")->required();
    del->callback(
      [&runtime, id]
      {
        if (auto const trackId = TrackId{id->as<std::uint32_t>()}; runtime.library().writer().deleteTrack(trackId))
        {
          std::println("deleted track: {}", trackId);
        }
        else
        {
          std::println("track not found: {}", trackId);
        }
      });

    auto* dumpCmd = track->add_subcommand("dump", "Dump tracks from database");
    auto* dumpId = dumpCmd->add_option("--id", "track id to dump");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");
    auto* dumpYaml = dumpCmd->add_flag("--yaml", "output as YAML");

    dumpCmd->callback(
      [&runtime, dumpId, dumpRaw, dumpYaml]
      {
        dumpTracks(runtime.musicLibrary(),
                   dumpId->count() > 0 ? dumpId->as<std::uint32_t>() : 0,
                   dumpRaw->count() > 0,
                   dumpYaml->count() > 0,
                   std::cout);
      });
  }
} // namespace ao::cli
