// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "InitCommand.h"
#include "TrackUtils.h"
#include <rs/core/MusicLibrary.h>
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
        auto record = loadTrackRecord(path, dict, txn);
        auto hotData = record.serializeHot();
        auto coldData = record.serializeCold(dict);
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
