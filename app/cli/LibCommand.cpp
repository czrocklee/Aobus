// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "DumpUtils.h"
#include "Output.h"
#include "ScanOutput.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/ListStore.h>
#include <ao/library/Meta.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Uuid.h>

#include <CLI/App.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
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
  namespace
  {
    std::string formatTimestamp(std::chrono::sys_time<std::chrono::milliseconds> timestamp)
    {
      auto const tp = std::chrono::system_clock::time_point{timestamp.time_since_epoch()};
      return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
    }

    void showPlain(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();

      std::println(os, "Library ID:    {}", utility::formatUuid(header.libraryId));
      std::println(os, "Library Version:  {}", header.libraryVersion);
      std::println(os, "Flags:       0x{:x}", header.flags);
      std::println(os, "Created:      {}", formatTimestamp(header.createdTime));
    }

    void showYaml(library::MusicLibrary& ml, std::ostream& os, std::int32_t indent = 0)
    {
      auto const& header = ml.metaHeader();
      yamlKeyValue(os, indent, "libraryId", utility::formatUuid(header.libraryId));
      yamlKeyValue(os, indent, "libraryVersion", static_cast<std::uint64_t>(header.libraryVersion));
      yamlKeyValue(os, indent, "flags", std::format("0x{:x}", header.flags));
      yamlKeyValue(os, indent, "createdTime", formatTimestamp(header.createdTime));
    }

    void showJsonFields(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();
      auto object = JsonObject{os};
      object.stringField("libraryId", utility::formatUuid(header.libraryId));
      object.uintField("libraryVersion", header.libraryVersion);
      object.stringField("flags", std::format("0x{:x}", header.flags));
      object.stringField("createdTime", formatTimestamp(header.createdTime));
      object.close();
    }

    void show(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      if (format == OutputFormat::Yaml)
      {
        showYaml(ml, os);
      }
      else if (format == OutputFormat::Json)
      {
        showJsonFields(ml, os);
        std::println(os);
      }
      else
      {
        showPlain(ml, os);
      }
    }

    void printLibraryTransfer(std::ostream& os,
                              OutputFormat format,
                              std::string_view action,
                              std::string const& path,
                              std::string const& modeStr)
    {
      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "action", action);
        yamlKeyValue(os, 0, "path", path);
        yamlKeyValue(os, 0, "mode", modeStr);
        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.stringField("action", action);
        object.stringField("path", path);
        object.stringField("mode", modeStr);
        object.close();
        std::println(os);
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

      if (auto const result = importer.importFromYaml(path, mode); !result)
      {
        auto const& error = result.error();
        throwCommandError(error, "import failed: {}", error.message);
      }

      printLibraryTransfer(os, format, "import", path, modeStr);
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
        auto const txn = ml.readTransaction();

        for ([[maybe_unused]] auto const& entry : ml.tracks().reader(txn))
        {
          ++stats.tracks;
        }

        for ([[maybe_unused]] auto const& entry : ml.lists().reader(txn))
        {
          ++stats.lists;
        }

        for (auto const& [_, bytes] : ml.resources().reader(txn))
        {
          ++stats.resources;
          stats.resourceBytes += bytes.size();
        }

        for ([[maybe_unused]] auto const& entry : ml.manifest().reader(txn))
        {
          ++stats.manifest;
        }

        auto tagIds = std::unordered_set<std::uint32_t>{};
        auto const trackReader = ml.tracks().reader(txn);

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

      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "tracks", static_cast<std::uint64_t>(stats.tracks));
        yamlKeyValue(os, 0, "lists", static_cast<std::uint64_t>(stats.lists));
        yamlKeyValue(os, 0, "resources", static_cast<std::uint64_t>(stats.resources));
        yamlKeyValue(os, 0, "resourceBytes", static_cast<std::uint64_t>(stats.resourceBytes));
        yamlKeyValue(os, 0, "manifest", static_cast<std::uint64_t>(stats.manifest));
        yamlKeyValue(os, 0, "dictionary", static_cast<std::uint64_t>(stats.dictionary));
        yamlKeyValue(os, 0, "tags", static_cast<std::uint64_t>(stats.tags));
        yamlKeyValue(os, 0, "diskBytes", stats.diskBytes);
        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.uintField("tracks", static_cast<std::uint64_t>(stats.tracks));
        object.uintField("lists", static_cast<std::uint64_t>(stats.lists));
        object.uintField("resources", static_cast<std::uint64_t>(stats.resources));
        object.uintField("resourceBytes", static_cast<std::uint64_t>(stats.resourceBytes));
        object.uintField("manifest", static_cast<std::uint64_t>(stats.manifest));
        object.uintField("dictionary", static_cast<std::uint64_t>(stats.dictionary));
        object.uintField("tags", static_cast<std::uint64_t>(stats.tags));
        object.uintField("diskBytes", stats.diskBytes);
        object.close();
        std::println(os);
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

    bool isVerifyIssue(library::ScanClassification classification)
    {
      return classification == library::ScanClassification::Changed ||
             classification == library::ScanClassification::Missing ||
             classification == library::ScanClassification::Error;
    }

    bool isVerifyFailure(library::ScanClassification classification)
    {
      return classification == library::ScanClassification::Missing ||
             classification == library::ScanClassification::Error;
    }

    void printVerifyIssues(std::vector<library::ScanItem> const& issues,
                           bool failed,
                           OutputFormat format,
                           std::ostream& os)
    {
      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "ok", !failed);

        if (issues.empty())
        {
          std::println(os, "issues: []");
          return;
        }

        std::println(os, "issues:");

        for (auto const& item : issues)
        {
          std::println(os, "  - type: {}", yamlQuote(scanClassificationName(item.classification)));
          yamlKeyValue(os, 4, "uri", item.uri);

          if (!item.errorMessage.empty())
          {
            yamlKeyValue(os, 4, "message", item.errorMessage);
          }
        }

        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.boolField("ok", !failed);
        object.field("issues");
        auto array = JsonArray{os};

        for (auto const& item : issues)
        {
          array.element();
          auto issue = JsonObject{os};
          issue.stringField("type", scanClassificationName(item.classification));
          issue.stringField("uri", item.uri);

          if (!item.errorMessage.empty())
          {
            issue.stringField("message", item.errorMessage);
          }
        }

        array.close();
        object.close();
        std::println(os);
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
      auto scanner = library::LibraryScanner{ml};
      auto planResult = scanner.buildPlan();

      if (!planResult)
      {
        auto const& error = planResult.error();
        throwCommandError(error, "verify failed: {}", error.message);
      }

      auto issues = std::vector<library::ScanItem>{};
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

    void listResources(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      auto const txn = ml.readTransaction();
      auto const reader = ml.resources().reader(txn);
      bool hasRows = false;

      for (auto const& [id, bytes] : reader)
      {
        if (format == OutputFormat::Yaml)
        {
          if (!hasRows)
          {
            std::println(os, "resources:");
          }

          std::println(os, "  - id: {}", id.raw());
          yamlKeyValue(os, 4, "size", static_cast<std::uint64_t>(bytes.size()));
        }
        else if (format == OutputFormat::Json)
        {
          auto object = JsonObject{os};
          object.uintField("id", id.raw());
          object.uintField("size", static_cast<std::uint64_t>(bytes.size()));
          object.close();
          std::println(os);
        }
        else
        {
          std::println(os, "{}  {}", id.raw(), bytes.size());
        }

        hasRows = true;
      }

      if (format == OutputFormat::Yaml && !hasRows)
      {
        std::println(os, "resources: []");
      }
    }

    void printResourceExport(ResourceId id,
                             std::filesystem::path const& path,
                             std::size_t size,
                             OutputFormat format,
                             std::ostream& os)
    {
      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "id", static_cast<std::uint64_t>(id.raw()));
        yamlKeyValue(os, 0, "output", path.string());
        yamlKeyValue(os, 0, "size", static_cast<std::uint64_t>(size));
        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.uintField("id", id.raw());
        object.stringField("output", path.string());
        object.uintField("size", static_cast<std::uint64_t>(size));
        object.close();
        std::println(os);
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
        auto const txn = ml.readTransaction();
        auto const reader = ml.resources().reader(txn);
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

    void dumpMeta(library::MusicLibrary& ml, bool raw, OutputFormat format, std::ostream& os)
    {
      if (format == OutputFormat::Yaml && !raw)
      {
        std::println(os, "meta:");
        showYaml(ml, os, 2);
      }
      else if (raw)
      {
        std::println(os, "--- Meta ---");
        auto const& header = ml.metaHeader();
        hexDump(std::as_bytes(std::span{&header, 1}), os);
      }
      else
      {
        std::println(os, "--- Meta ---");
        showPlain(ml, os);
      }
    }

    void dumpDictionary(library::MusicLibrary& ml, OutputFormat format, std::ostream& os)
    {
      if (auto const& dict = ml.dictionary(); format == OutputFormat::Yaml)
      {
        std::println(os, "dictionary:");

        for (std::size_t i = 1; i <= dict.size(); ++i)
        {
          std::println(os, "  {}: {}", i, yamlQuote(dict.get(DictionaryId{static_cast<std::uint32_t>(i)})));
        }
      }
      else if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};

        for (std::size_t i = 1; i <= dict.size(); ++i)
        {
          object.stringField(std::to_string(i), dict.get(DictionaryId{static_cast<std::uint32_t>(i)}));
        }
      }
      else
      {
        std::println(os, "--- Dictionary ({} entries) ---", dict.size());

        for (std::size_t i = 1; i <= dict.size(); ++i)
        {
          std::println(os, "  {}: {}", i, dict.get(DictionaryId{static_cast<std::uint32_t>(i)}));
        }
      }
    }

    void dumpManifest(library::MusicLibrary& ml, bool raw, OutputFormat format, std::ostream& os)
    {
      auto const txn = ml.readTransaction();

      if (auto const reader = ml.manifest().reader(txn); format == OutputFormat::Yaml && !raw)
      {
        bool hasRows = false;

        for (auto const& [uri, view] : reader)
        {
          if (!hasRows)
          {
            std::println(os, "manifest:");
          }

          std::println(os, "  - uri: {}", yamlQuote(uri));
          yamlKeyValue(os, 4, "trackId", static_cast<std::uint64_t>(view.trackId().raw()));
          yamlKeyValue(os, 4, "fileSize", view.fileSize());
          yamlKeyValue(os, 4, "mtime", view.mtime());
          yamlKeyValue(os, 4, "status", formatFileStatus(view.status()));
          hasRows = true;
        }

        if (!hasRows)
        {
          std::println(os, "manifest: []");
        }
      }
      else if (format == OutputFormat::Json && !raw)
      {
        auto array = JsonArray{os};

        for (auto const& [uri, view] : reader)
        {
          array.element();
          auto object = JsonObject{os};
          object.stringField("uri", uri);
          object.uintField("trackId", view.trackId().raw());
          object.uintField("fileSize", view.fileSize());
          object.uintField("mtime", view.mtime());
          object.stringField("status", formatFileStatus(view.status()));
        }
      }
      else if (raw)
      {
        std::println(os, "--- Manifest ---");

        for (auto const& [key, val] : reader.databaseReader())
        {
          auto const uri = utility::bytes::stringView(key);
          std::println(os, "URI: {}", uri);
          hexDump(val, os);
        }
      }
      else
      {
        std::println(os, "--- Manifest ---");

        for (auto const& [uri, view] : reader)
        {
          std::println(os, "  URI: {}", uri);
          std::println(os, "    Track ID: {}", view.trackId());
          std::println(os, "    File Size: {} bytes", view.fileSize());
          std::println(os, "    MTime: {}", view.mtime());
          std::println(os, "    Status: {}", formatFileStatus(view.status()));
        }
      }
    }

    void dumpResources(library::MusicLibrary& ml, bool raw, OutputFormat format, std::ostream& os)
    {
      auto const txn = ml.readTransaction();

      if (auto const reader = ml.resources().reader(txn); format == OutputFormat::Yaml && !raw)
      {
        bool hasRows = false;

        for (auto const& [resId, val] : reader)
        {
          if (!hasRows)
          {
            std::println(os, "resources:");
          }

          std::println(os, "  - id: {}", resId);
          yamlKeyValue(os, 4, "size", static_cast<std::uint64_t>(val.size()));
          hasRows = true;
        }

        if (!hasRows)
        {
          std::println(os, "resources: []");
        }
      }
      else if (format == OutputFormat::Json && !raw)
      {
        auto array = JsonArray{os};

        for (auto const& [resId, val] : reader)
        {
          array.element();
          auto object = JsonObject{os};
          object.uintField("id", resId.raw());
          object.uintField("size", static_cast<std::uint64_t>(val.size()));
        }
      }
      else if (raw)
      {
        std::println(os, "--- Resources ---");

        for (auto const& [resId, val] : reader)
        {
          std::println(os, "Resource ID: {} (Size: {})", resId, val.size());
          hexDump(val, os);
        }
      }
      else
      {
        std::println(os, "--- Resources ---");
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
    }

    void dumpLibJson(library::MusicLibrary& ml,
                     bool all,
                     bool dictFlag,
                     bool manifestFlag,
                     bool metaFlag,
                     bool resourcesFlag,
                     std::ostream& os)
    {
      auto object = JsonObject{os};

      if (all || metaFlag)
      {
        object.field("meta");
        showJsonFields(ml, os);
      }

      if (all || dictFlag)
      {
        object.field("dictionary");
        dumpDictionary(ml, OutputFormat::Json, os);
      }

      if (all || manifestFlag)
      {
        object.field("manifest");
        dumpManifest(ml, false, OutputFormat::Json, os);
      }

      if (all || resourcesFlag)
      {
        object.field("resources");
        dumpResources(ml, false, OutputFormat::Json, os);
      }

      object.close();
      std::println(os);
    }

    void printJsonSectionBreak(OutputFormat format, std::ostream& os)
    {
      if (format == OutputFormat::Json)
      {
        std::println(os);
      }
    }

    void dumpLibSections(library::MusicLibrary& ml,
                         bool all,
                         bool dictFlag,
                         bool manifestFlag,
                         bool metaFlag,
                         bool resourcesFlag,
                         bool raw,
                         OutputFormat format,
                         std::ostream& os)
    {
      if (all || metaFlag)
      {
        dumpMeta(ml, raw, format, os);
      }

      if (all || dictFlag)
      {
        dumpDictionary(ml, format, os);
        printJsonSectionBreak(format, os);
      }

      if (all || manifestFlag)
      {
        dumpManifest(ml, raw, format, os);
        printJsonSectionBreak(format, os);
      }

      if (all || resourcesFlag)
      {
        dumpResources(ml, raw, format, os);
        printJsonSectionBreak(format, os);
      }
    }

    void dumpLib(library::MusicLibrary& ml,
                 bool dictFlag,
                 bool manifestFlag,
                 bool metaFlag,
                 bool resourcesFlag,
                 bool raw,
                 OutputFormat format,
                 std::ostream& os)
    {
      if (raw && format != OutputFormat::Plain)
      {
        throwCommandError(Error::Code::InvalidInput, "lib dump --raw supports only plain output");
      }

      bool const all = !(dictFlag || manifestFlag || metaFlag || resourcesFlag);

      if (format == OutputFormat::Json && !raw)
      {
        dumpLibJson(ml, all, dictFlag, manifestFlag, metaFlag, resourcesFlag, os);
        return;
      }

      dumpLibSections(ml, all, dictFlag, manifestFlag, metaFlag, resourcesFlag, raw, format, os);
    }
  } // namespace

  void setupLibCommand(CLI::App& app, CliContext& context)
  {
    auto* lib = app.add_subcommand("lib", "Library management commands");
    lib->require_subcommand(1);

    lib->add_subcommand("show", "Show library information")
      ->callback([&context] { show(context.musicLibrary(), context.options().format, context.io().out); });

    lib->add_subcommand("stats", "Show library statistics")
      ->callback(
        [&context]
        {
          printStats(
            context.musicLibrary(), context.runtime().databasePath(), context.options().format, context.io().out);
        });

    lib->add_subcommand("verify", "Verify library files against the manifest")
      ->callback([&context] { verifyLibrary(context.musicLibrary(), context.options().format, context.io().out); });

    auto* exportCmd = lib->add_subcommand("export", "Export library to YAML");
    auto* exportPath = exportCmd->add_option("output,-o,--output", "Output YAML file path")->required();
    auto* exportMode =
      exportCmd->add_option("-m,--mode", "Export mode (delta, metadata, full, listOnly)")->default_val("full");
    exportCmd->callback(
      [&context, exportPath, exportMode]
      {
        exportLib(context.musicLibrary(),
                  exportPath->as<std::string>(),
                  exportMode->as<std::string>(),
                  context.options().format,
                  context.io().out);
      });

    auto* importCmd = lib->add_subcommand("import", "Import library from YAML");
    auto* importPath = importCmd->add_option("input,-i,--input", "Input YAML file path")->required();
    auto* importMode = importCmd->add_option("-m,--mode", "Import mode (restore, merge)")->default_val("restore");
    importCmd->callback(
      [&context, importPath, importMode]
      {
        importLib(context.musicLibrary(),
                  importPath->as<std::string>(),
                  importMode->as<std::string>(),
                  context.options().format,
                  context.io().out);
      });

    auto* dumpCmd = lib->add_subcommand("dump", "Dump infrastructure databases");
    auto* dumpDict = dumpCmd->add_flag("--dict", "dump dictionary only");
    auto* dumpManifest = dumpCmd->add_flag("--manifest", "dump manifest only");
    auto* dumpMeta = dumpCmd->add_flag("--meta", "dump meta only");
    auto* dumpResources = dumpCmd->add_flag("--resources", "dump resources only");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");

    dumpCmd->callback(
      [&context, dumpDict, dumpManifest, dumpMeta, dumpResources, dumpRaw]
      {
        dumpLib(context.musicLibrary(),
                dumpDict->count() > 0,
                dumpManifest->count() > 0,
                dumpMeta->count() > 0,
                dumpResources->count() > 0,
                dumpRaw->count() > 0,
                context.options().format,
                context.io().out);
      });

    auto* resource = lib->add_subcommand("resource", "Library resource commands");
    resource->require_subcommand(1);

    resource->add_subcommand("list", "List resources")
      ->callback([&context] { listResources(context.musicLibrary(), context.options().format, context.io().out); });

    auto* resourceExport = resource->add_subcommand("export", "Export a resource to a file");
    auto* resourceExportId = resourceExport->add_option("id", "resource id")->required();
    auto* resourceExportPath = resourceExport->add_option("-o,--output", "output file path")->required();
    resourceExport->callback(
      [&context, resourceExportId, resourceExportPath]
      {
        exportResource(context.musicLibrary(),
                       ResourceId{resourceExportId->as<std::uint32_t>()},
                       resourceExportPath->as<std::filesystem::path>(),
                       context.options().format,
                       context.io().out);
      });
  }
} // namespace ao::cli
