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

#include <rs/ml/core/MusicLibrary.h>
#include <rs/ml/utility/Finder.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/flac/File.h>

#include <rs/cli/BasicCommand.h>
#include <rs/cli/ComboCommand.h>

#include "ListCommand.h"
#include "TrackCommand.h"

#include <iostream>

namespace bpo = boost::program_options;

namespace
{
  ::flatbuffers::Offset<::flatbuffers::String> buildString(
    ::flatbuffers::FlatBufferBuilder& fbb,
    const rs::tag::ValueType& value)
  {
    return rs::tag::isNull(value) ? ::flatbuffers::Offset<::flatbuffers::String>{}
                                  : fbb.CreateString(std::get<std::string>(value));
  }
}

int main(int argc, const char* argv[])
{
  rs::ml::core::MusicLibrary ml{"."};

  rs::cli::ComboCommand root;

  root.addCommand<rs::rml::TrackCommand>("track", ml);
  root.addCommand<rs::rml::ListCommand>("list", ml);

  root.addCommand<rs::cli::BasicCommand>("init", [](const bpo::variables_map& vm, std::ostream& os) {
    rs::ml::core::MusicLibrary ml{"."};
    rs::ml::Finder finder{".", {".flac", "*.mp4"}};

    auto txn = ml.writeTransaction();
    auto trackWriter = ml.tracks().writer(txn);
    auto resourceWriter = ml.resources().writer(txn);

    for (const std::filesystem::path& path : finder)
    {
      rs::tag::flac::File file{path.string(), rs::tag::File::Mode::ReadOnly};
      auto metadata = file.loadMetadata();
      std::cout << path << std::endl;
      //std::cout << metadata.get(rs::tag::MetaField::AlbumArtist).index() << std::endl;

      auto [id, track] = trackWriter.create([&metadata, &resourceWriter](::flatbuffers::FlatBufferBuilder& fbb) {
        ::flatbuffers::Offset<rs::ml::fbs::Metadata> metaOffset;
        {
          auto titleOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Title));
          auto albumOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Album));
          auto artistOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Artist));
          auto albumArtistOffset = buildString(fbb, metadata.get(rs::tag::MetaField::AlbumArtist));
          auto genreOffset = buildString(fbb, metadata.get(rs::tag::MetaField::Genre));

          rs::ml::fbs::MetadataBuilder builder{fbb};
          builder.add_title(titleOffset);
          builder.add_album(albumOffset);
          builder.add_artist(artistOffset);
          builder.add_albumArtist(albumArtistOffset);
          builder.add_genre(genreOffset);
          metaOffset = builder.Finish();
        }

        std::vector<::flatbuffers::Offset<rs::ml::fbs::Resource>> rsrc;

        if (auto albumArt = metadata.get(rs::tag::MetaField::AlbumArt); !rs::tag::isNull(albumArt))
        {
          const auto& blob = std::get<rs::tag::Blob>(albumArt);
          std::uint64_t id = resourceWriter.create(boost::asio::buffer(blob.data(), blob.size()));
          rs::ml::fbs::ResourceBuilder builder{fbb};
          builder.add_type(rs::ml::fbs::ResourceType::AlbumArt);
          builder.add_id(id);
          rsrc.push_back(builder.Finish());
        }

        auto rsrcOffset = fbb.CreateVector(rsrc);

        /*         std::vector<std::string> t{"tag", "tag1", "tag2"};
                auto tags = fbb.CreateVectorOfStrings(t);
                std::vector<flatbuffers::Offset<rs::ml::core::CustomEntry>> entries;
                entries.push_back(rs::ml::core::CreateCustomEntry(fbb, fbb.CreateString("pop")));
                entries.push_back(rs::ml::core::CreateCustomEntry(fbb, fbb.CreateString("classic")));
                auto custom = fbb.CreateVectorOfSortedTables(&entries);
                */

        rs::ml::fbs::TrackBuilder builder{fbb};
        builder.add_meta(metaOffset);
        builder.add_rsrc(rsrcOffset);
        // builder.add_custom(custom);
        return builder.Finish();
      });
      auto cstr = [](const flatbuffers::String* str) { return str == nullptr ? "nil" : str->str(); };
      os << "add track: " << id << " " << cstr(track->meta()->title()) << std::endl;
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
