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

#include <stdio.h>
#include <regex.h>
#include <taglib/taglib.h>
#include <taglib/flacfile.h>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
//#include <openssl/md5.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/timer/timer.hpp>

#include <rs/ml/core/Track.h>
#include <rs/ml/core/MediaLibrary.h>
#include <rs/ml/query/TrackFilter.h>
#include <rs/ml/query/Parser.h>
//#include <rs/ml/Protocol_generated.h>
#include <rs/ml/utility/Finder.h>
//#include <rs/ml/MD5Generator.h>
#include <rs/cli/BasicCommand.h>
#include <rs/cli/ComboCommand.h>

namespace bpo = boost::program_options;

int main(int argc, const char *argv[])
{
  rs::ml::core::MediaLibrary ml{"/home/rocklee/RockStudio/mylib"};

  rs::cli::ComboCommand root;
  root.addCommand<rs::cli::BasicCommand>("show", [&ml](const auto& vm)
  {
    auto cstr = [](const flatbuffers::String* str) { return str == nullptr ? "nil" : str->str(); };

    if (vm.count("filter") > 0)
    {
      rs::ml::query::Parser parser;
      std::cout << vm["filter"].template as<std::string>() << std::endl;
      auto expr = parser.parse(vm["filter"].template as<std::string>());

 //     auto expr = parser.parse("%title% ~ \"Bach\"");
      rs::ml::query::TrackFilter filter{std::move(expr)};
      filter.dump();

      for (auto pair: ml.reader())
      {
        if (filter(pair.second))
        {
          std::cout << pair.first << " " << cstr(pair.second->meta()->artist()) << " " << cstr(pair.second->meta()->title()) << std::endl;
        }
      }

    }
    else
    {
      for (auto pair: ml.reader())
      {
        std::cout << pair.first << " " << cstr(pair.second->meta()->artist()) << " " << cstr(pair.second->meta()->title()) << std::endl;
        std::cout << (pair.second->custom()->LookupByKey("pop") != nullptr);
      }
    }

    return std::error_code{};
  }).addOption("filter,f", bpo::value<std::string>(), "filter expression");

  root.addCommand<rs::cli::BasicCommand>("add", [&ml](const auto& vm)
  {
    rs::ml::Finder finder{"/home/rocklee/RockStudio/mylib/", {".m4a", ".mp3", ".flac"}};
    auto writer = ml.writer();
    
    for (const boost::filesystem::path& path : finder)
    {
      writer.create([&path] (flatbuffers::FlatBufferBuilder& fbb)
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

      std::cout << path << std::endl;
    }
    
    return std::error_code{};
  });

  root.addCommand<rs::cli::BasicCommand>("del", [&ml](const bpo::variables_map& vm)
  {
    auto writer = ml.writer();
    writer.remove(vm["track"].as<rs::ml::core::TrackId>());
    return std::error_code{};
  }).addOption("track", bpo::value<rs::ml::core::TrackId>()->required(), "track id");


  root.addCommand<rs::cli::BasicCommand>("test", [&ml](const bpo::variables_map& vm)
  {
    std::deque<rs::ml::core::TrackId> tracks;

    for (auto pair : ml.reader())
    {
      tracks.push_back(pair.first);
    }

 //   std::random_shuffle(tracks.begin(), tracks.end());

    std::size_t i = 0;

    {

        auto reader = ml.reader();
      boost::timer::auto_cpu_timer timer;
      for (auto track : tracks)
      {
        i += (std::size_t)reader[track];
      }
    }
  
    std::cout << i << std::endl;
      
    return std::error_code{};
  });


//  std::vector<std::string> args(argv + 1, argv + argc);
  return root.execute(argc, argv).value();
}
