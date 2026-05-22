// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/library/LibraryScanner.h"

#include "ao/library/FileManifestStore.h"
#include "ao/library/MusicLibrary.h"
#include "ao/utility/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>

namespace ao::library
{
  namespace
  {
    bool isSupportedExtension(std::string const& ext)
    {
      static std::unordered_set<std::string> const supported = {".flac", ".mp3", ".m4a", ".wav", ".alac", ".ogg"};
      return supported.contains(ext);
    }
  }

  LibraryScanner::LibraryScanner(MusicLibrary& ml)
    : _ml{ml}
  {
  }

  ScanPlan LibraryScanner::buildPlan(ProgressCallback progress)
  {
    auto plan = ScanPlan{};
    auto const root = _ml.rootPath();

    if (!std::filesystem::exists(root))
    {
      APP_LOG_ERROR("LibraryScanner: Root path does not exist: {}", root.string());
      return plan;
    }

    auto txn = _ml.readTransaction();
    auto const manifestReader = _ml.manifest().reader(txn);

    // Track which URIs we've seen on disk to identify MISSING tracks later
    auto seenUris = std::unordered_set<std::string>{};

    // 1. Walk Filesystem
    try
    {
      for (auto const& entry : std::filesystem::recursive_directory_iterator{root})
      {
        if (progress)
        {
          progress(entry.path());
        }

        if (!entry.is_regular_file())
        {
          continue;
        }

        auto const& path = entry.path();
        auto ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return std::tolower(ch); });

        auto const uri = std::filesystem::relative(path, root).string();
        seenUris.insert(uri);

        auto item = ScanItem{.uri = uri, .fullPath = path, .classification = ScanClassification::Error};

        if (!isSupportedExtension(ext))
        {
          item.classification = ScanClassification::Unsupported;
          plan.items.push_back(std::move(item));
          continue;
        }

        try
        {
          item.fileSize = std::filesystem::file_size(path);
          item.mtime = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                    std::filesystem::last_write_time(path).time_since_epoch())
                                                    .count());

          if (auto const optView = manifestReader.get(uri))
          {
            item.trackId = optView->trackId();

            if (optView->fileSize() == item.fileSize && optView->mtime() == item.mtime)
            {
              item.classification = ScanClassification::Unchanged;
            }
            else
            {
              item.classification = ScanClassification::Changed;
            }
          }
          else
          {
            item.classification = ScanClassification::New;
          }
        }
        catch (std::exception const& e)
        {
          item.classification = ScanClassification::Error;
          item.errorMessage = e.what();
        }

        plan.items.push_back(std::move(item));
      }
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("LibraryScanner: Fatal error during FS walk: {}", e.what());
    }

    // 2. Identify MISSING (In manifest but not on disk)
    for (auto const& [uriView, view] : manifestReader)
    {
      if (auto const uri = std::string{uriView}; !seenUris.contains(uri))
      {
        auto item = ScanItem{.uri = uri,
                             .fullPath = {},
                             .classification = ScanClassification::Missing,
                             .fileSize = view.fileSize(),
                             .mtime = view.mtime(),
                             .trackId = view.trackId(),
                             .errorMessage = {}};
        plan.items.push_back(std::move(item));
      }
    }

    return plan;
  }
} // namespace ao::library
