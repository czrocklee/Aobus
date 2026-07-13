// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibCommand.h"

#include "CliRuntime.h"
#include "CommandError.h"
#include "DryRunFlag.h"
#include "DumpOutput.h"
#include "Output.h"
#include "ScanOutput.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/media/file/File.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Uuid.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::cli
{
  struct LibraryMetadataDto final
  {
    std::string libraryId{};
    std::uint64_t libraryVersion = 0;
    std::string flags{};
    std::string createdTime{};
  };

  struct LibraryTransferDto final
  {
    std::string action{};
    std::string path{};
    std::string mode{};
  };

  struct ImportReportDto final
  {
    std::string action{};
    std::string path{};
    std::string mode{};
    bool dryRun = false;
    std::uint64_t tracksCreated = 0;
    std::uint64_t tracksUpdated = 0;
    std::uint64_t tracksDeleted = 0;
    std::uint64_t listsCreated = 0;
    std::uint64_t listsDeleted = 0;
  };

  struct LibraryStats final
  {
    std::size_t tracks = 0;
    std::size_t lists = 0;
    std::size_t resources = 0;
    std::size_t resourceBytes = 0;
    std::size_t manifest = 0;
    std::size_t dictionary = 0;
    std::size_t tags = 0;
    std::uint64_t diskBytes = 0;
  };

  namespace
  {
    std::string formatTimestamp(std::chrono::sys_time<std::chrono::milliseconds> timestamp)
    {
      auto const tp = std::chrono::system_clock::time_point{timestamp.time_since_epoch()};
      return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
    }

    LibraryMetadataDto toLibraryMetadataDto(library::MusicLibrary& ml)
    {
      auto const header = ml.metadataHeader();
      return LibraryMetadataDto{.libraryId = utility::formatUuid(header.libraryId),
                                .libraryVersion = header.libraryVersion,
                                .flags = std::format("0x{:x}", header.flags),
                                .createdTime = formatTimestamp(header.createdTime)};
    }

    void printMetadataPlain(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const header = ml.metadataHeader();

      std::println(os, "Library ID:    {}", utility::formatUuid(header.libraryId));
      std::println(os, "Library Version:  {}", header.libraryVersion);
      std::println(os, "Flags:       0x{:x}", header.flags);
      std::println(os, "Created:      {}", formatTimestamp(header.createdTime));
    }

    void printMetadata(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      if (format == OutputFormat::Plain)
      {
        printMetadataPlain(ml, os);
        return;
      }

      emitDocument(os, format, toLibraryMetadataDto(ml));
    }

    void printLibraryTransfer(std::ostream& os,
                              OutputFormat format,
                              std::string_view action,
                              std::string const& path,
                              std::string const& modeStr)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os, format, LibraryTransferDto{.action = std::string{action}, .path = path, .mode = modeStr});
        return;
      }

      if (action == "export")
      {
        std::println(os, "Library exported to '{}' using mode '{}'.", path, modeStr);
      }
      else
      {
        std::println(os, "Library imported from '{}' using mode '{}'.", path, modeStr);
      }
    }

    void printLibraryImport(std::ostream& os,
                            OutputFormat format,
                            std::string const& path,
                            std::string const& modeStr,
                            bool dryRun,
                            rt::ImportReport const& report)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os,
                     format,
                     ImportReportDto{.action = "import",
                                     .path = path,
                                     .mode = modeStr,
                                     .dryRun = dryRun,
                                     .tracksCreated = report.tracksCreated,
                                     .tracksUpdated = report.tracksUpdated,
                                     .tracksDeleted = report.tracksDeleted,
                                     .listsCreated = report.listsCreated,
                                     .listsDeleted = report.listsDeleted});
        return;
      }

      std::println(os, "Library imported from '{}' using mode '{}'.{}", path, modeStr, dryRun ? " (dry-run)" : "");
    }

    void exportLib(library::MusicLibrary& ml,
                   std::string const& path,
                   std::string const& modeStr,
                   OutputFormat format,
                   std::ostream& os)
    {
      auto mode = rt::ExportMode::Full;

      if (modeStr == "delta")
      {
        mode = rt::ExportMode::Delta;
      }
      else if (modeStr == "metadata")
      {
        mode = rt::ExportMode::Metadata;
      }
      else if (modeStr == "full")
      {
        mode = rt::ExportMode::Full;
      }
      else if (modeStr == "listOnly")
      {
        mode = rt::ExportMode::ListOnly;
      }
      else
      {
        throwCommandError(Error::Code::InvalidInput,
                          "invalid export mode '{}'. Valid modes are: delta, metadata, full, listOnly.",
                          modeStr);
      }

      auto exporter = rt::LibraryYamlExporter{ml};

      if (auto const result = exporter.exportToYaml(path, mode); !result)
      {
        auto const& error = result.error();
        throwCommandError(error, "export failed: {}", error.message);
      }

      printLibraryTransfer(os, format, "export", path, modeStr);
    }

    void importLib(library::MusicLibrary& ml,
                   std::string const& path,
                   std::string const& modeStr,
                   bool dryRun,
                   OutputFormat format,
                   std::ostream& os)
    {
      auto mode = rt::ImportMode::Restore;

      if (modeStr == "restore")
      {
        mode = rt::ImportMode::Restore;
      }
      else if (modeStr == "merge")
      {
        mode = rt::ImportMode::Merge;
      }
      else
      {
        throwCommandError(
          Error::Code::InvalidInput, "invalid import mode '{}'. Valid modes are: restore, merge.", modeStr);
      }

      auto importer = rt::LibraryYamlImporter{ml};

      if (dryRun)
      {
        auto const result = importer.previewImportFromYaml(path, mode);

        if (!result)
        {
          auto const& error = result.error();
          throwCommandError(error, "import failed: {}", error.message);
        }

        printLibraryImport(os, format, path, modeStr, true, *result);
        return;
      }

      auto const result = importer.importFromYaml(path, mode);

      if (!result)
      {
        auto const& error = result.error();
        throwCommandError(error, "import failed: {}", error.message);
      }

      printLibraryImport(os, format, path, modeStr, false, *result);
    }

    std::string_view formatFileStatus(library::FileStatus status)
    {
      switch (status)
      {
        case library::FileStatus::Available: return "Available";
        case library::FileStatus::Missing: return "Missing";
        case library::FileStatus::Error: return "Error";
        default: return "Unknown";
      }
    }

    std::uint64_t directorySize(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      std::uint64_t total = 0;
      auto it = std::filesystem::recursive_directory_iterator{
        path, std::filesystem::directory_options::skip_permission_denied, ec};

      if (ec)
      {
        throwCommandError(Error::Code::IoError, "failed to inspect library size: {}", ec.message());
      }

      for (; it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
      {
        if (ec)
        {
          ec.clear();
          continue;
        }

        if (!it->is_regular_file(ec) || ec)
        {
          ec.clear();
          continue;
        }

        if (auto const size = it->file_size(ec); !ec)
        {
          total += size;
        }

        ec.clear();
      }

      return total;
    }

    LibraryStats collectStats(library::MusicLibrary& ml, std::filesystem::path const& databasePath)
    {
      auto stats = LibraryStats{};
      {
        auto const transaction = ml.readTransaction();

        for ([[maybe_unused]] auto const& entry : ml.tracks().reader(transaction))
        {
          ++stats.tracks;
        }

        for ([[maybe_unused]] auto const& entry : ml.lists().reader(transaction))
        {
          ++stats.lists;
        }

        for (auto const& [_, bytes] : ml.resources().reader(transaction))
        {
          ++stats.resources;
          stats.resourceBytes += bytes.size();
        }

        for ([[maybe_unused]] auto const& entry : ml.manifest().reader(transaction))
        {
          ++stats.manifest;
        }

        auto tagIds = std::unordered_set<std::uint32_t>{};
        auto const trackReader = ml.tracks().reader(transaction);

        for (auto const& [_, view] : trackReader.hot())
        {
          for (auto const tagId : view.tags())
          {
            if (tagId != kInvalidDictionaryId)
            {
              tagIds.insert(tagId.raw());
            }
          }
        }

        stats.dictionary = ml.dictionary().size();
        stats.tags = tagIds.size();
      }

      stats.diskBytes = directorySize(databasePath);
      return stats;
    }

    void printStats(library::MusicLibrary& ml,
                    std::filesystem::path const& databasePath,
                    OutputFormat format,
                    std::ostream& os)
    {
      auto const stats = collectStats(ml, databasePath);

      if (format != OutputFormat::Plain)
      {
        emitDocument(os, format, stats);
        return;
      }

      std::println(os, "tracks: {}", stats.tracks);
      std::println(os, "lists: {}", stats.lists);
      std::println(os, "resources: {}", stats.resources);
      std::println(os, "resourceBytes: {}", stats.resourceBytes);
      std::println(os, "manifest: {}", stats.manifest);
      std::println(os, "dictionary: {}", stats.dictionary);
      std::println(os, "tags: {}", stats.tags);
      std::println(os, "diskBytes: {}", stats.diskBytes);
    }

    bool isVerifyIssue(rt::ScanClassification classification)
    {
      return classification == rt::ScanClassification::Changed || classification == rt::ScanClassification::Moved ||
             classification == rt::ScanClassification::Missing || classification == rt::ScanClassification::Error;
    }

    bool isVerifyFailure(rt::ScanClassification classification)
    {
      return classification == rt::ScanClassification::Missing || classification == rt::ScanClassification::Error;
    }
  } // namespace

  struct VerifyIssueDto final
  {
    std::string type{};
    std::string uri{};
    std::optional<std::string> optMessage{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::VerifyIssueDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optMessage")
    {
      return "message";
    }

    return memberName;
  }
};

namespace ao::cli
{
  struct VerifyReportDto final
  {
    bool ok = false;
    std::vector<VerifyIssueDto> issues{};
  };

  struct RelinkCandidateDto final
  {
    std::string oldUri{};
    std::string newUri{};
    TrackId trackId{};
    std::uint64_t audioPayloadLength = 0;
  };

  struct RelinkListDto final
  {
    std::vector<std::string> missing{};
    std::vector<std::string> newFiles{};
    std::vector<RelinkCandidateDto> candidates{};
  };

  struct RelinkApplyDto final
  {
    bool dryRun = false;
    std::string oldUri{};
    std::string newUri{};
    TrackId trackId{};
  };

  struct FingerprintReportDto final
  {
    std::int32_t completed = 0;
    std::int32_t skipped = 0;
    std::int32_t failures = 0;
    bool cancelled = false;
  };

  struct ResourceRecordDto final
  {
    ResourceId id{};
    std::uint64_t size = 0;
  };

  struct ResourceListDto final
  {
    std::vector<ResourceRecordDto> resources{};
  };

  struct ResourceExportDto final
  {
    ResourceId id{};
    std::string output{};
    std::uint64_t size = 0;
  };

  struct ManifestRecordDto final
  {
    std::string uri{};
    TrackId trackId{};
    std::uint64_t fileSize = 0;
    std::uint64_t mtime = 0;
    std::string status{};
  };

  namespace
  {
    VerifyIssueDto toVerifyIssueDto(rt::ScanItem const& item)
    {
      return VerifyIssueDto{.type = std::string{scanClassificationName(item.classification)},
                            .uri = item.uri,
                            .optMessage = item.errorMessage.empty() ? std::nullopt : std::optional{item.errorMessage}};
    }

    void printVerifyIssues(std::vector<rt::ScanItem> const& issues, bool failed, OutputFormat format, std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        auto report = VerifyReportDto{.ok = !failed};
        report.issues.reserve(issues.size());

        for (auto const& item : issues)
        {
          report.issues.push_back(toVerifyIssueDto(item));
        }

        emitDocument(os, format, report);
        return;
      }

      if (issues.empty())
      {
        std::println(os, "ok");
        return;
      }

      for (auto const& item : issues)
      {
        if (item.errorMessage.empty())
        {
          std::println(os, "{} {}", scanClassificationName(item.classification), item.uri);
        }
        else
        {
          std::println(os, "{} {} {}", scanClassificationName(item.classification), item.uri, item.errorMessage);
        }
      }
    }

    void verifyLibrary(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      auto scanService = rt::LibraryScan{ml};
      auto planResult = scanService.buildPlan();

      if (!planResult)
      {
        auto const& error = planResult.error();
        throwCommandError(error, "verify failed: {}", error.message);
      }

      auto issues = std::vector<rt::ScanItem>{};
      bool failed = false;

      for (auto const& item : planResult->items)
      {
        if (!isVerifyIssue(item.classification))
        {
          continue;
        }

        failed = failed || isVerifyFailure(item.classification);
        issues.push_back(item);
      }

      printVerifyIssues(issues, failed, format, os);

      if (failed)
      {
        throwCommandError(Error::Code::CorruptData, "library verification failed");
      }
    }

    struct RelinkIdentity final
    {
      std::uint64_t payloadLength = 0;
      utility::Hash128 signature = {};
    };

    RelinkIdentity relinkIdentityFromItem(rt::ScanItem const& item)
    {
      return RelinkIdentity{.payloadLength = item.audioPayloadLength, .signature = item.audioSignature};
    }

    RelinkIdentity readRelinkAudioIdentity(std::filesystem::path const& path)
    {
      auto fileResult = media::file::File::open(path);

      if (!fileResult)
      {
        auto const& error = fileResult.error();
        throwCommandError(error, "failed to open relink candidate: {}", error.message);
      }

      auto payloadResult = fileResult->audioPayload();

      if (!payloadResult)
      {
        auto const& error = payloadResult.error();
        throwCommandError(error, "failed to read relink candidate payload: {}", error.message);
      }

      auto optIdentity = library::readAudioIdentity(payloadResult->bytes);

      if (!optIdentity)
      {
        throwCommandError(
          Error::Code::InvalidState, "fingerprinting relink candidate was cancelled: {}", path.string());
      }

      return RelinkIdentity{.payloadLength = optIdentity->payloadLength, .signature = optIdentity->signature};
    }

    bool hasSameRelinkIdentity(RelinkIdentity const& left, RelinkIdentity const& right)
    {
      return left.payloadLength == right.payloadLength && left.signature == right.signature;
    }

    std::string normalizeRelinkUri(library::MusicLibrary& ml, std::string const& input)
    {
      auto path = std::filesystem::path{input};

      if (path.is_absolute())
      {
        auto ec = std::error_code{};
        path = std::filesystem::relative(path, ml.rootPath(), ec);

        if (ec)
        {
          throwCommandError(Error::Code::InvalidInput, "failed to make path root-relative: {}", ec.message());
        }
      }

      path = path.lexically_normal();
      auto const uri = path.generic_string();

      if (uri.empty() || uri == "." || path.is_absolute())
      {
        throwCommandError(Error::Code::InvalidInput, "invalid library URI '{}'", input);
      }

      for (auto const& part : path)
      {
        if (part == "..")
        {
          throwCommandError(Error::Code::InvalidInput, "path is outside the music root: {}", input);
        }
      }

      return uri;
    }

    std::vector<RelinkCandidateDto> relinkCandidates(rt::ScanPlan const& plan)
    {
      auto missingIndices = std::vector<std::size_t>{};
      auto newIdentities = std::vector<std::pair<std::size_t, RelinkIdentity>>{};

      for (std::size_t index = 0; index < plan.items.size(); ++index)
      {
        if (auto const& item = plan.items[index];
            item.classification == rt::ScanClassification::Missing && rt::hasAudioIdentity(item))
        {
          missingIndices.push_back(index);
        }
      }

      for (std::size_t index = 0; index < plan.items.size(); ++index)
      {
        auto const& item = plan.items[index];

        if (item.classification != rt::ScanClassification::New)
        {
          continue;
        }

        if (rt::hasAudioIdentity(item))
        {
          newIdentities.emplace_back(index, relinkIdentityFromItem(item));
        }
      }

      auto candidates = std::vector<RelinkCandidateDto>{};

      for (auto const missingIndex : missingIndices)
      {
        auto const& missingItem = plan.items[missingIndex];
        auto const missingIdentity = relinkIdentityFromItem(missingItem);

        for (auto const& [newIndex, newIdentity] : newIdentities)
        {
          if (!hasSameRelinkIdentity(missingIdentity, newIdentity))
          {
            continue;
          }

          auto const& newItem = plan.items[newIndex];
          candidates.push_back(RelinkCandidateDto{.oldUri = missingItem.uri,
                                                  .newUri = newItem.uri,
                                                  .trackId = missingItem.trackId,
                                                  .audioPayloadLength = missingIdentity.payloadLength});
        }
      }

      return candidates;
    }

    RelinkListDto toRelinkListDto(rt::ScanPlan const& plan)
    {
      auto dto = RelinkListDto{.candidates = relinkCandidates(plan)};

      for (auto const& item : plan.items)
      {
        if (item.classification == rt::ScanClassification::Missing)
        {
          dto.missing.push_back(item.uri);
        }
        else if (item.classification == rt::ScanClassification::New)
        {
          dto.newFiles.push_back(item.uri);
        }
      }

      return dto;
    }

    void printRelinkCandidates(rt::ScanPlan const& plan, OutputFormat format, std::ostream& os)
    {
      auto const dto = toRelinkListDto(plan);

      if (format != OutputFormat::Plain)
      {
        emitDocument(os, format, dto);
        return;
      }

      if (dto.missing.empty() && dto.newFiles.empty())
      {
        std::println(os, "No unresolved relink candidates");
        return;
      }

      for (auto const& uri : dto.missing)
      {
        std::println(os, "missing {}", uri);
      }

      for (auto const& uri : dto.newFiles)
      {
        std::println(os, "new {}", uri);
      }

      for (auto const& candidate : dto.candidates)
      {
        std::println(os, "candidate {} -> {}", candidate.oldUri, candidate.newUri);
      }
    }

    rt::ScanItem const* findPlanItem(rt::ScanPlan const& plan,
                                     rt::ScanClassification classification,
                                     std::string_view uri)
    {
      for (auto const& item : plan.items)
      {
        if (item.classification == classification && item.uri == uri)
        {
          return &item;
        }
      }

      return nullptr;
    }

    RelinkCandidateDto validateRelink(rt::ScanPlan const& plan, std::string const& oldUri, std::string const& newUri)
    {
      auto const* const missingItem = findPlanItem(plan, rt::ScanClassification::Missing, oldUri);

      if (missingItem == nullptr)
      {
        throwCommandError(Error::Code::InvalidInput, "missing manifest row is not unresolved: {}", oldUri);
      }

      if (!rt::hasAudioIdentity(*missingItem))
      {
        throwCommandError(Error::Code::InvalidInput,
                          "missing manifest row has no audio signature: {}; run `aobus lib fingerprint --pending` "
                          "before relinking",
                          oldUri);
      }

      auto const* const newItem = findPlanItem(plan, rt::ScanClassification::New, newUri);

      if (newItem == nullptr)
      {
        throwCommandError(Error::Code::InvalidInput, "new file is not unresolved: {}", newUri);
      }

      auto const missingIdentity = relinkIdentityFromItem(*missingItem);
      auto const newIdentity = readRelinkAudioIdentity(newItem->fullPath);

      if (!hasSameRelinkIdentity(missingIdentity, newIdentity))
      {
        throwCommandError(Error::Code::InvalidInput, "audio identity mismatch: {} -> {}", oldUri, newUri);
      }

      return RelinkCandidateDto{.oldUri = oldUri,
                                .newUri = newUri,
                                .trackId = missingItem->trackId,
                                .audioPayloadLength = missingIdentity.payloadLength};
    }

    void printRelinkApply(RelinkCandidateDto const& candidate, bool dryRun, OutputFormat format, std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(
          os,
          format,
          RelinkApplyDto{
            .dryRun = dryRun, .oldUri = candidate.oldUri, .newUri = candidate.newUri, .trackId = candidate.trackId});
        return;
      }

      std::println(os, "relinked {} -> {}{}", candidate.oldUri, candidate.newUri, dryRun ? " (dry-run)" : "");
    }

    rt::ScanItem makeMovedRelinkItem(rt::ScanPlan const& plan, RelinkCandidateDto const& candidate)
    {
      auto const* const newItem = findPlanItem(plan, rt::ScanClassification::New, candidate.newUri);

      if (newItem == nullptr)
      {
        throwCommandError(Error::Code::InvalidInput, "new file is not unresolved: {}", candidate.newUri);
      }

      auto item = *newItem;
      item.classification = rt::ScanClassification::Moved;
      item.oldUri = candidate.oldUri;
      item.trackId = candidate.trackId;
      return item;
    }

    void applyRelink(library::MusicLibrary& ml,
                     rt::ScanPlan const& plan,
                     RelinkCandidateDto const& candidate,
                     bool dryRun,
                     OutputFormat format,
                     std::ostream& os)
    {
      if (dryRun)
      {
        printRelinkApply(candidate, true, format, os);
        return;
      }

      auto relinkPlan = rt::ScanPlan{};
      relinkPlan.items.push_back(makeMovedRelinkItem(plan, candidate));

      auto failures = std::vector<std::string>{};
      auto scanService = rt::LibraryScan{ml};
      auto result = scanService.applyPlan(
        std::move(relinkPlan),
        // Default (eager) options on purpose: Moved items are fingerprinted
        // during apply regardless of policy, so a relink must never leave the
        // rebound row with a pending identity.
        rt::ScanApplyOptions{},
        rt::LibraryScan::ApplyProgressCallback{},
        [&failures](rt::ScanFailure const& failure)
        {
          failures.push_back(failure.uri.empty()
                               ? std::format("failed to {}: {}", failure.stage, failure.message)
                               : std::format("failed to {} {}: {}", failure.stage, failure.uri, failure.message));
        });

      if (!result)
      {
        auto const& error = result.error();
        throwCommandError(error, "relink failed: {}", error.message);
      }

      if (result->failureCount != 0 || result->relinkedCount != 1)
      {
        auto const message = failures.empty() ? "relink did not update the library" : failures.front();
        throwCommandError(Error::Code::IoError, "relink failed: {}", message);
      }

      printRelinkApply(candidate, false, format, os);
    }

    void relinkLibrary(library::MusicLibrary& ml,
                       std::optional<std::string> const& optOldUri,
                       std::optional<std::string> const& optNewUri,
                       bool dryRun,
                       OutputFormat format,
                       std::ostream& os)
    {
      auto scanService = rt::LibraryScan{ml};
      auto planResult = scanService.buildPlan();

      if (!planResult)
      {
        auto const& error = planResult.error();
        throwCommandError(error, "relink scan failed: {}", error.message);
      }

      auto const& plan = *planResult;

      if (!optOldUri && !optNewUri)
      {
        printRelinkCandidates(plan, format, os);
        return;
      }

      if (!optOldUri || !optNewUri)
      {
        throwCommandError(Error::Code::InvalidInput, "lib relink requires both --from and --to");
      }

      auto const oldUri = normalizeRelinkUri(ml, *optOldUri);
      auto const newUri = normalizeRelinkUri(ml, *optNewUri);
      auto const candidate = validateRelink(plan, oldUri, newUri);
      applyRelink(ml, plan, candidate, dryRun, format, os);
    }

    void printBackfillFailure(rt::AudioIdentityIndexFailure const& failure, std::ostream& err)
    {
      if (failure.uri.empty())
      {
        std::println(err, "failed to {}: {}", failure.stage, failure.message);
        return;
      }

      std::println(err, "failed to {} {}: {}", failure.stage, failure.uri, failure.message);
    }

    void printFingerprintReport(rt::AudioIdentityIndexResult const& result, OutputFormat format, std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os,
                     format,
                     FingerprintReportDto{.completed = result.completedCount,
                                          .skipped = result.skippedCount,
                                          .failures = result.failureCount,
                                          .cancelled = result.cancelled});
        return;
      }

      std::println(
        os, "fingerprinted {}  skipped {}  failed {}", result.completedCount, result.skippedCount, result.failureCount);
    }

    void fingerprintPending(rt::CoreRuntime& runtime,
                            library::MusicLibrary& ml,
                            bool pending,
                            bool verbose,
                            OutputFormat format,
                            std::ostream& os,
                            std::ostream& err)
    {
      if (!pending)
      {
        throwCommandError(Error::Code::InvalidInput, "lib fingerprint requires --pending");
      }

      // indexPending fingerprints concurrently on the runtime worker pool; the
      // CLI blocks its own (non-pool) thread on the future. The indexer
      // serializes callbacks, so printing from them is safe.
      auto mutationMutex = std::mutex{};
      auto indexer = rt::AudioIdentityIndexer{runtime.async(), ml, mutationMutex};
      auto future = runtime.async().spawn(indexer.indexPending(
        {},
        verbose ? rt::AudioIdentityIndexer::ProgressCallback{[&err](rt::AudioIdentityIndexProgress const& progress)
                                                             {
                                                               if (progress.itemFraction == 0.0)
                                                               {
                                                                 std::println(err,
                                                                              "fingerprint: {}",
                                                                              progress.path.generic_string());
                                                               }
                                                             }}
                : nullptr,
        [&err](rt::AudioIdentityIndexFailure const& failure) { printBackfillFailure(failure, err); }));
      auto result = future.get();

      if (!result)
      {
        auto const& error = result.error();
        throwCommandError(error, "fingerprint failed: {}", error.message);
      }

      printFingerprintReport(*result, format, os);
    }

    std::vector<ResourceRecordDto> resourceRecords(library::MusicLibrary& ml)
    {
      auto const transaction = ml.readTransaction();
      auto const reader = ml.resources().reader(transaction);
      auto records = std::vector<ResourceRecordDto>{};

      for (auto const& [id, bytes] : reader)
      {
        records.push_back(ResourceRecordDto{.id = id, .size = static_cast<std::uint64_t>(bytes.size())});
      }

      return records;
    }

    void listResources(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os, format, ResourceListDto{.resources = resourceRecords(ml)});
        return;
      }

      auto const transaction = ml.readTransaction();
      auto const reader = ml.resources().reader(transaction);

      for (auto const& [id, bytes] : reader)
      {
        std::println(os, "{}  {}", id.raw(), bytes.size());
      }
    }

    void printResourceExport(ResourceId id,
                             std::filesystem::path const& path,
                             std::size_t size,
                             OutputFormat format,
                             std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(
          os, format, ResourceExportDto{.id = id, .output = path.string(), .size = static_cast<std::uint64_t>(size)});
        return;
      }

      std::println(os, "exported resource: {} {}", id, path.string());
    }

    void exportResource(library::MusicLibrary& ml,
                        ResourceId id,
                        std::filesystem::path const& path,
                        OutputFormat format,
                        std::ostream& os)
    {
      auto bytes = std::vector<std::byte>{};
      {
        auto const transaction = ml.readTransaction();
        auto const reader = ml.resources().reader(transaction);
        auto const optBytes = reader.get(id);

        if (!optBytes)
        {
          throwCommandError(Error::Code::NotFound, "resource not found: {}", id);
        }

        bytes.assign(optBytes->begin(), optBytes->end());
      }

      auto out = std::ofstream{path, std::ios::binary};

      if (!out)
      {
        throwCommandError(Error::Code::IoError, "failed to open resource output: {}", path.string());
      }

      auto const data = utility::bytes::stringView(bytes);
      out.write(data.data(), static_cast<std::streamsize>(data.size()));

      if (!out)
      {
        out.close();
        auto ec = std::error_code{};
        std::filesystem::remove(path, ec);
        throwCommandError(Error::Code::IoError, "failed to write resource output: {}", path.string());
      }

      printResourceExport(id, path, bytes.size(), format, os);
    }
  } // namespace

  struct LibraryDumpDto final
  {
    std::optional<LibraryMetadataDto> optMetadata{};
    std::optional<std::map<std::string, std::string>> optDictionary{};
    std::optional<std::vector<ManifestRecordDto>> optManifest{};
    std::optional<std::vector<ResourceRecordDto>> optResources{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::LibraryDumpDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optMetadata")
    {
      return "meta";
    }

    if (memberName == "optDictionary")
    {
      return "dictionary";
    }

    if (memberName == "optManifest")
    {
      return "manifest";
    }

    if (memberName == "optResources")
    {
      return "resources";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    std::map<std::string, std::string> dictionaryEntries(library::MusicLibrary& ml)
    {
      auto entries = std::map<std::string, std::string>{};
      auto const& dictionary = ml.dictionary();

      for (std::size_t i = 1; i <= dictionary.size(); ++i)
      {
        entries.emplace(std::to_string(i), dictionary.get(DictionaryId{static_cast<std::uint32_t>(i)}));
      }

      return entries;
    }

    std::vector<ManifestRecordDto> manifestRecords(library::MusicLibrary& ml)
    {
      auto records = std::vector<ManifestRecordDto>{};
      auto const transaction = ml.readTransaction();
      auto const reader = ml.manifest().reader(transaction);

      for (auto const& [uri, view] : reader)
      {
        records.push_back(ManifestRecordDto{.uri = std::string{uri},
                                            .trackId = view.trackId(),
                                            .fileSize = view.fileSize(),
                                            .mtime = view.mtime(),
                                            .status = std::string{formatFileStatus(view.status())}});
      }

      return records;
    }

    LibraryDumpDto toLibraryDumpDto(library::MusicLibrary& ml,
                                    bool all,
                                    bool dictionaryFlag,
                                    bool manifestFlag,
                                    bool metadataFlag,
                                    bool resourcesFlag)
    {
      auto dto = LibraryDumpDto{};

      if (all || metadataFlag)
      {
        dto.optMetadata = toLibraryMetadataDto(ml);
      }

      if (all || dictionaryFlag)
      {
        dto.optDictionary = dictionaryEntries(ml);
      }

      if (all || manifestFlag)
      {
        dto.optManifest = manifestRecords(ml);
      }

      if (all || resourcesFlag)
      {
        dto.optResources = resourceRecords(ml);
      }

      return dto;
    }

    void dumpMetadata(library::MusicLibrary& ml, bool raw, std::ostream& os)
    {
      std::println(os, "--- Meta ---");

      if (raw)
      {
        auto const header = ml.metadataHeader();
        hexDump(std::as_bytes(std::span{&header, 1}), os);
        return;
      }

      printMetadataPlain(ml, os);
    }

    void dumpDictionary(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& dictionary = ml.dictionary();
      std::println(os, "--- Dictionary ({} entries) ---", dictionary.size());

      for (std::size_t i = 1; i <= dictionary.size(); ++i)
      {
        std::println(os, "  {}: {}", i, dictionary.get(DictionaryId{static_cast<std::uint32_t>(i)}));
      }
    }

    void dumpManifest(library::MusicLibrary& ml, bool raw, std::ostream& os)
    {
      auto const transaction = ml.readTransaction();
      auto const reader = ml.manifest().reader(transaction);
      std::println(os, "--- Manifest ---");

      if (raw)
      {
        for (auto const& [key, val] : reader.databaseReader())
        {
          auto const uri = utility::bytes::stringView(key);
          std::println(os, "URI: {}", uri);
          hexDump(val, os);
        }

        return;
      }

      for (auto const& [uri, view] : reader)
      {
        std::println(os, "  URI: {}", uri);
        std::println(os, "    Track ID: {}", view.trackId());
        std::println(os, "    File Size: {} bytes", view.fileSize());
        std::println(os, "    MTime: {}", view.mtime());
        std::println(os, "    Status: {}", formatFileStatus(view.status()));
      }
    }

    void dumpResources(library::MusicLibrary& ml, bool raw, std::ostream& os)
    {
      auto const transaction = ml.readTransaction();
      auto const reader = ml.resources().reader(transaction);
      std::println(os, "--- Resources ---");

      if (raw)
      {
        for (auto const& [resId, val] : reader)
        {
          std::println(os, "Resource ID: {} (Size: {})", resId, val.size());
          hexDump(val, os);
        }

        return;
      }

      std::size_t count = 0;
      std::size_t totalBytes = 0;

      for (auto const& entry : reader)
      {
        totalBytes += entry.second.size();
        count++;
      }

      std::println(os, "Total: {} resources, {} bytes", count, totalBytes);

      constexpr std::size_t kPreviewByteLimit = 64;

      for (auto const& [resId, val] : reader)
      {
        std::println(os, "  Resource ID: {} (Size: {})", resId, val.size());
        std::println(os, "  Preview:");
        hexDump(val.subspan(0, std::min<std::size_t>(kPreviewByteLimit, val.size())), os);
      }
    }

    void dumpLibSections(library::MusicLibrary& ml,
                         bool all,
                         bool dictionaryFlag,
                         bool manifestFlag,
                         bool metadataFlag,
                         bool resourcesFlag,
                         bool raw,
                         std::ostream& os)
    {
      if (all || metadataFlag)
      {
        dumpMetadata(ml, raw, os);
      }

      if (all || dictionaryFlag)
      {
        dumpDictionary(ml, os);
      }

      if (all || manifestFlag)
      {
        dumpManifest(ml, raw, os);
      }

      if (all || resourcesFlag)
      {
        dumpResources(ml, raw, os);
      }
    }

    void dumpLib(library::MusicLibrary& ml,
                 bool dictionaryFlag,
                 bool manifestFlag,
                 bool metadataFlag,
                 bool resourcesFlag,
                 bool raw,
                 OutputFormat format,
                 std::ostream& os)
    {
      if (raw && format != OutputFormat::Plain)
      {
        throwCommandError(Error::Code::InvalidInput, "lib dump --raw supports only plain output");
      }

      bool const all = !(dictionaryFlag || manifestFlag || metadataFlag || resourcesFlag);

      if (format != OutputFormat::Plain)
      {
        emitDocument(os, format, toLibraryDumpDto(ml, all, dictionaryFlag, manifestFlag, metadataFlag, resourcesFlag));
        return;
      }

      dumpLibSections(ml, all, dictionaryFlag, manifestFlag, metadataFlag, resourcesFlag, raw, os);
    }
  } // namespace

  void configureLibCommand(CLI::App& app, CliRuntime& cli)
  {
    auto* lib = app.add_subcommand("lib", "Library management commands");
    lib->require_subcommand(1);

    lib->add_subcommand("show", "Show library information")
      ->callback([&cli] { printMetadata(cli.musicLibrary(), cli.options().format, cli.io().out); });

    lib->add_subcommand("stats", "Show library statistics")
      ->callback([&cli]
                 { printStats(cli.musicLibrary(), cli.core().databasePath(), cli.options().format, cli.io().out); });

    lib->add_subcommand("verify", "Verify library files against the manifest")
      ->callback([&cli] { verifyLibrary(cli.musicLibrary(), cli.options().format, cli.io().out); });

    auto* relinkCmd = lib->add_subcommand("relink", "List or apply explicit file relinks");
    auto* relinkFrom = relinkCmd->add_option("--from", "missing manifest URI or path");
    auto* relinkTo = relinkCmd->add_option("--to", "new file URI or path");
    auto* relinkDryRun = addDryRunFlag(*relinkCmd);
    relinkCmd->callback(
      [&cli, relinkFrom, relinkTo, relinkDryRun]
      {
        auto optFrom = std::optional<std::string>{};
        auto optTo = std::optional<std::string>{};

        if (relinkFrom->count() != 0)
        {
          optFrom = relinkFrom->as<std::string>();
        }

        if (relinkTo->count() != 0)
        {
          optTo = relinkTo->as<std::string>();
        }

        relinkLibrary(cli.musicLibrary(), optFrom, optTo, isDryRun(relinkDryRun), cli.options().format, cli.io().out);
      });

    auto* fingerprintCmd = lib->add_subcommand("fingerprint", "Fingerprint pending manifest audio identities");
    auto* fingerprintPendingFlag =
      fingerprintCmd->add_flag("--pending", "fingerprint manifest rows with pending audio identity");
    auto* fingerprintVerbose = fingerprintCmd->add_flag("--verbose", "print fingerprint progress to stderr");
    fingerprintCmd->callback(
      [&cli, fingerprintPendingFlag, fingerprintVerbose]
      {
        fingerprintPending(cli.core(),
                           cli.musicLibrary(),
                           fingerprintPendingFlag->count() > 0,
                           fingerprintVerbose->count() > 0,
                           cli.options().format,
                           cli.io().out,
                           cli.io().err);
      });

    auto* exportCmd = lib->add_subcommand("export", "Export library to YAML");
    auto* exportPath = exportCmd->add_option("output,-o,--output", "Output YAML file path")->required();
    auto* exportMode =
      exportCmd->add_option("-m,--mode", "Export mode (delta, metadata, full, listOnly)")->default_val("full");
    exportCmd->callback(
      [&cli, exportPath, exportMode]
      {
        exportLib(cli.musicLibrary(),
                  exportPath->as<std::string>(),
                  exportMode->as<std::string>(),
                  cli.options().format,
                  cli.io().out);
      });

    auto* importCmd = lib->add_subcommand("import", "Import library from YAML");
    auto* importPath = importCmd->add_option("input,-i,--input", "Input YAML file path")->required();
    auto* importMode = importCmd->add_option("-m,--mode", "Import mode (restore, merge)")->default_val("restore");
    auto* importDryRun = addDryRunFlag(*importCmd);
    importCmd->callback(
      [&cli, importPath, importMode, importDryRun]
      {
        importLib(cli.musicLibrary(),
                  importPath->as<std::string>(),
                  importMode->as<std::string>(),
                  isDryRun(importDryRun),
                  cli.options().format,
                  cli.io().out);
      });

    auto* dumpCmd = lib->add_subcommand("dump", "Dump infrastructure databases");
    auto* dumpDictionary = dumpCmd->add_flag("--dict", "dump dictionary only");
    auto* dumpManifest = dumpCmd->add_flag("--manifest", "dump manifest only");
    auto* dumpMetadataFlag = dumpCmd->add_flag("--meta", "dump metadata only");
    auto* dumpResources = dumpCmd->add_flag("--resources", "dump resources only");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");

    dumpCmd->callback(
      [&cli, dumpDictionary, dumpManifest, dumpMetadataFlag, dumpResources, dumpRaw]
      {
        dumpLib(cli.musicLibrary(),
                dumpDictionary->count() > 0,
                dumpManifest->count() > 0,
                dumpMetadataFlag->count() > 0,
                dumpResources->count() > 0,
                dumpRaw->count() > 0,
                cli.options().format,
                cli.io().out);
      });

    auto* resource = lib->add_subcommand("resource", "Library resource commands");
    resource->require_subcommand(1);

    resource->add_subcommand("list", "List resources")
      ->callback([&cli] { listResources(cli.musicLibrary(), cli.options().format, cli.io().out); });

    auto* resourceExport = resource->add_subcommand("export", "Export a resource to a file");
    auto* resourceExportId = resourceExport->add_option("id", "resource id")->required();
    auto* resourceExportPath = resourceExport->add_option("-o,--output", "output file path")->required();
    resourceExport->callback(
      [&cli, resourceExportId, resourceExportPath]
      {
        exportResource(cli.musicLibrary(),
                       ResourceId{resourceExportId->as<std::uint32_t>()},
                       resourceExportPath->as<std::filesystem::path>(),
                       cli.options().format,
                       cli.io().out);
      });
  }
} // namespace ao::cli
