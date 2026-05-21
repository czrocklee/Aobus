// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "InitCommand.h"

#include "ao/Type.h"
#include "ao/library/FileManifestStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackStore.h"
#include "ao/tag/TagFile.h"
#include "ao/utility/Finder.h"
#include "runtime/CoreRuntime.h"

#include <CLI/App.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>

namespace ao::cli
{
  namespace
  {
    void scanAndImport(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const finder = utility::Finder{".", {".flac", ".m4a", ".mp3"}};
      auto txn = ml.writeTransaction();
      auto writer = ml.tracks().writer(txn);
      auto manifestWriter = ml.manifest().writer(txn);
      auto& dict = ml.dictionary();

      for (std::filesystem::path const& path : finder.paths())
      {
        try
        {
          auto const optTagFile = tag::TagFile::open(path);

          if (!optTagFile)
          {
            continue;
          }

          auto builder = optTagFile->loadTrack();
          // NOTE: pathStr must outlive builder because PropertyBuilder stores string_view
          auto const pathStr = path.string();
          builder.property()
            .uri(pathStr)
            .fileSize(std::filesystem::file_size(path))
            .mtime(std::filesystem::last_write_time(path).time_since_epoch().count());

          auto const [preparedHot, preparedCold] = builder.prepare(txn, dict, ml.resources());
          auto const [id, trackView] = writer.createHotCold(
            preparedHot.size(),
            preparedCold.size(),
            [&preparedHot, &preparedCold](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
            {
              preparedHot.writeTo(hot);
              preparedCold.writeTo(cold);
            });

          // Populate Manifest
          auto manifestEntry = library::ManifestEntry{.trackId = id};
          manifestEntry.fileSize(builder.property().fileSize());
          manifestEntry.mtime(builder.property().mtime());
          manifestWriter.put(pathStr, manifestEntry);

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

  void setupInitCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* const cmd = app.add_subcommand("init", "Scan current directory and initialize library");
    cmd->callback([&runtime] { scanAndImport(runtime.musicLibrary(), std::cout); });
  }
}
