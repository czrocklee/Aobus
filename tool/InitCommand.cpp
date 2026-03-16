// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "InitCommand.h"
#include <rs/core/TrackRecord.h>
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>
#include <rs/utility/Finder.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <unordered_map>

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

  void scanAndImport(core::MusicLibrary& ml, std::ostream& os)
  {
    rs::utility::Finder finder{".", {".flac", ".m4a", ".mp3"}};
    auto txn = ml.writeTransaction();
    auto trackWriter = ml.tracks().writer(txn);
    auto& dictionary = ml.dictionary();

    for (std::filesystem::path const& path : finder)
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
        std::cerr << "failed to parse metadata for " << path.filename() << ": " << e.what() << std::endl;
        continue;
      }

      // Build track record
      core::TrackRecord record;
      record.metadata.uri = path.string();
      record.property.fileSize = std::filesystem::file_size(path);
      record.property.mtime = std::filesystem::last_write_time(path).time_since_epoch().count();

      // Get metadata fields
      auto titleVal = metadata.get(rs::tag::MetaField::Title);
      if (!rs::tag::isNull(titleVal))
      {
        record.metadata.title = getString(titleVal);
      }

      std::string artistStr, albumStr, genreStr;
      auto artistVal = metadata.get(rs::tag::MetaField::Artist);
      if (!rs::tag::isNull(artistVal))
      {
        artistStr = getString(artistVal);
        record.metadata.artist = artistStr;
      }

      auto albumVal = metadata.get(rs::tag::MetaField::Album);
      if (!rs::tag::isNull(albumVal))
      {
        albumStr = getString(albumVal);
        record.metadata.album = albumStr;
      }

      auto genreVal = metadata.get(rs::tag::MetaField::Genre);
      if (!rs::tag::isNull(genreVal))
      {
        genreStr = getString(genreVal);
        record.metadata.genre = genreStr;
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

      // Add to dictionary and get IDs for header
      if (!artistStr.empty())
      {
        record.artistId = dictionary.getId(artistStr);
        if (record.artistId.value() == 0)
        {
          record.artistId = dictionary.put(txn, artistStr);
        }
      }
      if (!albumStr.empty())
      {
        record.albumId = dictionary.getId(albumStr);
        if (record.albumId.value() == 0)
        {
          record.albumId = dictionary.put(txn, albumStr);
        }
      }
      if (!genreStr.empty())
      {
        record.genreId = dictionary.getId(genreStr);
        if (record.genreId.value() == 0)
        {
          record.genreId = dictionary.put(txn, genreStr);
        }
      }

      // Serialize and store
      auto data = record.serialize();
      auto [id, trackView] = trackWriter.create(data);

      os << "add track: " << id << " " << record.metadata.title << std::endl;
    }

    txn.commit();
  }
}

InitCommand::InitCommand(rs::core::MusicLibrary& ml)
{
  setExecutor([&ml](auto const& vm, auto& os) {
    (void)vm;
    scanAndImport(ml, os);
    return "";
  });
}
