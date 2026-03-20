// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackCommand.h"
#include "BasicCommand.h"
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <iomanip>
#include <filesystem>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  std::unique_ptr<rs::tag::File> createTagFileByExtension(std::filesystem::path const& path)
  {
    static std::unordered_map<std::string,
                              std::function<std::unique_ptr<rs::tag::File>(std::filesystem::path const)>> const
      CreatorMap = {
        {".mp3",
         [](auto const& path) { return std::make_unique<rs::tag::mpeg::File>(path, rs::tag::File::Mode::ReadOnly); }},
        {".m4a",
         [](auto const& path) { return std::make_unique<rs::tag::mp4::File>(path, rs::tag::File::Mode::ReadOnly); }},
        {".flac",
         [](auto const& path) { return std::make_unique<rs::tag::flac::File>(path, rs::tag::File::Mode::ReadOnly); }}};

    return std::invoke(CreatorMap.at(path.extension().string()), path);
  }

  std::string getString(rs::tag::ValueType const& val)
  {
    if (rs::tag::isNull(val)) return {};
    return std::get<std::string>(val);
  }

  void show(core::MusicLibrary& ml, std::string const& filter, bool json, std::size_t limit, std::size_t offset, std::ostream& os)
  {
    auto txn = ml.readTransaction();
    auto reader = ml.tracks().reader(txn);

    // Collect matching tracks
    std::vector<std::pair<core::TrackId, core::TrackView>> matches;

    if (filter.empty())
    {
      for (auto [id, view] : reader)
      {
        matches.emplace_back(id, std::move(view));
      }
    }
    else
    {
      auto expr = rs::expr::parse(filter);
      rs::expr::QueryCompiler compiler{&ml.dictionary()};
      auto plan = compiler.compile(expr);
      rs::expr::PlanEvaluator evaluator;

      switch (plan.accessProfile)
      {
        case rs::expr::AccessProfile::HotOnly:
        case rs::expr::AccessProfile::ColdOnly:
        case rs::expr::AccessProfile::HotAndCold:
        {
          for (auto [id, view] : reader)
          {
            if (evaluator.matches(plan, view))
            {
              matches.emplace_back(id, std::move(view));
            }
          }
          break;
        }
      }
    }

    // Apply offset
    if (offset >= matches.size())
    {
      if (json)
      {
        os << "[]\n";
      }
      return;
    }

    // Apply limit (0 means no limit)
    std::size_t end = (limit == 0) ? matches.size() : std::min(offset + limit, matches.size());

    if (json)
    {
      os << "[\n";
      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        os << "  {\"id\": " << id
           << ", \"title\": \"" << view.metadata().title() << "\"";
        if (view.metadata().artistId() > 0)
        {
          os << ", \"artist\": \"" << ml.dictionary().get(view.metadata().artistId()) << "\"";
        }
        if (view.metadata().albumId() > 0)
        {
          os << ", \"album\": \"" << ml.dictionary().get(view.metadata().albumId()) << "\"";
        }
        os << "}";
        if (i < end - 1) os << ",";
        os << "\n";
      }
      os << "]\n";
    }
    else
    {
      for (std::size_t i = offset; i < end; ++i)
      {
        auto const& [id, view] = matches[i];
        os << std::setw(5) << id << " " << view.metadata().title() << std::endl;
      }
      if (limit > 0 && offset + limit < matches.size())
      {
        os << "... (" << (matches.size() - offset - limit) << " more)\n";
      }
    }
  }

  void createTrack(core::MusicLibrary& ml, std::filesystem::path const& path, std::ostream& os)
  {
    std::unique_ptr<rs::tag::File> file;
    rs::tag::Metadata metadata;

    try
    {
      file = createTagFileByExtension(path);
      metadata = file->loadMetadata();
    }
    catch (std::exception const& e)
    {
      os << "failed to parse metadata for " << path.filename() << ": " << e.what() << std::endl;
      return;
    }

    core::TrackRecord record;
    record.metadata.uri = path.string();
    record.property.fileSize = std::filesystem::file_size(path);
    record.property.mtime = std::filesystem::last_write_time(path).time_since_epoch().count();

    auto titleVal = metadata.get(rs::tag::MetaField::Title);
    if (!rs::tag::isNull(titleVal))
    {
      record.metadata.title = getString(titleVal);
    }

    auto artistVal = metadata.get(rs::tag::MetaField::Artist);
    if (!rs::tag::isNull(artistVal))
    {
      record.metadata.artist = getString(artistVal);
    }

    auto albumVal = metadata.get(rs::tag::MetaField::Album);
    if (!rs::tag::isNull(albumVal))
    {
      record.metadata.album = getString(albumVal);
    }

    auto genreVal = metadata.get(rs::tag::MetaField::Genre);
    if (!rs::tag::isNull(genreVal))
    {
      record.metadata.genre = getString(genreVal);
    }

    auto year = metadata.get(rs::tag::MetaField::Year);
    if (!rs::tag::isNull(year))
    {
      record.metadata.year = static_cast<std::uint16_t>(std::get<std::int64_t>(year));
    }

    auto trackNum = metadata.get(rs::tag::MetaField::TrackNumber);
    if (!rs::tag::isNull(trackNum))
    {
      record.metadata.trackNumber = static_cast<std::uint16_t>(std::get<std::int64_t>(trackNum));
    }

    auto totalTracks = metadata.get(rs::tag::MetaField::TotalTracks);
    if (!rs::tag::isNull(totalTracks))
    {
      record.metadata.totalTracks = static_cast<std::uint16_t>(std::get<std::int64_t>(totalTracks));
    }

    auto discNum = metadata.get(rs::tag::MetaField::DiscNumber);
    if (!rs::tag::isNull(discNum))
    {
      record.metadata.discNumber = static_cast<std::uint16_t>(std::get<std::int64_t>(discNum));
    }

    auto totalDiscs = metadata.get(rs::tag::MetaField::TotalDiscs);
    if (!rs::tag::isNull(totalDiscs))
    {
      record.metadata.totalDiscs = static_cast<std::uint16_t>(std::get<std::int64_t>(totalDiscs));
    }

    auto txn = ml.writeTransaction();
    auto trackWriter = ml.tracks().writer(txn);
    auto hotData = record.serializeHot();
    auto coldData = record.serializeCold();
    auto [id, trackView] = trackWriter.createHotCold(hotData, coldData);
    txn.commit();

    os << "add track: " << id << " " << record.metadata.title << std::endl;
  }
}

TrackCommand::TrackCommand(core::MusicLibrary& ml) : _ml{ml}
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
    .addOption("id", bpo::value<std::uint64_t>()->required(), "track id", 1)
    .setExecutor([this](auto const& vm, auto& os) {
      auto id = core::TrackId{vm["id"].template as<std::uint32_t>()};
      auto txn = _ml.writeTransaction();
      auto writer = _ml.tracks().writer(txn);
      if (writer.remove(id))
      {
        os << "deleted track: " << id << std::endl;
        txn.commit();
      }
      else
      {
        os << "track not found: " << id << std::endl;
      }
      return "";
    });
}
