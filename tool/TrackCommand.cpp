// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackCommand.h"
#include "BasicCommand.h"
#include "TrackUtils.h"
#include <rs/core/TrackLayout.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>

#include <filesystem>
#include <iomanip>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  std::vector<std::pair<core::TrackId, core::TrackView>> collectTracks(
      core::MusicLibrary& ml, std::string const& filter)
  {
    auto txn = ml.readTransaction();
    auto reader = ml.tracks().reader(txn);
    std::vector<std::pair<core::TrackId, core::TrackView>> matches;

    if (filter.empty())
    {
      for (auto [id, view] : reader) { matches.emplace_back(id, std::move(view)); }
      return matches;
    }

    auto expr = rs::expr::parse(filter);
    auto compiler = rs::expr::QueryCompiler{&ml.dictionary()};
    auto plan = compiler.compile(expr);
    rs::expr::PlanEvaluator evaluator;

    for (auto [id, view] : reader)
    {
      if (evaluator.matches(plan, view)) { matches.emplace_back(id, std::move(view)); }
    }
    return matches;
  }

  void formatJson(std::vector<std::pair<core::TrackId, core::TrackView>> const& matches,
                  std::size_t offset,
                  std::size_t limit,
                  core::MusicLibrary& ml,
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
      if (i < end - 1) { os << ","; }
      os << "\n";
    }

    os << "]\n";
  }

  void formatPlain(std::vector<std::pair<core::TrackId, core::TrackView>> const& matches,
                   std::size_t offset,
                   std::size_t limit,
                   std::ostream& os)
  {
    if (offset >= matches.size()) { return; }

    std::size_t end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());

    for (std::size_t i = offset; i < end; ++i)
    {
      auto const& [id, view] = matches[i];
      os << std::setw(5) << id << " " << view.metadata().title()
         << '\n'; // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    }

    if (limit > 0 && offset + limit < matches.size())
    {
      os << "... (" << (matches.size() - offset - limit) << " more)\n";
    }
  }

  void show(core::MusicLibrary& ml,
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

  void createTrack(core::MusicLibrary& ml, std::filesystem::path const& path, std::ostream& os)
  {
    auto txn = ml.writeTransaction();
    auto writer = ml.tracks().writer(txn);
    auto resourceWriter = ml.resources().writer(txn);
    auto builder = loadTrackRecord(path, ml.dictionary(), resourceWriter, txn);
    auto [hotData, coldData] = builder.serialize(txn, ml.dictionary(), ml.resources());
    auto [id, trackView] = writer.createHotCold(hotData, coldData);
    txn.commit();

    os << "add track: " << id << " " << trackView.metadata().title() << '\n';
  }
}

TrackCommand::TrackCommand(core::MusicLibrary& ml)
  : _ml{ml}
{
  addCommand<BasicCommand>("show")
    .addOption("filter,f", bpo::value<std::string>()->default_value(""), "track filter expression", 1)
    .addOption("json,j", "output as JSON")
    .addOption("limit,l", bpo::value<std::size_t>()->default_value(0), "limit number of results (0 = all)")
    .addOption("offset,o", bpo::value<std::size_t>()->default_value(0), "offset results")
    .setExecutor([this](auto const& vm, auto& os) {
      return show(_ml,
                  vm["filter"].template as<std::string>(),
                  vm.count("json") > 0,
                  vm["limit"].template as<std::size_t>(),
                  vm["offset"].template as<std::size_t>(),
                  os);
    });

  addCommand<BasicCommand>("create")
    .addOption("path", bpo::value<std::string>()->required(), "audio file path", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto path = vm["path"].template as<std::string>();
      createTrack(_ml, path, os);
      return "";
    });

  addCommand<BasicCommand>("delete")
    .addOption("id", bpo::value<std::uint32_t>()->required(), "track id", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = core::TrackId{vm["id"].template as<std::uint32_t>()};
      auto txn = _ml.writeTransaction();
      auto writer = _ml.tracks().writer(txn);
      if (writer.remove(id))
      {
        os << "deleted track: " << id << '\n';
        txn.commit();
      }
      else
      {
        os << "track not found: " << id << '\n';
      }
      return "";
    });
}
