// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "InitCommand.h"
#include <rs/core/MusicLibrary.h>
#include <rs/tag/File.h>
#include <rs/utility/Finder.h>

#include <iostream>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  void scanAndImport(core::MusicLibrary& ml, std::ostream& os)
  {
    utility::Finder finder{".", {".flac", ".m4a", ".mp3"}};
    auto txn = ml.writeTransaction();
    auto writer = ml.tracks().writer(txn);
    auto& dict = ml.dictionary();

    for (std::filesystem::path const& path : finder)
    {
      try
      {
        auto tagFile = tag::File::open(path);
        if (!tagFile) { continue; }

        auto builder = tagFile->loadTrack();
        builder.property()
          .uri(path.string())
          .fileSize(std::filesystem::file_size(path))
          .mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

        auto [hotData, coldData] = builder.serialize(txn, dict, ml.resources());
        auto [id, trackView] = writer.createHotCold(hotData, coldData);
        os << "add track: " << id << " " << trackView.metadata().title() << '\n';
      }
      catch (std::exception const& e)
      {
        std::cerr << "failed to parse metadata for " << path.filename() << ": " << e.what() << '\n';
      }
    }

    txn.commit();
  }
}

InitCommand::InitCommand(core::MusicLibrary& ml)
{
  setExecutor([&ml]([[maybe_unused]] auto const& vm, auto& os) { scanAndImport(ml, os); });
}
