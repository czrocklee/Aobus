// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "InitCommand.h"

#include <ao/CoreIds.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackWrite.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Finder.h>

#include <CLI/App.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <print>
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

            std::println(
              stderr, "failed to open metadata for {}: {}", path.filename().string(), tagFileResult.error().message);
            continue;
          }

          auto builderResult = (*tagFileResult)->loadTrack();

          if (!builderResult)
          {
            std::println(
              stderr, "failed to parse metadata for {}: {}", path.filename().string(), builderResult.error().message);
            continue;
          }

          auto builder = *builderResult;
          // NOTE: pathStr must outlive builder because PropertyBuilder stores string_view
          auto const pathStr = path.string();
          builder.property().uri(pathStr);

          auto preparedResult = builder.prepare(txn, dict, ml.resources());

          if (!preparedResult)
          {
            std::println(stderr,
                         "failed to serialize metadata for {}: {}",
                         path.filename().string(),
                         preparedResult.error().message);
            continue;
          }

          auto const& [preparedHot, preparedCold] = *preparedResult;
          auto createResult = library::createPreparedTrackData(writer, preparedHot, preparedCold);

          if (!createResult)
          {
            std::println(
              stderr, "failed to create track for {}: {}", path.filename().string(), createResult.error().message);
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
            std::println(
              stderr, "failed to update manifest for {}: {}", path.filename().string(), putResult.error().message);
            continue;
          }

          std::println(os, "add track: {} {}", id, trackView.metadata().title());
        }
        catch (std::exception const& e)
        {
          std::println(stderr, "failed to parse metadata for {}: {}", path.filename().string(), e.what());
        }
      }

      if (auto result = txn.commit(); !result)
      {
        std::println(stderr, "failed to commit imported tracks: {}", result.error().message);
      }
    }
  } // namespace

  void setupInitCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* const cmd = app.add_subcommand("init", "Scan current directory and initialize library");
    cmd->callback([&runtime] { scanAndImport(runtime.musicLibrary(), std::cout); });
  }
} // namespace ao::cli
