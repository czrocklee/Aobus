// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibCommand.h"

#include "DumpUtils.h"
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/Meta.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/LibraryYamlExporter.h>
#include <ao/rt/LibraryYamlImporter.h>
#include <ao/utility/ByteView.h>

#include <CLI/App.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace ao::cli
{
  namespace
  {
    constexpr std::size_t kUuidByteCount = 16;
    constexpr std::size_t kHexCharsPerByte = 2;
    constexpr std::size_t kUuidTimeLowByteCount = 4;
    constexpr std::size_t kUuidTimeMidByteCount = 2;
    constexpr std::size_t kUuidTimeHighByteCount = 2;
    constexpr std::size_t kUuidClockSeqByteCount = 2;
    constexpr std::size_t kUuidNodeByteCount = 6;
    constexpr auto kUuidGroupByteCounts = std::to_array<std::size_t>({kUuidTimeLowByteCount,
                                                                      kUuidTimeMidByteCount,
                                                                      kUuidTimeHighByteCount,
                                                                      kUuidClockSeqByteCount,
                                                                      kUuidNodeByteCount});
    constexpr auto kUuidHyphenCount = kUuidGroupByteCounts.size() - 1;
    constexpr auto kUuidTextLength = (kUuidByteCount * kHexCharsPerByte) + kUuidHyphenCount;

    std::string formatUuid(std::array<std::byte, kUuidByteCount> const& id)
    {
      auto result = std::string{};
      result.reserve(kUuidTextLength);
      auto byteIndex = std::size_t{0};

      for (auto groupIndex = std::size_t{0}; groupIndex < kUuidGroupByteCounts.size(); ++groupIndex)
      {
        if (groupIndex > 0)
        {
          result.push_back('-');
        }

        for (auto groupByte = std::size_t{0}; groupByte < kUuidGroupByteCounts.at(groupIndex); ++groupByte)
        {
          result += std::format("{:02x}", static_cast<unsigned char>(id.at(byteIndex)));
          ++byteIndex;
        }
      }

      return result;
    }

    std::string formatTimestamp(std::chrono::sys_time<std::chrono::milliseconds> timestamp)
    {
      auto const tp = std::chrono::system_clock::time_point{timestamp.time_since_epoch()};
      return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
    }

    void show(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();

      os << "Library ID:    " << formatUuid(header.libraryId) << "\n";
      os << "Library Version:  " << header.libraryVersion << "\n";
      os << "Flags:       0x" << std::hex << header.flags << std::dec << "\n";
      os << "Created:      " << formatTimestamp(header.createdTime) << "\n";
    }

    void exportLib(library::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
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
        os << "Error: Invalid export mode '" << modeStr << "'. Valid modes are: delta, metadata, full, listOnly.\n";
        return;
      }

      auto exporter = rt::LibraryYamlExporter{ml};

      if (auto const result = exporter.exportToYaml(path, mode); !result)
      {
        os << "Error: Export failed: " << result.error().message << '\n';
        return;
      }

      os << "Library exported to '" << path << "' using mode '" << modeStr << "'.\n";
    }

    void importLib(library::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
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
        os << "Error: Invalid import mode '" << modeStr << "'. Valid modes are: restore, merge.\n";
        return;
      }

      auto importer = rt::LibraryYamlImporter{ml};

      if (auto const result = importer.importFromYaml(path, mode); !result)
      {
        os << "Error: Import failed: " << result.error().message << '\n';
        return;
      }

      os << "Library imported from '" << path << "' using mode '" << modeStr << "'.\n";
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

    void dumpMeta(library::MusicLibrary& ml, bool raw, bool yaml, std::ostream& os)
    {
      if (yaml)
      {
        os << "meta:\n";
        auto const& header = ml.metaHeader();
        os << "  libraryId: \"" << formatUuid(header.libraryId) << "\"\n"
           << "  libraryVersion: " << header.libraryVersion << "\n"
           << "  flags: \"0x" << std::hex << header.flags << std::dec << "\"\n"
           << "  createdTime: \"" << formatTimestamp(header.createdTime) << "\"\n";
      }
      else if (raw)
      {
        os << "--- Meta ---\n";
        auto const& header = ml.metaHeader();
        hexDump(std::as_bytes(std::span{&header, 1}), os);
      }
      else
      {
        os << "--- Meta ---\n";
        show(ml, os);
      }
    }

    void dumpDictionary(library::MusicLibrary& ml, bool yaml, std::ostream& os)
    {
      if (auto const& dict = ml.dictionary(); yaml)
      {
        os << "dictionary:\n";

        for (std::size_t i = 1; i <= dict.size(); ++i)
        {
          os << "  " << i << ": \"" << dict.get(DictionaryId{static_cast<std::uint32_t>(i)}) << "\"\n";
        }
      }
      else
      {
        os << "--- Dictionary (" << dict.size() << " entries) ---\n";

        for (std::size_t i = 1; i <= dict.size(); ++i)
        {
          os << "  " << i << ": " << dict.get(DictionaryId{static_cast<std::uint32_t>(i)}) << "\n";
        }
      }
    }

    void dumpManifest(library::MusicLibrary& ml, bool raw, bool yaml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();

      if (auto const reader = ml.manifest().reader(txn); yaml)
      {
        os << "manifest:\n";

        for (auto const& [uri, view] : reader)
        {
          os << "  - uri: \"" << uri << "\"\n"
             << "    trackId: " << view.trackId() << "\n"
             << "    fileSize: " << view.fileSize() << "\n"
             << "    mtime: " << view.mtime() << "\n"
             << "    status: \"" << formatFileStatus(view.status()) << "\"\n";
        }
      }
      else if (raw)
      {
        os << "--- Manifest ---\n";

        for (auto const& [key, val] : reader.databaseReader())
        {
          auto const uri = utility::bytes::stringView(key);
          os << "URI: " << uri << "\n";
          hexDump(val, os);
        }
      }
      else
      {
        os << "--- Manifest ---\n";

        for (auto const& [uri, view] : reader)
        {
          os << "  URI: " << uri << "\n"
             << "    Track ID: " << view.trackId() << "\n"
             << "    File Size: " << view.fileSize() << " bytes\n"
             << "    MTime: " << view.mtime() << "\n"
             << "    Status: " << formatFileStatus(view.status()) << "\n";
        }
      }
    }

    void dumpResources(library::MusicLibrary& ml, bool raw, bool yaml, std::ostream& os)
    {
      auto const txn = ml.readTransaction();

      if (auto const reader = ml.resources().reader(txn); yaml)
      {
        os << "resources:\n";

        for (auto const& [key, val] : reader)
        {
          auto const resId = *utility::layout::view<std::uint32_t>(key);
          os << "  - id: " << resId << "\n"
             << "    size: " << val.size() << "\n";
        }
      }
      else if (raw)
      {
        os << "--- Resources ---\n";

        for (auto const& [key, val] : reader)
        {
          auto const resId = *utility::layout::view<std::uint32_t>(key);
          os << "Resource ID: " << resId << " (Size: " << val.size() << ")\n";
          hexDump(val, os);
        }
      }
      else
      {
        os << "--- Resources ---\n";
        std::size_t count = 0;
        std::size_t totalBytes = 0;

        for (auto const& [key, val] : reader)
        {
          totalBytes += val.size();
          count++;
        }

        os << "Total: " << count << " resources, " << totalBytes << " bytes\n";

        constexpr std::size_t kPreviewByteLimit = 64;

        for (auto const& [key, val] : reader)
        {
          auto const resId = *utility::layout::view<std::uint32_t>(key);
          os << "  Resource ID: " << resId << " (Size: " << val.size() << ")\n";
          os << "  Preview:\n";
          hexDump(val.subspan(0, std::min<std::size_t>(kPreviewByteLimit, val.size())), os);
        }
      }
    }

    void dumpLib(library::MusicLibrary& ml,
                 bool dictFlag,
                 bool manifestFlag,
                 bool metaFlag,
                 bool resourcesFlag,
                 bool raw,
                 bool yaml,
                 std::ostream& os)
    {
      bool const all = !(dictFlag || manifestFlag || metaFlag || resourcesFlag);

      if (all || metaFlag)
      {
        dumpMeta(ml, raw, yaml, os);
      }

      if (all || dictFlag)
      {
        dumpDictionary(ml, yaml, os);
      }

      if (all || manifestFlag)
      {
        dumpManifest(ml, raw, yaml, os);
      }

      if (all || resourcesFlag)
      {
        dumpResources(ml, raw, yaml, os);
      }
    }
  }

  void setupLibCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* lib = app.add_subcommand("lib", "Library management commands");

    lib->add_subcommand("show", "Show library information")
      ->callback([&runtime] { show(runtime.musicLibrary(), std::cout); });

    auto* exportCmd = lib->add_subcommand("export", "Export library to YAML");
    auto* exportPath = exportCmd->add_option("output,-o,--output", "Output YAML file path")->required();
    auto* exportMode =
      exportCmd->add_option("-m,--mode", "Export mode (delta, metadata, full, listOnly)")->default_val("full");
    exportCmd->callback(
      [&runtime, exportPath, exportMode]
      { exportLib(runtime.musicLibrary(), exportPath->as<std::string>(), exportMode->as<std::string>(), std::cout); });

    auto* importCmd = lib->add_subcommand("import", "Import library from YAML");
    auto* importPath = importCmd->add_option("input,-i,--input", "Input YAML file path")->required();
    auto* importMode = importCmd->add_option("-m,--mode", "Import mode (restore, merge)")->default_val("restore");
    importCmd->callback(
      [&runtime, importPath, importMode]
      { importLib(runtime.musicLibrary(), importPath->as<std::string>(), importMode->as<std::string>(), std::cout); });

    auto* dumpCmd = lib->add_subcommand("dump", "Dump infrastructure databases");
    auto* dumpDict = dumpCmd->add_flag("--dict", "dump dictionary only");
    auto* dumpManifest = dumpCmd->add_flag("--manifest", "dump manifest only");
    auto* dumpMeta = dumpCmd->add_flag("--meta", "dump meta only");
    auto* dumpResources = dumpCmd->add_flag("--resources", "dump resources only");
    auto* dumpRaw = dumpCmd->add_flag("--raw", "hex dump raw bytes");
    auto* dumpYaml = dumpCmd->add_flag("--yaml", "output as YAML");

    dumpCmd->callback(
      [&runtime, dumpDict, dumpManifest, dumpMeta, dumpResources, dumpRaw, dumpYaml]
      {
        dumpLib(runtime.musicLibrary(),
                dumpDict->count() > 0,
                dumpManifest->count() > 0,
                dumpMeta->count() > 0,
                dumpResources->count() > 0,
                dumpRaw->count() > 0,
                dumpYaml->count() > 0,
                std::cout);
      });
  }
} // namespace ao::cli
