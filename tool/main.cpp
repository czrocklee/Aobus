/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/timer/timer.hpp>

#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackRecord.h>
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>
#include <rs/utility/Finder.h>

#include "BasicCommand.h"
#include "ComboCommand.h"
#include "ListCommand.h"
#include "TrackCommand.h"

#include <iostream>
#include <sstream>
#include <vector>

namespace bpo = boost::program_options;
using namespace rs;

namespace
{
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
}

int main(int argc, char const* argv[])
{
  rs::core::MusicLibrary ml{"."};

  ComboCommand root;

  root.addCommand<TrackCommand>("track", ml);
  root.addCommand<ListCommand>("list", ml);

  root.addCommand<BasicCommand>("init", [](bpo::variables_map const&, std::ostream& os) {
    rs::core::MusicLibrary ml{"."};
    rs::utility::Finder finder{".", {".flac", ".m4a"}};
    auto txn = ml.writeTransaction();
    auto trackWriter = ml.tracks().writer(txn);
    [[maybe_unused]] auto& dictionary = ml.dictionary();

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

      // Serialize and store
      auto data = record.serialize();
      auto [id, trackView] = trackWriter.create(data.data(), data.size());

      os << "add track: " << id << " " << record.metadata.title << std::endl;
    }

    txn.commit();
    return "";
  });

  try
  {
    root.execute(argc, argv, std::cout);
  }
  catch (std::exception const& e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
