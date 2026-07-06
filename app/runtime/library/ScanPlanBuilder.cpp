// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanPlanBuilder.h"

#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Hash128.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    struct AudioIdentityKey final
    {
      std::uint64_t payloadLength = 0;
      utility::Hash128 signature = {};

      bool operator==(AudioIdentityKey const&) const noexcept = default;
    };

    struct AudioIdentityKeyHasher final
    {
      std::size_t operator()(AudioIdentityKey const& key) const noexcept
      {
        auto seed = std::hash<std::uint64_t>{}(key.payloadLength);
        constexpr std::uint32_t kGoldenRatio = 0x9e3779b9U;
        constexpr std::size_t kShiftLeft = 6U;
        constexpr std::size_t kShiftRight = 2U;

        for (auto const byte : key.signature.bytes)
        {
          seed ^= std::hash<std::size_t>{}(std::to_integer<std::size_t>(byte)) + kGoldenRatio + (seed << kShiftLeft) +
                  (seed >> kShiftRight);
        }

        return seed;
      }
    };

    void processEntry(std::filesystem::directory_entry const& entry,
                      std::filesystem::path const& root,
                      library::FileManifestStore::Reader const& manifestReader,
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
                             .oldUri = {},
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

      // Only files we can actually decode belong in the plan. Everything else -
      // cover art, playlists, logs, or formats we have no reader for (.ogg,
      // a literal .alac) - is not music we support and is ignored here.
      if (!tag::TagFile::isSupported(path))
      {
        return;
      }

      auto const uri = std::filesystem::relative(path, root, entryEc).generic_string();
      seenUris.insert(uri);

      auto item = ScanItem{.uri = uri, .oldUri = {}, .fullPath = path, .classification = ScanClassification::Error};

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
        else if (auto manifestResult = manifestReader.get(uri); !manifestResult)
        {
          if (manifestResult.error().code == Error::Code::NotFound)
          {
            item.classification = ScanClassification::New;
          }
          else
          {
            item.classification = ScanClassification::Error;
            item.errorMessage = manifestResult.error().message;
          }
        }
        else
        {
          auto const& view = *manifestResult;
          item.trackId = view.trackId();
          item.audioPayloadLength = view.audioPayloadLength();
          item.audioSignature = view.audioSignature();

          if (view.fileSize() == item.fileSize && view.mtime() == item.mtime)
          {
            item.classification = ScanClassification::Unchanged;
          }
          else
          {
            item.classification = ScanClassification::Changed;
          }
        }
      }
      catch (std::exception const& e)
      {
        item.classification = ScanClassification::Error;
        item.errorMessage = e.what();
      }

      plan.items.push_back(std::move(item));
    }
    void addMissingEntries(ScanPlan& plan,
                           library::FileManifestStore::Reader const& manifestReader,
                           std::unordered_set<std::string> const& seenUris)
    {
      for (auto const& [uriView, view] : manifestReader)
      {
        if (auto const uri = std::string{uriView}; !seenUris.contains(uri))
        {
          auto item = ScanItem{.uri = uri,
                               .oldUri = {},
                               .fullPath = {},
                               .classification = ScanClassification::Missing,
                               .fileSize = view.fileSize(),
                               .mtime = view.mtime(),
                               .audioPayloadLength = view.audioPayloadLength(),
                               .audioSignature = view.audioSignature(),
                               .trackId = view.trackId(),
                               .errorMessage = {}};
          plan.items.push_back(std::move(item));
        }
      }
    }

    void classifyMovedEntries(ScanPlan& plan)
    {
      auto missingByLength = std::unordered_map<std::uint64_t, std::vector<std::size_t>>{};
      auto missingByIdentity = std::unordered_map<AudioIdentityKey, std::vector<std::size_t>, AudioIdentityKeyHasher>{};

      for (std::size_t index = 0; index < plan.items.size(); ++index)
      {
        auto const& item = plan.items[index];

        if (item.classification != ScanClassification::Missing || !hasAudioIdentity(item))
        {
          continue;
        }

        auto const key = AudioIdentityKey{.payloadLength = item.audioPayloadLength, .signature = item.audioSignature};
        missingByLength[item.audioPayloadLength].push_back(index);
        missingByIdentity[key].push_back(index);
      }

      if (missingByLength.empty())
      {
        return;
      }

      auto newByIdentity = std::unordered_map<AudioIdentityKey, std::vector<std::size_t>, AudioIdentityKeyHasher>{};

      for (std::size_t index = 0; index < plan.items.size(); ++index)
      {
        auto& item = plan.items[index];

        if (item.classification != ScanClassification::New)
        {
          continue;
        }

        auto tagFileResult = tag::TagFile::open(item.fullPath);

        if (!tagFileResult)
        {
          continue;
        }

        auto payloadResult = (*tagFileResult)->audioPayload();

        if (!payloadResult)
        {
          continue;
        }

        item.audioPayloadLength = static_cast<std::uint64_t>(payloadResult->bytes.size());

        if (!missingByLength.contains(item.audioPayloadLength))
        {
          continue;
        }

        auto identityResult = library::readAudioIdentity(**tagFileResult);

        if (!identityResult || !*identityResult)
        {
          continue;
        }

        item.audioPayloadLength = (*identityResult)->payloadLength;
        item.audioSignature = (*identityResult)->signature;
        auto const key = AudioIdentityKey{.payloadLength = item.audioPayloadLength, .signature = item.audioSignature};
        newByIdentity[key].push_back(index);
      }

      auto matchedMissingIndexes = std::unordered_set<std::size_t>{};

      for (auto const& [key, newIndexes] : newByIdentity)
      {
        auto const missingIt = missingByIdentity.find(key);

        if (missingIt == missingByIdentity.end())
        {
          continue;
        }

        auto const& missingIndexes = missingIt->second;

        if (newIndexes.size() != 1 || missingIndexes.size() != 1)
        {
          continue;
        }

        auto& newItem = plan.items[newIndexes.front()];
        auto const& missingItem = plan.items[missingIndexes.front()];
        newItem.classification = ScanClassification::Moved;
        newItem.oldUri = missingItem.uri;
        newItem.trackId = missingItem.trackId;
        matchedMissingIndexes.insert(missingIndexes.front());
      }

      if (matchedMissingIndexes.empty())
      {
        return;
      }

      auto items = std::vector<ScanItem>{};
      items.reserve(plan.items.size() - matchedMissingIndexes.size());

      for (std::size_t index = 0; index < plan.items.size(); ++index)
      {
        if (!matchedMissingIndexes.contains(index))
        {
          items.push_back(std::move(plan.items[index]));
        }
      }

      plan.items = std::move(items);
    }
  } // namespace

  ScanPlanBuilder::ScanPlanBuilder(library::MusicLibrary& ml)
    : _ml{ml}
  {
  }

  Result<ScanPlan> ScanPlanBuilder::buildPlan(ProgressCallback progress)
  {
    auto plan = ScanPlan{};
    auto const root = _ml.rootPath();

    if (auto rootEc = std::error_code{}; !std::filesystem::exists(root, rootEc))
    {
      if (rootEc)
      {
        return makeError(
          Error::Code::IoError, "Failed to inspect library root path " + root.string() + ": " + rootEc.message());
      }

      return makeError(Error::Code::NotFound, "Library root path does not exist: " + root.string());
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
      return makeError(
        Error::Code::IoError, "Failed to start filesystem walk of " + root.string() + ": " + ec.message());
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
          [[maybe_unused]] auto const testIt = std::filesystem::directory_iterator{entry.path(), testEc};
        }

        if (testEc)
        {
          auto const uri = std::filesystem::relative(entry.path(), root, entryEc).generic_string();
          auto item = ScanItem{.uri = uri,
                               .oldUri = {},
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
    addMissingEntries(plan, manifestReader, seenUris);
    classifyMovedEntries(plan);

    return plan;
  }
} // namespace ao::rt
