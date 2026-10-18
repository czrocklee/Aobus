// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryScan.h>

#include "CueBinding.h"
#include "MediaTrack.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/FileTimeConversion.h>
#include <ao/library/LibraryUri.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/media/cue/Sheet.h>
#include <ao/media/file/File.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
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
      std::optional<FileTimestamp> optMtime;
      std::uint64_t audioPayloadLength = 0;
      utility::Hash128 audioSignature = {};
      library::FileStatus status = library::FileStatus::Available;
      std::uint64_t totalFrames = 0;
      std::uint32_t sampleRate = 0;
      std::vector<ScanCarrierMember> members{};
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
      auto const trackReader = source.tracks().reader(transaction);
      auto entries = std::vector<ManifestSnapshotEntry>{};

      for (auto const& [uri, view] : manifestReader)
      {
        async::throwIfStopRequested(stopToken);
        entries.push_back(ManifestSnapshotEntry{.uri = std::string{uri},
                                                .trackId = view.trackId(),
                                                .fileSize = view.fileSize(),
                                                .optMtime = view.mtime(),
                                                .audioPayloadLength = view.audioPayloadLength(),
                                                .audioSignature = view.audioSignature(),
                                                .status = view.status(),
                                                .totalFrames = view.totalFrames()});

        for (auto const id : view.segmentTrackIds())
        {
          auto const optTrack = trackReader.get(id, library::TrackStore::Reader::LoadMode::Both);
          AO_INVARIANT(optTrack, "Carrier member lost its Track");
          auto const optSegment = optTrack->property().segment();
          AO_INVARIANT(optSegment, "Carrier member lost its segment");
          entries.back().sampleRate = optTrack->property().sampleRate().raw();
          entries.back().members.push_back({.trackId = id, .segment = *optSegment});
        }
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

      auto const timestampRes = library::lastWriteTimestamp(path);

      if (!timestampRes)
      {
        item.errorMessage = timestampRes.error().message;
        items.push_back(std::move(item));
        return {};
      }

      item.optMtime = *timestampRes;

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
          .optMtime = manifestEntry->optMtime,
          .audioPayloadLength = manifestEntry->audioPayloadLength,
          .audioSignature = manifestEntry->audioSignature,
          .status = manifestEntry->status,
          .totalFrames = manifestEntry->totalFrames,
          .sampleRate = manifestEntry->sampleRate,
          .members = manifestEntry->members,
        };

        if (manifestEntry->status != library::FileStatus::Missing && manifestEntry->fileSize == item.fileSize &&
            manifestEntry->optMtime == item.optMtime)
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

    // Classifies one entry visited by the filesystem walk against the manifest
    // snapshot. An entry whose relative path does not form a library URI
    // becomes an Error-classified item labeled with that path and without a
    // blocked prefix: no manifest entry can live under a name that cannot form
    // a URI, so those rows stay eligible for Missing classification. A file of
    // an unsupported format needs no URI and is ignored, as scanEntry()
    // ignores it. An entry that resolves or inspects unsafely becomes an
    // Error-classified item whose URI prefix is blocked from Missing
    // classification. Either way the result reports false so the walk does
    // not descend into the entry; a true result allows descent.
    Result<bool> classifyWalkEntry(std::filesystem::directory_entry const& entry,
                                   std::filesystem::path const& root,
                                   std::filesystem::path const& resolvedRoot,
                                   ManifestSnapshot const& manifest,
                                   std::unordered_set<std::string>& seenUris,
                                   std::unordered_set<std::string>& blockedUriPrefixes,
                                   std::vector<ScanItem>& items)
    {
      auto const& entryPath = entry.path();
      auto const relativePath = utility::pathToGenericUtf8(entryPath.lexically_relative(root));
      auto uriRes = library::LibraryUri::parse(relativePath);

      if (!uriRes)
      {
        // Only a file of an unsupported format is ignored; a directory, a
        // symlink to one, and an entry whose type cannot be read are reported.
        // The entry caches the type the walk read where the platform reports
        // it, so a real directory is still recognized when its path is too
        // long to inspect again.
        auto typeEc = std::error_code{};
        auto const isSymlink = entry.is_symlink(typeEc);
        auto const isDirectory = !typeEc && entry.is_directory(typeEc);

        if (!typeEc && !isDirectory && !media::file::File::isSupported(entryPath))
        {
          return false;
        }

        // The walk does not follow directory symlinks, so only a real
        // directory has a subtree that this error skips.
        auto const skipsSubtree = isDirectory && !isSymlink;
        auto item = ScanItem{
          .uri = relativePath,
          .fullPath = entryPath,
          .classification = ScanClassification::Error,
          .errorMessage = skipsSubtree ? std::format("{}; its contents were not scanned", uriRes.error().message)
                                       : uriRes.error().message};
        items.push_back(std::move(item));
        return false;
      }

      auto resolvedPathRes = uriRes->resolveUnder(root);

      if (!resolvedPathRes)
      {
        blockedUriPrefixes.insert(std::string{uriRes->value()});
        auto item = ScanItem{.uri = std::string{uriRes->value()},
                             .fullPath = entryPath,
                             .classification = ScanClassification::Error,
                             .errorMessage = resolvedPathRes.error().message};
        items.push_back(std::move(item));
        return false;
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
        return false;
      }

      // Proactively check if this is a directory we can't enter
      if (auto entryEc = std::error_code{}; std::filesystem::is_directory(*resolvedPathRes, entryEc))
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
          return false;
        }
      }

      if (auto res = scanEntry(*resolvedPathRes, std::string{canonicalUriRes->value()}, manifest, seenUris, items);
          !res)
      {
        return std::unexpected{res.error()};
      }

      return true;
    }

    // Advances the walk to the next entry. The entry path is taken by value
    // because an increment replaces the visited entry. A failed increment
    // leaves the recursive iterator at its end value, so the walk can neither
    // continue nor skip the failed subtree and the error is reported to the
    // caller.
    Result<> advanceWalk(std::filesystem::recursive_directory_iterator& it, std::filesystem::path entryPath)
    {
      auto ec = std::error_code{};
      it.increment(ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError,
          "Failed to continue filesystem walk at " + utility::pathToUtf8(entryPath) + ": " + ec.message());
      }

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
                               .optMtime = entry.optMtime,
                               .audioPayloadLength = entry.audioPayloadLength,
                               .audioSignature = entry.audioSignature,
                               .trackId = entry.trackId,
                               .optManifestEvidence = ScanManifestEvidence{
                                 .fileSize = entry.fileSize,
                                 .optMtime = entry.optMtime,
                                 .audioPayloadLength = entry.audioPayloadLength,
                                 .audioSignature = entry.audioSignature,
                                 .status = entry.status,
                                 .totalFrames = entry.totalFrames,
                                 .sampleRate = entry.sampleRate,
                                 .members = entry.members,
                               }};
          items.push_back(std::move(item));
        }
      }
    }

    bool tryValidateMovedCarrier(ScanItem& destination, ScanItem const& source)
    {
      if (!source.optManifestEvidence || source.optManifestEvidence->members.empty())
      {
        return true;
      }

      auto mediaRes = readMediaTrack(destination.fullPath);

      if (!mediaRes)
      {
        destination.classification = ScanClassification::Error;
        destination.errorMessage = mediaRes.error().message;
        return false;
      }

      destination.totalFrames = mediaRes->optTotalFrames().value_or(0);
      destination.sampleRate = mediaRes->builder().property().sampleRate().raw();
      destination.hasExactMediaFacts = mediaRes->optTotalFrames().has_value();

      if (auto const& expected = *source.optManifestEvidence; !destination.hasExactMediaFacts ||
                                                              destination.totalFrames != expected.totalFrames ||
                                                              destination.sampleRate != expected.sampleRate)
      {
        destination.classification = ScanClassification::Error;
        destination.errorMessage = "Moved carrier extent or sample rate differs from imported segments";
        destination.cueDiagnostic = ScanCueDiagnostic::CarrierFactsMismatch;
        return false;
      }

      return true;
    }

    // Returns the paired Missing sources it removed from the plan.
    std::vector<ScanItem> classifyMovedEntries(std::vector<ScanItem>& items, std::stop_token const stopToken)
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
        return {};
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

        if (!tryValidateMovedCarrier(newItem, missingItem))
        {
          continue;
        }

        newItem.classification = ScanClassification::Moved;
        newItem.oldUri = missingItem.uri;
        newItem.trackId = missingItem.trackId;
        newItem.optManifestEvidence = missingItem.optManifestEvidence;
        matchedMissingIndices.insert(missingIndices.front());
      }

      if (matchedMissingIndices.empty())
      {
        return {};
      }

      auto filteredItems = std::vector<ScanItem>{};
      filteredItems.reserve(items.size() - matchedMissingIndices.size());
      auto sources = std::vector<ScanItem>{};
      sources.reserve(matchedMissingIndices.size());

      for (std::size_t index = 0; index < items.size(); ++index)
      {
        async::throwIfStopRequested(stopToken);

        if (matchedMissingIndices.contains(index))
        {
          sources.push_back(std::move(items[index]));
        }
        else
        {
          filteredItems.push_back(std::move(items[index]));
        }
      }

      items = std::move(filteredItems);
      return sources;
    }

    // A move refused after pairing applies nothing, so its source is still
    // absent. Restoring that Missing item lets apply mark the row and a later
    // scan pair the files again; the destination keeps only its own failure.
    void restoreRefusedMoveSources(std::vector<ScanItem>& items, std::vector<ScanItem> sources)
    {
      auto refusedSourceUris = std::unordered_set<std::string>{};

      for (auto& item : items)
      {
        if (item.oldUri.empty() || item.classification == ScanClassification::Moved)
        {
          continue;
        }

        refusedSourceUris.insert(std::exchange(item.oldUri, {}));
        item.trackId = kInvalidTrackId;
        item.optManifestEvidence.reset();
      }

      for (auto& source : sources)
      {
        if (refusedSourceUris.contains(source.uri))
        {
          items.push_back(std::move(source));
        }
      }
    }

    bool needsCueBinding(ScanItem const& item) noexcept
    {
      return item.classification != ScanClassification::Missing && item.classification != ScanClassification::Error &&
             item.classification != ScanClassification::Notice;
    }

    // Ordered so reports and their notification excerpt do not depend on hashing.
    using CueDirectorySheets = std::map<std::filesystem::path, Result<std::vector<CueSheetSnapshot>>>;

    // Keys are the resolved parents that bindPlanItem looks up. An item path
    // is its symlink target, whose directory the walk may never have listed,
    // so the set comes from the planned items rather than the walk. A
    // directory that cannot be read affects only its own items.
    CueDirectorySheets readPlanCueDirectories(std::filesystem::path const& root,
                                              std::span<ScanItem const> items,
                                              std::set<std::filesystem::path> directories,
                                              std::stop_token stopToken)
    {
      for (auto const& item : items)
      {
        if (needsCueBinding(item))
        {
          directories.insert(item.fullPath.parent_path());
        }
      }

      auto sheetsByDirectory = CueDirectorySheets{};

      for (auto const& directory : directories)
      {
        async::throwIfStopRequested(stopToken);
        sheetsByDirectory.emplace(directory, readCueDirectory(root, directory));
      }

      return sheetsByDirectory;
    }

    void bindPlanItem(ScanItem& item, std::span<CueSheetSnapshot const> sheets)
    {
      auto const audioName = utility::pathToGenericUtf8(utility::pathFromUtf8(item.uri).filename());
      auto const candidates = cueEvidenceFor(audioName, sheets);
      auto const& optStored = item.optManifestEvidence;
      auto const isCarrier = optStored && !optStored->members.empty();

      if (!isCarrier && item.classification == ScanClassification::Unchanged && candidates.general.empty() &&
          candidates.extensionSpecific.empty())
      {
        // Admission already recorded this whole file's embedded result. Matching
        // file facts preserve that empty evidence without reopening the file.
        item.cueEvidence = candidates;
        return;
      }

      // Member codecs can be absent or disagree after a full restore. They
      // are metadata hints, not authority for physical binding capability.
      auto mediaRes = readMediaTrack(item.fullPath);

      if (!mediaRes)
      {
        if (!isCarrier && candidates.general.empty() && candidates.extensionSpecific.empty())
        {
          // Whole-file planning historically needs file facts only; media
          // parse failures remain preparation failures when no sheet applies.
          item.cueEvidence = candidates;
          return;
        }

        item.classification = ScanClassification::Error;
        item.errorMessage = mediaRes.error().message;
        return;
      }

      auto binding = bindCueSheet(audioName, *mediaRes, sheets);
      item.totalFrames = mediaRes->optTotalFrames().value_or(0);
      item.sampleRate = mediaRes->builder().property().sampleRate().raw();
      item.hasExactMediaFacts = mediaRes->optTotalFrames().has_value();

      item.cueEvidence = std::move(binding.evidence);
      item.cueLayout = std::move(binding.layout);
      item.cueDiagnostic = binding.cueDiagnostic;
      item.cueDetail = std::move(binding.cueDetail);

      item.errorMessage = std::move(binding.errorMessage);

      if (item.classification == ScanClassification::New && binding.state == CueBindingState::Undetermined)
      {
        item.classification = ScanClassification::Error;
      }
      else
      {
        classifyStoredCueBinding(item);
      }
    }

    bool hasMatchingAudio(std::filesystem::path const& directory,
                          std::string_view sheetName,
                          std::span<ScanItem const> items)
    {
      return std::ranges::any_of(items,
                                 [&directory, sheetName](ScanItem const& item)
                                 {
                                   return item.fullPath.parent_path() == directory &&
                                          media::file::File::isSupported(item.fullPath) &&
                                          media::cue::classifySheetCandidate(
                                            utility::pathToGenericUtf8(utility::pathFromUtf8(item.uri).filename()),
                                            sheetName) != media::cue::SheetCandidate::None;
                                 });
    }

    bool isImageSheet(CueSheetSnapshot const& sheet)
    {
      if (!sheet.optSheet)
      {
        return false;
      }

      return std::ranges::count(sheet.optSheet->tracks, media::cue::TrackType::Audio, &media::cue::Track::type) >= 2;
    }

    void appendUnmatchedSheetReports(std::vector<ScanItem>& items,
                                     CueDirectorySheets const& sheetsByDirectory,
                                     std::filesystem::path const& resolvedRoot,
                                     std::stop_token stopToken)
    {
      auto reports = std::vector<ScanItem>{};

      for (auto const& [directory, sheetsRes] : sheetsByDirectory)
      {
        if (!sheetsRes)
        {
          continue;
        }

        for (auto const& sheet : *sheetsRes)
        {
          async::throwIfStopRequested(stopToken);
          auto const hasMatch = hasMatchingAudio(directory, sheet.evidence.name, items);

          if (!hasMatch && isImageSheet(sheet))
          {
            auto const path = directory / utility::pathFromUtf8(sheet.evidence.name);
            reports.push_back(ScanItem{.uri = utility::pathToGenericUtf8(path.lexically_relative(resolvedRoot)),
                                       .fullPath = path,
                                       .classification = ScanClassification::Notice,
                                       .errorMessage = "No supported audio file has this CUE sheet's name",
                                       .cueDiagnostic = ScanCueDiagnostic::NoMatchingAudio});
          }
        }
      }

      items.append_range(std::move(reports));
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
    // Directories whose sheets may match no planned item. Item directories
    // are added from their resolved paths after the walk.
    auto sheetDirectories = std::set<std::filesystem::path>{};

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
      auto const& entry = *it;

      if (isCueSheet(entry.path()))
      {
        // A directory that cannot be resolved loses only its unmatched-sheet
        // reports; its media items resolve their own parents.
        auto directoryEc = std::error_code{};

        if (auto directory = std::filesystem::weakly_canonical(entry.path().parent_path(), directoryEc); !directoryEc)
        {
          sheetDirectories.insert(std::move(directory));
        }
      }

      if (progress)
      {
        progress(entry.path());
        async::throwIfStopRequested(stopToken);
      }

      // The returned bool is the descent permission: false means the entry
      // was classified as unsafe, so the walk must not descend into it.
      auto const canDescendRes =
        classifyWalkEntry(entry, root, resolvedRoot, snapshot.manifest, seenUris, blockedUriPrefixes, items);

      if (!canDescendRes)
      {
        return std::unexpected{canDescendRes.error()};
      }

      if (!*canDescendRes)
      {
        it.disable_recursion_pending();
      }

      if (auto const advanceRes = advanceWalk(it, entry.path()); !advanceRes)
      {
        return std::unexpected{advanceRes.error()};
      }
    }

    // 2. Identify MISSING (In manifest but not on disk)
    addMissingEntries(items, snapshot.manifest, seenUris, blockedUriPrefixes, stopToken);
    auto pairedSources = classifyMovedEntries(items, stopToken);

    auto const sheetsByDirectory = readPlanCueDirectories(resolvedRoot, items, std::move(sheetDirectories), stopToken);

    for (auto& item : items)
    {
      async::throwIfStopRequested(stopToken);

      if (!needsCueBinding(item))
      {
        continue;
      }

      auto const found = sheetsByDirectory.find(item.fullPath.parent_path());
      AO_INVARIANT(found != sheetsByDirectory.end(), "Planned media directory has no CUE snapshot");

      if (auto const& sheetsRes = found->second; !sheetsRes)
      {
        // Unknown sheet state must neither import the file whole nor rebind it.
        // An unchanged item applies nothing, so for it the state is only reported.
        if (item.classification == ScanClassification::Unchanged)
        {
          item.cueDiagnostic = ScanCueDiagnostic::SheetUnreadableOrMalformed;
        }
        else
        {
          item.classification = ScanClassification::Error;
        }

        item.errorMessage = sheetsRes.error().message;
        continue;
      }

      bindPlanItem(item, *found->second);
    }

    restoreRefusedMoveSources(items, std::move(pairedSources));
    appendUnmatchedSheetReports(items, sheetsByDirectory, resolvedRoot, stopToken);
    return ScanPlan{snapshot.libraryId, snapshot.libraryRevision, std::move(items)};
  }
} // namespace ao::rt
