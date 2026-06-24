// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "InitCommand.h"

#include <ao/Type.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Finder.h>

#include <CLI/App.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
          auto tagFileResult = tag::TagFile::open(path);

          if (!tagFileResult)
          {
            if (tagFileResult.error().code == Error::Code::NotSupported)
            {
              continue;
            }

            std::cerr << "failed to open metadata for " << path.filename() << ": " << tagFileResult.error().message
                      << '\n';
            continue;
          }

          auto builderResult = (*tagFileResult)->loadTrack();

          if (!builderResult)
          {
            std::cerr << "failed to parse metadata for " << path.filename() << ": " << builderResult.error().message
                      << '\n';
            continue;
          }

          auto builder = *builderResult;
          // NOTE: pathStr must outlive builder because PropertyBuilder stores string_view
          auto const pathStr = path.string();
          builder.property().uri(pathStr);

          auto preparedResult = builder.prepare(txn, dict, ml.resources());

          if (!preparedResult)
          {
            std::cerr << "failed to serialize metadata for " << path.filename() << ": "
                      << preparedResult.error().message << '\n';
            continue;
          }

          auto const& [preparedHot, preparedCold] = *preparedResult;
          auto createResult = writer.createHotCold(
            preparedHot.size(),
            preparedCold.size(),
            [&preparedHot, &preparedCold](TrackId, std::span<std::byte> hot, std::span<std::byte> cold)
            {
              preparedHot.writeTo(hot);
              preparedCold.writeTo(cold);
            });

          if (!createResult)
          {
            std::cerr << "failed to create track for " << path.filename() << ": " << createResult.error().message
                      << '\n';
            continue;
          }

          auto const [id, trackView] = *createResult;

          // Populate Manifest
          auto manifestBuilder = library::FileManifestBuilder::createNew();
          manifestBuilder.trackId(id)
            .fileSize(std::filesystem::file_size(path))
            .mtime(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::filesystem::last_write_time(path).time_since_epoch())
                                                .count()));

          if (auto putResult = manifestWriter.put(pathStr, manifestBuilder.serialize()); !putResult)
          {
            std::cerr << "failed to update manifest for " << path.filename() << ": " << putResult.error().message
                      << '\n';
            continue;
          }

          os << "add track: " << id << " " << trackView.metadata().title() << '\n';
        }
        catch (std::exception const& e)
        {
          std::cerr << "failed to parse metadata for " << path.filename() << ": " << e.what() << '\n';
        }
      }

      if (auto result = txn.commit(); !result)
      {
        std::cerr << "failed to commit imported tracks: " << result.error().message << '\n';
      }
    }
  } // namespace

  void setupInitCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* const cmd = app.add_subcommand("init", "Scan current directory and initialize library");
    cmd->callback([&runtime] { scanAndImport(runtime.musicLibrary(), std::cout); });
  }
} // namespace ao::cli
