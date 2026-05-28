// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackCommand.h"

#include "DumpUtils.h"
#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackLayout.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"
#include "ao/query/ExecutionPlan.h"
#include "ao/query/Parser.h"
#include "ao/query/PlanEvaluator.h"
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/TrackCommandService.h>

#include <CLI/App.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace ao::cli
{
  namespace
  {
    constexpr int kPlainTrackIdWidth = 5;

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
      auto compiler = query::QueryCompiler{&ml.dictionary()};
      auto const plan = compiler.compile(expr);
      auto evaluator = query::PlanEvaluator{};

      for (auto const& [id, view] : reader)
      {
        if (evaluator.matches(plan, view))
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
        os << "tracks: []\n";
        return;
      }

      std::size_t const end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());
      os << "tracks:\n";

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        os << "  - id: " << id << "\n"
           << "    title: \"" << view.metadata().title() << "\"\n";

        if (view.metadata().artistId() > 0)
        {
          os << "    artist: \"" << ml.dictionary().get(view.metadata().artistId()) << "\"\n";
        }

        if (view.metadata().albumId() > 0)
        {
          os << "    album: \"" << ml.dictionary().get(view.metadata().albumId()) << "\"\n";
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
        os << std::setw(kPlainTrackIdWidth) << id << " " << view.metadata().title() << '\n';
      }

      if (limit > 0 && offset + limit < matches.size())
      {
        os << "... (" << (matches.size() - offset - limit) << " more)\n";
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
        os << "  - id: " << id << "\n";

        if (view.isHotValid())
        {
          os << "    title: \"" << view.metadata().title() << "\"\n"
             << "    artist: \"" << resolveDict(dict, view.metadata().artistId()) << "\"\n"
             << "    album: \"" << resolveDict(dict, view.metadata().albumId()) << "\"\n";
        }

        if (view.isColdValid())
        {
          os << "    duration: " << view.property().durationMs() << "\n"
             << "    sampleRate: " << view.property().sampleRate() << "\n"
             << "    uri: \"" << view.property().uri() << "\"\n";
        }
      }
      else if (raw)
      {
        os << "Track ID: " << id << "\n";

        if (view.isHotValid())
        {
          os << "Hot Header:\n";
          hexDump(view.hotData().subspan(0, sizeof(library::TrackHotHeader)), os);
          os << "Hot Payload:\n";

          if (view.hotData().size() > sizeof(library::TrackHotHeader))
          {
            hexDump(view.hotData().subspan(sizeof(library::TrackHotHeader)), os);
          }
        }

        if (view.isColdValid())
        {
          os << "Cold Header:\n";
          hexDump(view.coldData().subspan(0, sizeof(library::TrackColdHeader)), os);
          os << "Cold Payload:\n";

          if (view.coldData().size() > sizeof(library::TrackColdHeader))
          {
            hexDump(view.coldData().subspan(sizeof(library::TrackColdHeader)), os);
          }
        }
      }
      else
      {
        os << "Track ID: " << id << "\n";

        if (view.isHotValid())
        {
          os << "  Title: " << view.metadata().title() << "\n"
             << "  Artist: " << resolveDict(dict, view.metadata().artistId()) << " (ID: " << view.metadata().artistId()
             << ")\n"
             << "  Album: " << resolveDict(dict, view.metadata().albumId()) << " (ID: " << view.metadata().albumId()
             << ")\n"
             << "  Tag Bloom: 0x" << std::hex << std::setw(8) << std::setfill('0') << view.tags().bloom() << std::dec
             << std::setfill(' ') << "\n"
             << "  Tags: ";

          for (auto const tagId : view.tags())
          {
            os << resolveDict(dict, tagId) << " (ID: " << tagId << ") ";
          }

          os << "\n";
        }

        if (view.isColdValid())
        {
          os << "  Duration: " << view.property().durationMs() << "ms\n"
             << "  Sample Rate: " << view.property().sampleRate() << "Hz\n"
             << "  URI: " << view.property().uri() << "\n";

          for (auto const& [customId, val] : view.custom())
          {
            os << "  Custom [" << resolveDict(dict, customId) << "]: " << val << "\n";
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
        os << "tracks:\n";
      }

      if (targetId > 0)
      {
        auto const id = TrackId{targetId};

        if (auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both))
        {
          processTrackDump(id, *optView, dict, raw, yaml, os);
        }
        else
        {
          if (!yaml)
          {
            os << "Track " << targetId << " not found.\n";
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
  }

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
        if (auto const trackId = runtime.trackCommands().createTrackFromFile(path->as<std::string>());
            trackId != kInvalidTrackId)
        {
          std::cout << "added track: " << trackId << '\n';
        }
        else
        {
          std::cout << "error adding track from: " << path->as<std::string>() << '\n';
        }
      });

    auto* del = track->add_subcommand("delete", "Delete a track by id");
    auto* id = del->add_option("id", "track id")->required();
    del->callback(
      [&runtime, id]
      {
        if (auto const trackId = TrackId{id->as<std::uint32_t>()}; runtime.trackCommands().deleteTrack(trackId))
        {
          std::cout << "deleted track: " << trackId << '\n';
        }
        else
        {
          std::cout << "track not found: " << trackId << '\n';
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
}
