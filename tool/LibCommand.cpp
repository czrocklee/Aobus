// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "LibCommand.h"
#include <chrono>
#include <rs/library/Exporter.h>
#include <rs/library/Importer.h>

#include <array>
#include <format>
#include <ranges>

namespace rs::tool
{
  namespace
  {
    constexpr std::size_t kUuidByteCount = 16;

    std::string formatUuid(std::array<std::byte, kUuidByteCount> const& id)
    {
      constexpr std::size_t kUuidStringLength = 36;
      constexpr std::size_t kFirstDashPos = 3;
      constexpr std::size_t kSecondDashPos = 5;
      constexpr std::size_t kThirdDashPos = 7;
      constexpr std::size_t kFourthDashPos = 9;

      auto result = std::string{};
      result.reserve(kUuidStringLength);

      for (auto const [idx, byte] : std::views::enumerate(id))
      {
        result += std::format("{:02x}", static_cast<unsigned char>(byte));

        if (idx == kFirstDashPos || idx == kSecondDashPos || idx == kThirdDashPos || idx == kFourthDashPos)
        {
          result += "-";
        }
      }

      return result;
    }

    std::string formatTimestamp(std::uint64_t unixMs)
    {
      auto const tp = std::chrono::system_clock::time_point{std::chrono::milliseconds{unixMs}};
      return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
    }

    void show(rs::library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();

      os << "Library ID:        " << formatUuid(header.libraryId) << "\n";
      os << "Library Version:   " << header.libraryVersion << "\n";
      os << "Flags:             0x" << std::hex << header.flags << std::dec << "\n";
      os << "Created:           " << formatTimestamp(header.createdAtUnixMs) << "\n";
      os << "Migrated:          " << formatTimestamp(header.migratedAtUnixMs) << "\n";
    }

    void exportLib(rs::library::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
    {
      auto mode = rs::library::ExportMode::Full;

      if (modeStr == "minimum")
      {
        mode = rs::library::ExportMode::Minimum;
      }
      else if (modeStr == "metadata")
      {
        mode = rs::library::ExportMode::Metadata;
      }
      else if (modeStr == "full")
      {
        mode = rs::library::ExportMode::Full;
      }
      else
      {
        os << "Error: Invalid export mode '" << modeStr << "'. Valid modes are: minimum, metadata, full.\n";
        return;
      }

      auto exporter = rs::library::Exporter{ml};
      exporter.exportToYaml(path, mode);
      os << "Library exported to '" << path << "' using mode '" << modeStr << "'.\n";
    }

    void importLib(rs::library::MusicLibrary& ml, std::string const& path, std::ostream& os)
    {
      auto importer = rs::library::Importer{ml};
      importer.importFromYaml(path);
      os << "Library imported from '" << path << "'.\n";
    }
  }

  void setupLibCommand(CLI::App& app, rs::library::MusicLibrary& ml)
  {
    auto* lib = app.add_subcommand("lib", "Library management commands");

    lib->add_subcommand("show", "Show library information")->callback([&ml]() { show(ml, std::cout); });

    auto* exportCmd = lib->add_subcommand("export", "Export library to YAML");
    auto* exportPath = exportCmd->add_option("output,-o,--output", "Output YAML file path")->required();
    auto* exportMode = exportCmd->add_option("-m,--mode", "Export mode (minimum, metadata, full)")->default_val("full");
    exportCmd->callback([&ml, exportPath, exportMode]()
                        { exportLib(ml, exportPath->as<std::string>(), exportMode->as<std::string>(), std::cout); });

    auto* importCmd = lib->add_subcommand("import", "Import library from YAML");
    auto* importPath = importCmd->add_option("input,-i,--input", "Input YAML file path")->required();
    importCmd->callback([&ml, importPath]() { importLib(ml, importPath->as<std::string>(), std::cout); });
  }
}
