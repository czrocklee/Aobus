// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibCommand.h"

#include "ao/library/MusicLibrary.h"
#include "runtime/CoreRuntime.h"
#include "runtime/LibraryExporter.h"
#include "runtime/LibraryImporter.h"

#include <CLI/App.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>

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

    std::string formatTimestamp(std::uint64_t unixMs)
    {
      auto const tp = std::chrono::system_clock::time_point{std::chrono::milliseconds{unixMs}};
      return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
    }

    void show(library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();

      os << "Library ID:    " << formatUuid(header.libraryId) << "\n";
      os << "Library Version:  " << header.libraryVersion << "\n";
      os << "Flags:       0x" << std::hex << header.flags << std::dec << "\n";
      os << "Created:      " << formatTimestamp(header.createdAtUnixMs) << "\n";
    }

    void exportLib(library::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
    {
      auto mode = rt::ExportMode::Full;

      if (modeStr == "minimum")
      {
        mode = rt::ExportMode::Minimum;
      }
      else if (modeStr == "metadata")
      {
        mode = rt::ExportMode::Metadata;
      }
      else if (modeStr == "full")
      {
        mode = rt::ExportMode::Full;
      }
      else
      {
        os << "Error: Invalid export mode '" << modeStr << "'. Valid modes are: minimum, metadata, full.\n";
        return;
      }

      auto exporter = rt::LibraryExporter{ml};
      exporter.exportToYaml(path, mode);
      os << "Library exported to '" << path << "' using mode '" << modeStr << "'.\n";
    }

    void importLib(library::MusicLibrary& ml, std::string const& path, std::ostream& os)
    {
      auto importer = rt::LibraryImporter{ml};
      importer.importFromYaml(path);
      os << "Library imported from '" << path << "'.\n";
    }
  }

  void setupLibCommand(CLI::App& app, rt::CoreRuntime& runtime)
  {
    auto* lib = app.add_subcommand("lib", "Library management commands");

    lib->add_subcommand("show", "Show library information")
      ->callback([&runtime] { show(runtime.musicLibrary(), std::cout); });

    auto* exportCmd = lib->add_subcommand("export", "Export library to YAML");
    auto* exportPath = exportCmd->add_option("output,-o,--output", "Output YAML file path")->required();
    auto* exportMode = exportCmd->add_option("-m,--mode", "Export mode (minimum, metadata, full)")->default_val("full");
    exportCmd->callback(
      [&runtime, exportPath, exportMode]
      { exportLib(runtime.musicLibrary(), exportPath->as<std::string>(), exportMode->as<std::string>(), std::cout); });

    auto* importCmd = lib->add_subcommand("import", "Import library from YAML");
    auto* importPath = importCmd->add_option("input,-i,--input", "Input YAML file path")->required();
    importCmd->callback([&runtime, importPath]
                        { importLib(runtime.musicLibrary(), importPath->as<std::string>(), std::cout); });
  }
}
