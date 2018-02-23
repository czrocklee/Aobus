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

#include "rml/include/lmdb++.h"
#include <stdio.h>
#include <regex.h>
#include <taglib/taglib.h>
#include <taglib/flacfile.h>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <openssl/md5.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/timer/timer.hpp>

#include <rml/MediaLibrary.H>
//#include <rml/Protocol_generated.h>
#include <rml/Finder.H>
#include <rml/MD5Generator.H>
#include <cli/BasicCommand.H>
#include <cli/ComboCommand.H>

struct Data
{
  char value[10] = "jkackerer";
};

int main(int argc, const char *argv[])
{
  rml::MediaLibrary ml{"/home/rocklee/RockStudio/mylib"};

  cli::ComboCommand root;
  root.addCommand<cli::BasicCommand>("show", [&ml](const auto& vm)
  {
    //std::cout << tk.getArtist().cStr() << " " << tk.getTitle().cStr() << std::endl;
    for (const auto& pair: ml)
    {
      /*
      if (pair.second.metaData()->lastModified() != boost::filesystem::last_write_time(pair.second.metaData()->filepath()->c_str()))
      {
        std::cout << pair.first << " " << pair.second.metaData()->filepath()->str() << std::endl;
      }*/
      std::cout << pair.first << " "; 
      pair.second([](rml::Track::Reader track)
      {
        std::cout << track.getArtist().cStr() << " " << track.getTitle().cStr() << std::endl;
      });
    }

    return std::error_code{};
  });

  root.addCommand<cli::BasicCommand>("add", [&ml](const auto& vm)
  {
    rml::Finder finder{"/home/rocklee/RockStudio/mylib", {".mp3"}};

    for (const boost::filesystem::path& path : finder)
    {
      TagLib::FileRef file(path.string().c_str());
    //  TagLib::MP3::File file(path.string().c_str());

      ml.add([&file, &path](rml::Track::Builder track)
      {
  //      std::ifstream ifs{path.string(), std::ios::in | std::ios::binary};
  //      auto digest = rml::calculateMD5(ifs);

  //      auto artist = fbb.CreateString(file.tag()->artist().toCString(true));

        track.setArtist(file.tag()->artist().toCString(true));
        track.setAlbum(file.tag()->album().toCString(true));
        track.setTitle(file.tag()->title().toCString(true));
        


  //      auto album = fbb.CreateString(file.tag()->album().toCString(true));
  //      auto title = fbb.CreateString(file.tag()->title().toCString(true));
  //      auto metaData = rml::CreateMetaData(fbb, fbb.CreateString(path.string()), boost::filesystem::last_write_time(path), fbb.CreateVector(digest.data(), digest.size()));
      });

      std::cout << path << std::endl;
    }

    return std::error_code{};
  });

  std::vector<std::string> args(argv + 1, argv + argc);
  root.execute(argc - 1, argv + 1);

  /*
  rml::Finder finder{"/home/rocklee/RockStudio/mylib", {".mp3"}};


  for (const boost::filesystem::path& path : finder)
  {
    TagLib::FileRef file(path.string().c_str());
  //  TagLib::MP3::File file(path.string().c_str());

    ml.add([&file, &path](rml::Track::Builder track)
    {
//      std::ifstream ifs{path.string(), std::ios::in | std::ios::binary};
//      auto digest = rml::calculateMD5(ifs);

//      auto artist = fbb.CreateString(file.tag()->artist().toCString(true));

      track.setArtist(file.tag()->artist().toCString(true));
      track.setAlbum(file.tag()->album().toCString(true));
      track.setTitle(file.tag()->title().toCString(true));
      


//      auto album = fbb.CreateString(file.tag()->album().toCString(true));
//      auto title = fbb.CreateString(file.tag()->title().toCString(true));
//      auto metaData = rml::CreateMetaData(fbb, fbb.CreateString(path.string()), boost::filesystem::last_write_time(path), fbb.CreateVector(digest.data(), digest.size()));
    });

    std::cout << path << std::endl;

  }*/

  /*
  TagLib::Tag* tag = f.tag();
  auto map = tag->properties();

  for (const auto& pair : map)
  {
    std::cout << pair.first << ' ' << pair.second.toString(":") << std::endl;
  }

  auto env = lmdb::env::create();
   env.set_mapsize(1UL * 1024UL * 1024UL * 1024UL);
   env.open("testdb", 0, 0664);



   auto wtxn = lmdb::txn::begin(env);
   auto dbi = lmdb::dbi::open(wtxn, nullptr, 0);

   std::uint64_t index = 0;
   Data data;

   int trans_count = 0;
   boost::timer::auto_cpu_timer t;
   boost::multiprecision::uint128_t a = 0;

   while (index < 10000000L)
   {
     std::string n = std::to_string(index);
     boost::multiprecision::uint128_t md5;
     //MD5Value md5;
     MD5((const unsigned char*)n.c_str(), n.length(), (unsigned char*)&md5);
     a += md5;

     char mdString[33];

         for(int i = 0; i < 16; i++)
              sprintf(&mdString[i*2], "%02x", (unsigned int)digest[i]);

         printf("md5 digest: %s\n", mdString);


     //lmdb::val{&index, sizeof(index)};

     //dbi.put(wtxn, md5, data);
     //++index;
    // dbi.put(wtxn, "email", "jhacker@example.org");
    // dbi.put(wtxn, "fullname", "J. Random Hacker");

   }
   wtxn.commit();
   std::cout << a << std::endl;

   auto rtxn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
   auto cursor = lmdb::cursor::open(rtxn, dbi);
   lmdb::val key, value;
   while (cursor.get(key, value, MDB_NEXT)) {
     a += *key.data<boost::multiprecision::uint128_t>();
     //std::cout << *key.data<boost::multiprecision::uint128_t>() << ' ' << key.size() << std::endl;
     //std::printf("key: '%d', value: '%s'\n", *((int *)key.c_str()), value.c_str());
   }
   std::cout << a << std::endl;
   cursor.close();
   rtxn.abort();*/
   std::cout << "done" << std::endl;
}
