// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "InitCommand.h"
#include <rs/library/MusicLibrary.h>
#include <rs/tag/File.h>
#include <rs/utility/Finder.h>

#include <iostream>

namespace rs::tool
{
  namespace
  {
    void scanAndImport(rs::library::MusicLibrary& ml, std::ostream& os)
    {
      utility::Finder finder{".", {".flac", ".m4a", ".mp3"}};
      auto txn = ml.writeTransaction();
      auto writer = ml.tracks().writer(txn);
      auto& dict = ml.dictionary();

      for (std::filesystem::path const& path : finder.paths())
      {
        try
        {
          auto tagFile = tag::File::open(path);

          if (!tagFile)
          {
            continue;
          }

          auto builder = tagFile->loadTrack();
          // NOTE: pathStr must outlive builder because PropertyBuilder stores string_view
          auto const pathStr = path.string();
          builder.property()
            .uri(pathStr)
            .fileSize(std::filesystem::file_size(path))
            .mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

          auto [preparedHot, preparedCold] = builder.prepare(txn, dict, ml.resources());
          auto [id, trackView] = writer.createHotCold(
            preparedHot.size(),
            preparedCold.size(),
            [&preparedHot, &preparedCold](rs::TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
            {
              preparedHot.writeTo(hot);
              preparedCold.writeTo(cold);
            });
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

  void setupInitCommand(CLI::App& app, rs::library::MusicLibrary& ml)
  {
    auto* cmd = app.add_subcommand("init", "Scan current directory and initialize library");
    cmd->callback([&ml]() { scanAndImport(ml, std::cout); });
  }
}
