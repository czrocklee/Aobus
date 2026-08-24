// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryScan.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Path.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
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
    struct ManifestSnapshotEntry final
    {
      std::string uri;
      TrackId trackId = kInvalidTrackId;
      std::uint64_t fileSize = 0;
      std::uint64_t mtime = 0;
      std::uint64_t audioPayloadLength = 0;
      utility::Hash128 audioSignature = {};
      library::FileStatus status = library::FileStatus::Available;
    };

    class ManifestSnapshot final
    {
    public:
      explicit ManifestSnapshot(std::vector<ManifestSnapshotEntry> entries)
        : _entries{std::move(entries)}
      {
      }

      ManifestSnapshotEntry const* find(std::string_view const uri) const
      {
        auto const it = std::ranges::lower_bound(_entries, uri, {}, &ManifestSnapshotEntry::uri);
        return it != _entries.end() && it->uri == uri ? &*it : nullptr;
      }

      std::span<ManifestSnapshotEntry const> entries() const noexcept { return _entries; }

    private:
      std::vector<ManifestSnapshotEntry> _entries;
    };

    struct PlanSnapshot final
    {
      std::array<std::byte, 16> libraryId{};
      std::uint64_t libraryRevision = 0;
      ManifestSnapshot manifest{std::vector<ManifestSnapshotEntry>{}};
    };

    PlanSnapshot capturePlanSnapshot(library::MusicLibrary const& source, std::stop_token const stopToken)
    {
      async::throwIfStopRequested(stopToken);

      auto transaction = source.readTransaction();
      auto const header = source.metadataHeader(transaction);
      auto const libraryRevision = source.libraryRevision(transaction);
      auto const manifestReader = source.manifest().reader(transaction);
      auto entries = std::vector<ManifestSnapshotEntry>{};

      for (auto const& [uri, view] : manifestReader)
      {
        async::throwIfStopRequested(stopToken);
        entries.push_back(ManifestSnapshotEntry{.uri = std::string{uri},
                                                .trackId = view.trackId(),
                                                .fileSize = view.fileSize(),
                                                .mtime = view.mtime(),
                                                .audioPayloadLength = view.audioPayloadLength(),
                                                .audioSignature = view.audioSignature(),
                                                .status = view.status()});
      }

      return PlanSnapshot{
        .libraryId = header.libraryId,
        .libraryRevision = libraryRevision,
        .manifest = ManifestSnapshot{std::move(entries)},
      };
    }

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

    Result<> scanEntry(std::filesystem::path const& path,
                       std::string const& uri,
                       ManifestSnapshot const& manifest,
                       std::unordered_set<std::string>& seenUris,
                       std::vector<ScanItem>& items)
    {
      auto entryEc = std::error_code{};
      bool isFile = false;

      isFile = std::filesystem::is_regular_file(path, entryEc);

      if (entryEc)
      {
        seenUris.insert(uri);
        auto item = ScanItem{
          .uri = uri, .fullPath = path, .classification = ScanClassification::Error, .errorMessage = entryEc.message()};
        items.push_back(std::move(item));
        return {};
      }

      if (!isFile)
      {
        return {};
      }

      // Only files we can actually decode belong in the plan. Everything else -
      // cover art, playlists, logs, or formats we have no reader for (.ogg,
      // a literal .alac) - is not music we support and is ignored here.
      if (!media::file::File::isSupported(utility::pathFromUtf8(uri)))
      {
        return {};
      }

      if (!seenUris.insert(uri).second)
      {
        return {};
      }

      auto item = ScanItem{.uri = uri, .fullPath = path, .classification = ScanClassification::Error};

      item.fileSize = std::filesystem::file_size(path, entryEc);

      if (entryEc)
      {
        item.errorMessage = entryEc.message();
        items.push_back(std::move(item));
        return {};
      }

      auto const lastWriteTime = std::filesystem::last_write_time(path, entryEc);

      if (entryEc)
      {
        item.errorMessage = entryEc.message();
        items.push_back(std::move(item));
        return {};
      }

      item.mtime = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count());

      if (auto const* const manifestEntry = manifest.find(uri); manifestEntry == nullptr)
      {
        item.classification = ScanClassification::New;
      }
      else
      {
        item.trackId = manifestEntry->trackId;
        item.audioPayloadLength = manifestEntry->audioPayloadLength;
        item.audioSignature = manifestEntry->audioSignature;
        item.optManifestEvidence = ScanManifestEvidence{
          .fileSize = manifestEntry->fileSize,
          .mtime = manifestEntry->mtime,
          .audioPayloadLength = manifestEntry->audioPayloadLength,
          .audioSignature = manifestEntry->audioSignature,
          .status = manifestEntry->status,
        };

        if (manifestEntry->status != library::FileStatus::Missing && manifestEntry->fileSize == item.fileSize &&
            manifestEntry->mtime == item.mtime)
        {
          item.classification = ScanClassification::Unchanged;
        }
        else
        {
          item.classification = ScanClassification::Changed;
        }
      }

      items.push_back(std::move(item));
      return {};
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
                           ManifestSnapshot const& manifest,
                           std::unordered_set<std::string> const& seenUris,
                           std::unordered_set<std::string> const& blockedUriPrefixes,
                           std::stop_token const stopToken)
    {
      for (auto const& entry : manifest.entries())
      {
        async::throwIfStopRequested(stopToken);

        if (!hasBlockedUriPrefix(entry.uri, blockedUriPrefixes) && !seenUris.contains(entry.uri))
        {
          auto item = ScanItem{.uri = entry.uri,
                               .classification = ScanClassification::Missing,
                               .fileSize = entry.fileSize,
                               .mtime = entry.mtime,
                               .audioPayloadLength = entry.audioPayloadLength,
                               .audioSignature = entry.audioSignature,
                               .trackId = entry.trackId,
                               .optManifestEvidence = ScanManifestEvidence{
                                 .fileSize = entry.fileSize,
                                 .mtime = entry.mtime,
                                 .audioPayloadLength = entry.audioPayloadLength,
                                 .audioSignature = entry.audioSignature,
                                 .status = entry.status,
                               }};
          items.push_back(std::move(item));
        }
      }
    }

    void classifyMovedEntries(std::vector<ScanItem>& items, std::stop_token const stopToken)
    {
      auto missingByLength = std::unordered_map<std::uint64_t, std::vector<std::size_t>>{};
      auto missingByIdentity = std::unordered_map<AudioIdentityKey, std::vector<std::size_t>, AudioIdentityKeyHasher>{};

      for (std::size_t index = 0; index < items.size(); ++index)
      {
        async::throwIfStopRequested(stopToken);
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
        async::throwIfStopRequested(stopToken);
        auto& item = items[index];

        if (item.classification != ScanClassification::New)
        {
          continue;
        }

        auto fileRes = media::file::File::open(item.fullPath);

        if (!fileRes)
        {
          continue;
        }

        auto payloadRes = fileRes->audioPayload();

        if (!payloadRes)
        {
          continue;
        }

        item.audioPayloadLength = static_cast<std::uint64_t>(payloadRes->bytes.size());

        if (!missingByLength.contains(item.audioPayloadLength))
        {
          continue;
        }

        auto optIdentity = library::readAudioIdentity(payloadRes->bytes, {}, stopToken);

        if (!optIdentity)
        {
          async::throwIfStopRequested(stopToken);
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
        async::throwIfStopRequested(stopToken);
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
        newItem.optManifestEvidence = missingItem.optManifestEvidence;
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
        async::throwIfStopRequested(stopToken);

        if (!matchedMissingIndices.contains(index))
        {
          filteredItems.push_back(std::move(items[index]));
        }
      }

      items = std::move(filteredItems);
    }
  } // namespace

  LibraryScan::LibraryScan(library::MusicLibrary const& library)
    : _library{library}
  {
  }

  Result<ScanPlan> LibraryScan::buildPlan(BuildProgressCallback progress, std::stop_token const stopToken)
  {
    return buildPlanUnchecked(std::move(progress), stopToken);
  }

  Result<ScanPlan> LibraryScan::buildPlanUnchecked(BuildProgressCallback progress, std::stop_token const stopToken)
  {
    async::throwIfStopRequested(stopToken);
    auto const root = _library.rootPath();

    if (auto rootEc = std::error_code{}; !std::filesystem::exists(root, rootEc))
    {
      if (rootEc)
      {
        return makeError(Error::Code::IoError,
                         "Failed to inspect library root path " + utility::pathToUtf8(root) + ": " + rootEc.message());
      }

      return makeError(Error::Code::NotFound, "Library root path does not exist: " + utility::pathToUtf8(root));
    }

    auto rootEc = std::error_code{};
    auto const resolvedRoot = std::filesystem::weakly_canonical(root, rootEc);

    if (rootEc)
    {
      return makeError(Error::Code::IoError,
                       "Failed to resolve library root path " + utility::pathToUtf8(root) + ": " + rootEc.message());
    }

    auto snapshot = capturePlanSnapshot(_library, stopToken);
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
        Error::Code::IoError, "Failed to start filesystem walk of " + utility::pathToUtf8(root) + ": " + ec.message());
    }

    while (it != std::filesystem::recursive_directory_iterator{})

    {
      async::throwIfStopRequested(stopToken);
      auto entryEc = std::error_code{};
      auto const& entry = *it;

      if (progress)
      {
        progress(entry.path());
        async::throwIfStopRequested(stopToken);
      }

      auto uriRes = library::LibraryUri::parse(utility::pathToGenericUtf8(entry.path().lexically_relative(root)));

      if (!uriRes)
      {
        auto item = ScanItem{.uri = utility::pathToGenericUtf8(entry.path().filename()),
                             .fullPath = entry.path(),
                             .classification = ScanClassification::Error,
                             .errorMessage = uriRes.error().message};
        items.push_back(std::move(item));
        it.disable_recursion_pending();
        it.increment(ec);
        ec.clear();
        continue;
      }

      auto resolvedPathRes = uriRes->resolveUnder(root);

      if (!resolvedPathRes)
      {
        blockedUriPrefixes.insert(std::string{uriRes->value()});
        auto item = ScanItem{.uri = std::string{uriRes->value()},
                             .fullPath = entry.path(),
                             .classification = ScanClassification::Error,
                             .errorMessage = resolvedPathRes.error().message};
        items.push_back(std::move(item));
        it.disable_recursion_pending();
        it.increment(ec);
        ec.clear();
        continue;
      }

      auto canonicalUriRes =
        library::LibraryUri::parse(utility::pathToGenericUtf8(resolvedPathRes->lexically_relative(resolvedRoot)));

      if (!canonicalUriRes)
      {
        blockedUriPrefixes.insert(std::string{uriRes->value()});
        auto item = ScanItem{.uri = std::string{uriRes->value()},
                             .fullPath = *resolvedPathRes,
                             .classification = ScanClassification::Error,
                             .errorMessage = canonicalUriRes.error().message};
        items.push_back(std::move(item));
        it.disable_recursion_pending();
        it.increment(ec);
        ec.clear();
        continue;
      }

      // Proactively check if this is a directory we can't enter
      if (std::filesystem::is_directory(*resolvedPathRes, entryEc))
      {
        auto testEc = std::error_code{};
        {
          [[maybe_unused]] auto const testIt = std::filesystem::directory_iterator{*resolvedPathRes, testEc};
        }

        if (testEc)
        {
          blockedUriPrefixes.insert(std::string{canonicalUriRes->value()});
          auto item = ScanItem{.uri = std::string{canonicalUriRes->value()},
                               .fullPath = *resolvedPathRes,
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

      if (auto result =
            scanEntry(*resolvedPathRes, std::string{canonicalUriRes->value()}, snapshot.manifest, seenUris, items);
          !result)
      {
        return std::unexpected{result.error()};
      }

      it.increment(ec);

      if (ec)
      {
        ec.clear();
      }
    }

    // 2. Identify MISSING (In manifest but not on disk)
    addMissingEntries(items, snapshot.manifest, seenUris, blockedUriPrefixes, stopToken);
    classifyMovedEntries(items, stopToken);

    return ScanPlan{snapshot.libraryId, snapshot.libraryRevision, std::move(items)};
  }
} // namespace ao::rt
