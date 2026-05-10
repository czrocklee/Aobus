// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LibCommand.h"
#include <ao/library/Exporter.h>
#include <ao/library/Importer.h>
#include <chrono>

#include <array>
#include <format>
#include <ranges>

namespace ao::cli
{
  namespace
  {
    constexpr std::size_t kUuidByteCount = 16;

    std::string formatUuid(std::array<std::byte, kUuidByteCount> const& id)
    {
      return std::format("{:02x}{:02x}{:02x}{:02x}-"
                         "{:02x}{:02x}-"
                         "{:02x}{:02x}-"
                         "{:02x}{:02x}-"
                         "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                         static_cast<unsigned char>(id[0]),
                         static_cast<unsigned char>(id[1]),
                         static_cast<unsigned char>(id[2]),
                         static_cast<unsigned char>(id[3]),
                         static_cast<unsigned char>(id[4]),
                         static_cast<unsigned char>(id[5]),
                         static_cast<unsigned char>(id[6]),
                         static_cast<unsigned char>(id[7]),
                         static_cast<unsigned char>(id[8]),
                         static_cast<unsigned char>(id[9]),
                         static_cast<unsigned char>(id[10]),
                         static_cast<unsigned char>(id[11]),
                         static_cast<unsigned char>(id[12]),
                         static_cast<unsigned char>(id[13]),
                         static_cast<unsigned char>(id[14]),
                         static_cast<unsigned char>(id[15]));
    }

    std::string formatTimestamp(std::uint64_t unixMs)
    {
      auto const tp = std::chrono::system_clock::time_point{std::chrono::milliseconds{unixMs}};
      return std::format("{:%Y-%m-%d %H:%M:%S}", tp);
    }

    void show(ao::library::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();

      os << "Library ID:        " << formatUuid(header.libraryId) << "\n";
      os << "Library Version:   " << header.libraryVersion << "\n";
      os << "Flags:             0x" << std::hex << header.flags << std::dec << "\n";
      os << "Created:           " << formatTimestamp(header.createdAtUnixMs) << "\n";
      os << "Migrated:          " << formatTimestamp(header.migratedAtUnixMs) << "\n";
    }

    void exportLib(ao::library::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
    {
      auto mode = ao::library::ExportMode::Full;

      if (modeStr == "minimum")
      {
        mode = ao::library::ExportMode::Minimum;
      }
      else if (modeStr == "metadata")
      {
        mode = ao::library::ExportMode::Metadata;
      }
      else if (modeStr == "full")
      {
        mode = ao::library::ExportMode::Full;
      }
      else
      {
        os << "Error: Invalid export mode '" << modeStr << "'. Valid modes are: minimum, metadata, full.\n";
        return;
      }

      auto exporter = ao::library::Exporter{ml};
      exporter.exportToYaml(path, mode);
      os << "Library exported to '" << path << "' using mode '" << modeStr << "'.\n";
    }

    void importLib(ao::library::MusicLibrary& ml, std::string const& path, std::ostream& os)
    {
      auto importer = ao::library::Importer{ml};
      importer.importFromYaml(path);
      os << "Library imported from '" << path << "'.\n";
    }
  }

  void setupLibCommand(CLI::App& app, ao::library::MusicLibrary& ml)
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
