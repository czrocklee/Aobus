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
  ::flatbuffers::Offset<::flatbuffers::String> buildString(::flatbuffers::FlatBufferBuilder& fbb,
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
    rs::ml::Finder finder{".", {".flac"}};

    auto txn = ml.writeTransaction();
    auto trackWriter = ml.tracks().writer(txn);
    auto resourceWriter = ml.resources().writer(txn);

    for (const std::filesystem::path& path : finder)
    {
      rs::tag::flac::File file{path.string(), rs::tag::File::Mode::ReadOnly};
      auto metadata = file.loadMetadata();

      
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
