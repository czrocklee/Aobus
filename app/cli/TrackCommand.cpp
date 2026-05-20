// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackCommand.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"
#include "ao/query/ExecutionPlan.h"
#include "ao/query/Parser.h"
#include "ao/query/PlanEvaluator.h"
#include "runtime/CoreRuntime.h"
#include "runtime/TrackCommandService.h"

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
    using namespace ao;

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

    void formatJson(std::vector<std::pair<TrackId, library::TrackView>> const& matches,
                    std::size_t offset,
                    std::size_t limit,
                    library::MusicLibrary& ml,
                    std::ostream& os)
    {
      if (offset >= matches.size())
      {
        os << "[]\n";
        return;
      }

      std::size_t const end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());
      os << "[\n";

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        os << " {\"id\": " << id << R"(, "title": ")" << view.metadata().title() << "\"";

        if (view.metadata().artistId() > 0)
        {
          os << R"(, "artist": ")" << ml.dictionary().get(view.metadata().artistId()) << "\"";
        }

        if (view.metadata().albumId() > 0)
        {
          os << R"(, "album": ")" << ml.dictionary().get(view.metadata().albumId()) << "\"";
        }

        os << "}";

        if (i < end - 1)
        {
          os << ",";
        }

        os << "\n";
      }

      os << "]\n";
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
              bool json,
              std::size_t limit,
              std::size_t offset,
              std::ostream& os)
    {
      if (auto const matches = collectTracks(ml, filter); json)
      {
        formatJson(matches, offset, limit, ml, os);
      }
      else
      {
        formatPlain(matches, offset, limit, os);
      }
    }
  }

  void setupTrackCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* track = app.add_subcommand("track", "Track management commands");
    auto* showCmd = track->add_subcommand("show", "Show tracks matching a filter");
    auto* filter = showCmd->add_option("filter,-f,--filter", "track filter expression");
    auto* json = showCmd->add_flag("-j,--json", "output as JSON");
    auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
    auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);

    showCmd->callback(
      [&runtime, filter, json, limit, offset]
      {
        show(runtime.musicLibrary(),
             filter->as<std::string>(),
             json->count() > 0,
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
  }
}
