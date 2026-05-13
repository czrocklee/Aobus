// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackCommand.h"
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/tag/TagFile.h>

#include <filesystem>
#include <iomanip>
#include <iostream>

namespace ao::cli
{
  namespace
  {
    using namespace ao;

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

      std::size_t end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());
      os << "[\n";

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        os << " {\"id\": " << id << ", \"title\": \"" << view.metadata().title() << "\"";

        if (view.metadata().artistId() > 0)
        {
          os << ", \"artist\": \"" << ml.dictionary().get(view.metadata().artistId()) << "\"";
        }

        if (view.metadata().albumId() > 0)
        {
          os << ", \"album\": \"" << ml.dictionary().get(view.metadata().albumId()) << "\"";
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

      std::size_t end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());

      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        os << std::setw(5) << id << " " << view.metadata().title() << '\n'; // NOLINT(readability-magic-numbers)
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
      auto matches = collectTracks(ml, filter);

      if (json)
      {
        formatJson(matches, offset, limit, ml, os);
      }
      else
      {
        formatPlain(matches, offset, limit, os);
      }
    }

    void createTrack(library::MusicLibrary& ml, std::filesystem::path const& path, std::ostream& os)
    {
      auto const optTagFile = tag::TagFile::open(path);

      if (!optTagFile)
      {
        os << "unsupported file format: " << path << '\n';
        return;
      }

      auto txn = ml.writeTransaction();
      auto writer = ml.tracks().writer(txn);
      auto builder = optTagFile->loadTrack();
      builder.property()
        .uri(path.string())
        .fileSize(std::filesystem::file_size(path))
        .mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

      auto const [preparedHot, preparedCold] = builder.prepare(txn, ml.dictionary(), ml.resources());
      auto const [id, trackView] =
        writer.createHotCold(preparedHot.size(),
                             preparedCold.size(),
                             [&preparedHot, &preparedCold](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
                             {
                               preparedHot.writeTo(hot);
                               preparedCold.writeTo(cold);
                             });
      txn.commit();

      os << "add track: " << id << " " << trackView.metadata().title() << '\n';
    }
  }

  void setupTrackCommand(CLI::App& app, library::MusicLibrary& ml)
  {
    auto* track = app.add_subcommand("track", "Track management commands");

    auto* showCmd = track->add_subcommand("show", "Show tracks matching a filter");
    auto* filter = showCmd->add_option("filter,-f,--filter", "track filter expression");
    auto* json = showCmd->add_flag("-j,--json", "output as JSON");
    auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
    auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);
    showCmd->callback(
      [&ml, filter, json, limit, offset]
      {
        show(ml,
             filter->as<std::string>(),
             json->count() > 0,
             limit->as<std::size_t>(),
             offset->as<std::size_t>(),
             std::cout);
      });

    auto* create = track->add_subcommand("create", "Create a track from a file");
    auto* path = create->add_option("path", "audio file path")->required();
    create->callback([&ml, path] { createTrack(ml, path->as<std::string>(), std::cout); });

    auto* del = track->add_subcommand("delete", "Delete a track by id");
    auto* id = del->add_option("id", "track id")->required();
    del->callback(
      [&ml, id]
      {
        auto txn = ml.writeTransaction();
        auto writer = ml.tracks().writer(txn);
        auto const trackId = TrackId{id->as<std::uint32_t>()};

        if (writer.remove(trackId))
        {
          std::cout << "deleted track: " << trackId << '\n';
          txn.commit();
        }
        else
        {
          std::cout << "track not found: " << trackId << '\n';
        }
      });
  }
}
