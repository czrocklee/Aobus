/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */


//#include <openssl/md5.h>
#include <boost/timer/timer.hpp>

#include <rs/ml/core/Track.h>
#include <rs/ml/core/MusicLibrary.h>
#include <rs/ml/utility/Finder.h>
#include <rs/ml/file/MusicFile.h>

#include <rs/cli/BasicCommand.h>
#include <rs/cli/ComboCommand.h>

#include "ListCommand.h"
#include "TrackCommand.h"

#include <iostream>

namespace bpo = boost::program_options;

int main(int argc, const char *argv[])
{
  rs::ml::core::MusicLibrary ml{"/home/rocklee/RockStudio/mylib"};

  rs::cli::ComboCommand root;
  
  /*
  root.addCommand<rs::cli::BasicCommand>("add", [&ml](const auto& vm)
  {
    rs::ml::Finder finder{"/home/rocklee/RockStudio/mylib/", {".m4a", ".mp3", ".flac"}};
    auto txn = ml.tracks().writeTransaction();
    
    for (const boost::filesystem::path& path : finder)
    {
      auto id = txn.create([&path] (flatbuffers::FlatBufferBuilder& fbb)
      {
        TagLib::FileRef file(path.string().c_str());
        
        ::flatbuffers::Offset<rs::ml::core::Metadata> meta;
        {
          auto tag = file.tag();
          auto artist = fbb.CreateString(tag->artist().toCString(true));
          auto album = fbb.CreateString(tag->album().toCString(true));
          auto title = fbb.CreateString(tag->title().toCString(true));
          auto track = tag->track();
          rs::ml::core::MetadataBuilder builder{fbb};
          builder.add_artist(artist);
          builder.add_album(album);
          builder.add_title(title);
          builder.add_trackNumber(track);
          meta = builder.Finish();
        }

        ::flatbuffers::Offset<rs::ml::core::Properties> prop;
        {
          auto ap = file.audioProperties();
          rs::ml::core::PropertiesBuilder builder{fbb};
          builder.add_length(ap->lengthInSeconds());
          prop = builder.Finish();
        }

        std::vector<std::string> t{"tag", "tag1", "tag2"};
        auto tags = fbb.CreateVectorOfStrings(t);

        std::vector<flatbuffers::Offset<rs::ml::core::CustomEntry>> entries;
        entries.push_back(rs::ml::core::CreateCustomEntry(fbb, fbb.CreateString("pop")));
        entries.push_back(rs::ml::core::CreateCustomEntry(fbb, fbb.CreateString("classic")));
        auto custom = fbb.CreateVectorOfSortedTables(&entries);

        rs::ml::core::TrackBuilder builder{fbb};
        builder.add_meta(meta);
        builder.add_prop(prop);
        builder.add_tags(tags);
        builder.add_custom(custom);
        return builder.Finish();
      });

        std::cout << id << " " << path << std::endl;
    }

    txn.commit();
    return std::error_code{};
  });
  */

  root.addCommand<rs::rml::TrackCommand>("track", ml);
  root.addCommand<rs::rml::ListCommand>("list", ml);
  
  
  root.addCommand<rs::cli::BasicCommand>("init", [](const bpo::variables_map& vm, std::ostream& os) 
  {
    rs::ml::core::MusicLibrary ml{"."};
    rs::ml::Finder finder{".", {".m4a", ".mp3", ".flac"}};

    auto txn = ml.writeTransaction();
    auto trackWriter = ml.tracks().writer(txn);
    auto resourceWriter = ml.resources().writer(txn);
    auto cstr = [](const flatbuffers::String* str) { return str == nullptr ? "nil" : str->str(); };

    for (const boost::filesystem::path& path : finder)
    {
      rs::ml::file::MusicFile file(path.string());
      auto tt = file.loadTrack();
      auto aa = file.loadAlbumArt();
      //os << "load track " << tt.meta->title << " ca size: " << ca.size() << std::endl;

      if (!aa.isEmpty())
      {
        std::uint64_t id = resourceWriter.create(boost::asio::buffer(aa.data(), aa.size()));
        auto resource = std::make_unique<rs::ml::fbs::ResourceT>();
        resource->type = rs::ml::fbs::ResourceType::AlbumArt;
        resource->id = id;
        tt.rsrc.push_back(std::move(resource));
      }

      auto track = trackWriter.createT(tt);
      os << "add track: " << cstr(track.value->meta()->title()) << std::endl;
    }

    txn.commit();
  });

  try 
  {
    root.execute(argc, argv, std::cout);
  }
  catch (const std::exception& e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
