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
#include <openssl/md5.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/timer/timer.hpp>

#include <rs/ml/core/Track.H>
#include <rs/ml/core/MediaLibrary.H>
#include <rs/ml/query/TrackFilter.H>
#include <rs/ml/query/Parser.H>
//#include <rs/ml/Protocol_generated.h>
#include <rs/ml/utility/Finder.H>
//#include <rs/ml/MD5Generator.H>
#include <rs/cli/BasicCommand.H>
#include <rs/cli/ComboCommand.H>

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
          std::cout << pair.first << " " << cstr(pair.second->artist()) << " " << cstr(pair.second->title()) << std::endl;
        }
      }

    }
    else
    {
      for (auto pair: ml.reader())
      {
        std::cout << pair.first << " " << cstr(pair.second->artist()) << " " << cstr(pair.second->title()) << std::endl;
      }
    }

    return std::error_code{};
  }).addOption("filter,f", bpo::value<std::string>(), "filter expression");

  root.addCommand<rs::cli::BasicCommand>("add", [&ml](const auto& vm)
  {
    rs::ml::Finder finder{"/media/rocklee/900E07FE0E07DC5A/Music/", {".m4a"}};
    auto writer = ml.writer();
    
    for (const boost::filesystem::path& path : finder)
    {
      TagLib::FileRef file(path.string().c_str());
    //  TagLib::MP3::File file(path.string().c_str());
      rs::ml::core::TrackT track;
      track.artist = file.tag()->artist().toCString(true);
      track.album = file.tag()->album().toCString(true);
      track.title = file.tag()->title().toCString(true);
      track.trackNumber = file.tag()->track();

 //     for (auto i = 0u; i < 50; ++i)
        writer.create(track);

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
