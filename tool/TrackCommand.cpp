// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackCommand.h"
#include <rs/library/TrackLayout.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <rs/tag/File.h>

#include <filesystem>
#include <iomanip>
#include <iostream>

namespace rs::tool
{
  namespace
  {
    using namespace rs;

    std::vector<std::pair<rs::TrackId, rs::library::TrackView>> collectTracks(rs::library::MusicLibrary& ml,
                                                                         std::string const& filter)
    {
      auto txn = ml.readTransaction();
      auto reader = ml.tracks().reader(txn);
      auto matches = std::vector<std::pair<rs::TrackId, rs::library::TrackView>>{};

      if (filter.empty())
      {
        for (auto [id, view] : reader)
        {
          matches.emplace_back(id, std::move(view));
        }
        return matches;
      }

      auto expr = rs::expr::parse(filter);
      auto compiler = rs::expr::QueryCompiler{&ml.dictionary()};
      auto plan = compiler.compile(expr);
      auto evaluator = rs::expr::PlanEvaluator{};

      for (auto [id, view] : reader)
      {
        if (evaluator.matches(plan, view))
        {
          matches.emplace_back(id, std::move(view));
        }
      }
      return matches;
    }

    void formatJson(std::vector<std::pair<rs::TrackId, rs::library::TrackView>> const& matches,
                    std::size_t offset,
                    std::size_t limit,
                    rs::library::MusicLibrary& ml,
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
        os << "  {\"id\": " << id << ", \"title\": \"" << view.metadata().title() << "\"";

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

    void formatPlain(std::vector<std::pair<rs::TrackId, rs::library::TrackView>> const& matches,
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

    void show(rs::library::MusicLibrary& ml,
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

    void createTrack(rs::library::MusicLibrary& ml, std::filesystem::path const& path, std::ostream& os)
    {
      auto tagFile = tag::File::open(path);

      if (!tagFile)
      {
        os << "unsupported file format: " << path << '\n';
        return;
      }

      auto txn = ml.writeTransaction();
      auto writer = ml.tracks().writer(txn);
      auto builder = tagFile->loadTrack();
      builder.property()
        .uri(path.string())
        .fileSize(std::filesystem::file_size(path))
        .mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

      auto [preparedHot, preparedCold] = builder.prepare(txn, ml.dictionary(), ml.resources());
      auto [id, trackView] = writer.createHotCold(
        preparedHot.size(),
        preparedCold.size(),
        [&preparedHot, &preparedCold](rs::TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
        {
          preparedHot.writeTo(hot);
          preparedCold.writeTo(cold);
        });
      txn.commit();

      os << "add track: " << id << " " << trackView.metadata().title() << '\n';
    }
  }

  void setupTrackCommand(CLI::App& app, rs::library::MusicLibrary& ml)
  {
    auto* track = app.add_subcommand("track", "Track management commands");

    auto* showCmd = track->add_subcommand("show", "Show tracks matching a filter");
    auto* filter = showCmd->add_option("filter,-f,--filter", "track filter expression");
    auto* json = showCmd->add_flag("-j,--json", "output as JSON");
    auto* limit = showCmd->add_option("-l,--limit", "limit number of results (0 = all)")->default_val(0);
    auto* offset = showCmd->add_option("-o,--offset", "offset results")->default_val(0);
    showCmd->callback(
      [&ml, filter, json, limit, offset]()
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
    create->callback([&ml, path]() { createTrack(ml, path->as<std::string>(), std::cout); });

    auto* del = track->add_subcommand("delete", "Delete a track by id");
    auto* id = del->add_option("id", "track id")->required();
    del->callback(
      [&ml, id]()
      {
        auto txn = ml.writeTransaction();
        auto writer = ml.tracks().writer(txn);
        auto const trackId = rs::TrackId{id->as<std::uint32_t>()};

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
