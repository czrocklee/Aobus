// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanPlanBuilder.h"

#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
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

    void scanEntry(std::filesystem::path const& path,
                   std::string const& uri,
                   library::FileManifestStore::Reader const& manifestReader,
                   std::unordered_set<std::string>& seenUris,
                   std::vector<ScanItem>& items)
    {
      auto entryEc = std::error_code{};
      bool isFile = false;

      try
      {
        isFile = std::filesystem::is_regular_file(path, entryEc);
      }
      catch (std::exception const& /*e*/)
      {
        isFile = false;
        entryEc = std::make_error_code(std::errc::permission_denied);
      }

      if (entryEc)
      {
        seenUris.insert(uri);
        auto item = ScanItem{
          .uri = uri, .fullPath = path, .classification = ScanClassification::Error, .errorMessage = entryEc.message()};
        items.push_back(std::move(item));
        return;
      }

      if (!isFile)
      {
        return;
      }

      // Only files we can actually decode belong in the plan. Everything else -
      // cover art, playlists, logs, or formats we have no reader for (.ogg,
      // a literal .alac) - is not music we support and is ignored here.
      if (!media::file::File::isSupported(std::filesystem::path{uri}))
      {
        return;
      }

      if (!seenUris.insert(uri).second)
      {
        return;
      }

      auto item = ScanItem{.uri = uri, .fullPath = path, .classification = ScanClassification::Error};

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

      items.push_back(std::move(item));
    }
    bool hasBlockedUriPrefix(std::string_view uri, std::unordered_set<std::string> const& blockedUriPrefixes)
    {
      while (!uri.empty())
      {
        if (blockedUriPrefixes.contains(std::string{uri}))
        {
          return true;
        }

        auto const separator = uri.rfind('/');

        if (separator == std::string_view::npos)
        {
          return false;
        }

        uri = uri.substr(0, separator);
      }

      return false;
    }

    void addMissingEntries(std::vector<ScanItem>& items,
                           library::FileManifestStore::Reader const& manifestReader,
                           std::unordered_set<std::string> const& seenUris,
                           std::unordered_set<std::string> const& blockedUriPrefixes)
    {
      for (auto const& [uriView, view] : manifestReader)
      {
        if (auto const uri = std::string{uriView};
            !hasBlockedUriPrefix(uriView, blockedUriPrefixes) && !seenUris.contains(uri))
        {
          auto item = ScanItem{.uri = uri,
                               .classification = ScanClassification::Missing,
                               .fileSize = view.fileSize(),
                               .mtime = view.mtime(),
                               .audioPayloadLength = view.audioPayloadLength(),
                               .audioSignature = view.audioSignature(),
                               .trackId = view.trackId()};
          items.push_back(std::move(item));
        }
      }
    }

    void classifyMovedEntries(std::vector<ScanItem>& items)
    {
      auto missingByLength = std::unordered_map<std::uint64_t, std::vector<std::size_t>>{};
      auto missingByIdentity = std::unordered_map<AudioIdentityKey, std::vector<std::size_t>, AudioIdentityKeyHasher>{};

      for (std::size_t index = 0; index < items.size(); ++index)
      {
        auto const& item = items[index];

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

      for (std::size_t index = 0; index < items.size(); ++index)
      {
        auto& item = items[index];

        if (item.classification != ScanClassification::New)
        {
          continue;
        }

        auto fileResult = media::file::File::open(item.fullPath);

        if (!fileResult)
        {
          continue;
        }

        auto payloadResult = fileResult->audioPayload();

        if (!payloadResult)
        {
          continue;
        }

        item.audioPayloadLength = static_cast<std::uint64_t>(payloadResult->bytes.size());

        if (!missingByLength.contains(item.audioPayloadLength))
        {
          continue;
        }

        auto optIdentity = library::readAudioIdentity(payloadResult->bytes);

        if (!optIdentity)
        {
          continue;
        }

        item.audioPayloadLength = optIdentity->payloadLength;
        item.audioSignature = optIdentity->signature;
        auto const key = AudioIdentityKey{.payloadLength = item.audioPayloadLength, .signature = item.audioSignature};
        newByIdentity[key].push_back(index);
      }

      auto matchedMissingIndices = std::unordered_set<std::size_t>{};

      for (auto const& [key, newIndices] : newByIdentity)
      {
        auto const missingIt = missingByIdentity.find(key);

        if (missingIt == missingByIdentity.end())
        {
          continue;
        }

        auto const& missingIndices = missingIt->second;

        if (newIndices.size() != 1 || missingIndices.size() != 1)
        {
          continue;
        }

        auto& newItem = items[newIndices.front()];
        auto const& missingItem = items[missingIndices.front()];
        newItem.classification = ScanClassification::Moved;
        newItem.oldUri = missingItem.uri;
        newItem.trackId = missingItem.trackId;
        matchedMissingIndices.insert(missingIndices.front());
      }

      if (matchedMissingIndices.empty())
      {
        return;
      }

      auto filteredItems = std::vector<ScanItem>{};
      filteredItems.reserve(items.size() - matchedMissingIndices.size());

      for (std::size_t index = 0; index < items.size(); ++index)
      {
        if (!matchedMissingIndices.contains(index))
        {
          filteredItems.push_back(std::move(items[index]));
        }
      }

      items = std::move(filteredItems);
    }
  } // namespace

  ScanPlanBuilder::ScanPlanBuilder(library::MusicLibrary const& ml)
    : _ml{ml}
  {
  }

  Result<ScanPlan> ScanPlanBuilder::buildPlan(ProgressCallback progress)
  {
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

    auto rootEc = std::error_code{};
    auto const resolvedRoot = std::filesystem::weakly_canonical(root, rootEc);

    if (rootEc)
    {
      return makeError(
        Error::Code::IoError, "Failed to resolve library root path " + root.string() + ": " + rootEc.message());
    }

    auto transaction = _ml.readTransaction();
    auto const headerResult = _ml.metadata().load(transaction);

    if (!headerResult)
    {
      return std::unexpected{headerResult.error()};
    }

    auto const libraryRevision = _ml.libraryRevision(transaction);
    auto const manifestReader = _ml.manifest().reader(transaction);
    auto items = std::vector<ScanItem>{};

    // Track which URIs we've seen on disk to identify MISSING tracks later
    auto seenUris = std::unordered_set<std::string>{};
    // A present entry that cannot be inspected safely prevents both that URI
    // and its descendants from being classified as missing.
    auto blockedUriPrefixes = std::unordered_set<std::string>{};

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

      auto uri = library::LibraryUri::parse(entry.path().lexically_relative(root).generic_string());

      if (!uri)
      {
        auto item = ScanItem{.uri = entry.path().filename().generic_string(),
                             .fullPath = entry.path(),
                             .classification = ScanClassification::Error,
                             .errorMessage = uri.error().message};
        items.push_back(std::move(item));
        it.disable_recursion_pending();
        it.increment(ec);
        ec.clear();
        continue;
      }

      auto resolvedPath = uri->resolveUnder(root);

      if (!resolvedPath)
      {
        blockedUriPrefixes.insert(std::string{uri->value()});
        auto item = ScanItem{.uri = std::string{uri->value()},
                             .fullPath = entry.path(),
                             .classification = ScanClassification::Error,
                             .errorMessage = resolvedPath.error().message};
        items.push_back(std::move(item));
        it.disable_recursion_pending();
        it.increment(ec);
        ec.clear();
        continue;
      }

      auto canonicalUri = library::LibraryUri::parse(resolvedPath->lexically_relative(resolvedRoot).generic_string());

      if (!canonicalUri)
      {
        blockedUriPrefixes.insert(std::string{uri->value()});
        auto item = ScanItem{.uri = std::string{uri->value()},
                             .fullPath = *resolvedPath,
                             .classification = ScanClassification::Error,
                             .errorMessage = canonicalUri.error().message};
        items.push_back(std::move(item));
        it.disable_recursion_pending();
        it.increment(ec);
        ec.clear();
        continue;
      }

      // Proactively check if this is a directory we can't enter
      if (std::filesystem::is_directory(*resolvedPath, entryEc))
      {
        auto testEc = std::error_code{};
        {
          [[maybe_unused]] auto const testIt = std::filesystem::directory_iterator{*resolvedPath, testEc};
        }

        if (testEc)
        {
          blockedUriPrefixes.insert(std::string{canonicalUri->value()});
          auto item = ScanItem{.uri = std::string{canonicalUri->value()},
                               .fullPath = *resolvedPath,
                               .classification = ScanClassification::Error,
                               .errorMessage = testEc.message()};
          items.push_back(std::move(item));

          it.disable_recursion_pending();
          it.increment(ec);

          if (ec)
          {
            ec.clear();
          }

          continue;
        }
      }

      scanEntry(*resolvedPath, std::string{canonicalUri->value()}, manifestReader, seenUris, items);

      it.increment(ec);

      if (ec)
      {
        ec.clear();
      }
    }

    // 2. Identify MISSING (In manifest but not on disk)
    addMissingEntries(items, manifestReader, seenUris, blockedUriPrefixes);
    classifyMovedEntries(items);

    return ScanPlan{headerResult->libraryId, libraryRevision, std::move(items)};
  }
} // namespace ao::rt
