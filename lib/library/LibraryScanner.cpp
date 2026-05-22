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
#include <system_error>
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

    void processEntry(std::filesystem::directory_entry const& entry,
                      std::filesystem::path const& root,
                      ao::library::FileManifestStore::Reader const& manifestReader,
                      std::unordered_set<std::string>& seenUris,
                      ScanPlan& plan)
    {
      auto entryEc = std::error_code{};
      bool isFile = false;

      try
      {
        isFile = entry.is_regular_file(entryEc);
      }
      catch (std::exception const& /*e*/)
      {
        isFile = false;
        entryEc = std::make_error_code(std::errc::permission_denied);
      }

      if (entryEc)
      {
        auto const uri = std::filesystem::relative(entry.path(), root, entryEc).generic_string();
        auto item = ScanItem{.uri = uri,
                             .fullPath = entry.path(),
                             .classification = ScanClassification::Error,
                             .errorMessage = entryEc.message()};
        plan.items.push_back(std::move(item));
        return;
      }

      if (!isFile)
      {
        return;
      }

      auto const& path = entry.path();
      auto ext = path.extension().string();
      std::ranges::transform(
        ext, ext.begin(), [](unsigned char ch) { return static_cast<unsigned char>(std::tolower(ch)); });

      auto const uri = std::filesystem::relative(path, root, entryEc).generic_string();
      seenUris.insert(uri);

      auto item = ScanItem{.uri = uri, .fullPath = path, .classification = ScanClassification::Error};

      if (!isSupportedExtension(ext))
      {
        item.classification = ScanClassification::Unsupported;
        plan.items.push_back(std::move(item));
        return;
      }

      try
      {
        item.fileSize = std::filesystem::file_size(path, entryEc);
        item.mtime = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::filesystem::last_write_time(path, entryEc).time_since_epoch())
                                                  .count());

        if (entryEc)
        {
          item.classification = ScanClassification::Error;
          item.errorMessage = entryEc.message();
        }
        else if (auto const optView = manifestReader.get(uri))
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
    auto ec = std::error_code{};
    auto it = std::filesystem::recursive_directory_iterator{root, ec};

    if (ec)
    {
      APP_LOG_ERROR("LibraryScanner: Fatal error starting FS walk: {}", ec.message());
      return plan;
    }

    while (it != std::filesystem::recursive_directory_iterator{})

    {
      auto entryEc = std::error_code{};
      auto const& entry = *it;

      if (progress)
      {
        progress(entry.path());
      }

      // Proactively check if this is a directory we can't enter
      if (entry.is_directory(entryEc))
      {
        auto testEc = std::error_code{};
        {
          auto const testIt = std::filesystem::directory_iterator{entry.path(), testEc};
          std::ignore = testIt;
        }

        if (testEc)
        {
          auto const uri = std::filesystem::relative(entry.path(), root, entryEc).generic_string();
          auto item = ScanItem{.uri = uri,
                               .fullPath = entry.path(),
                               .classification = ScanClassification::Error,
                               .errorMessage = testEc.message()};
          plan.items.push_back(std::move(item));

          it.disable_recursion_pending();
          it.increment(ec);

          if (ec)
          {
            ec.clear();
          }

          continue;
        }
      }

      processEntry(entry, root, manifestReader, seenUris, plan);

      it.increment(ec);

      if (ec)
      {
        ec.clear();
      }
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
