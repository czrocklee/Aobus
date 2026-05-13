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
      // NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-pro-bounds-constant-array-index)
      auto const cast = [&](std::size_t idx) { return static_cast<unsigned char>(id[idx]); };

      return std::format("{:02x}{:02x}{:02x}{:02x}-"
                         "{:02x}{:02x}-"
                         "{:02x}{:02x}-"
                         "{:02x}{:02x}-"
                         "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                         cast(0),
                         cast(1),
                         cast(2),
                         cast(3),
                         cast(4),
                         cast(5),
                         cast(6),
                         cast(7),
                         cast(8),
                         cast(9),
                         cast(10),
                         cast(11),
                         cast(12),
                         cast(13),
                         cast(14),
                         cast(15));
      // NOLINTEND(readability-magic-numbers, cppcoreguidelines-pro-bounds-constant-array-index)
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
      os << "Migrated:     " << formatTimestamp(header.migratedAtUnixMs) << "\n";
    }

    void exportLib(library::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
    {
      auto mode = library::ExportMode::Full;

      if (modeStr == "minimum")
      {
        mode = library::ExportMode::Minimum;
      }
      else if (modeStr == "metadata")
      {
        mode = library::ExportMode::Metadata;
      }
      else if (modeStr == "full")
      {
        mode = library::ExportMode::Full;
      }
      else
      {
        os << "Error: Invalid export mode '" << modeStr << "'. Valid modes are: minimum, metadata, full.\n";
        return;
      }

      auto exporter = library::Exporter{ml};
      exporter.exportToYaml(path, mode);
      os << "Library exported to '" << path << "' using mode '" << modeStr << "'.\n";
    }

    void importLib(library::MusicLibrary& ml, std::string const& path, std::ostream& os)
    {
      auto importer = library::Importer{ml};
      importer.importFromYaml(path);
      os << "Library imported from '" << path << "'.\n";
    }
  }

  void setupLibCommand(CLI::App& app, library::MusicLibrary& ml)
  {
    auto* lib = app.add_subcommand("lib", "Library management commands");

    lib->add_subcommand("show", "Show library information")->callback([&ml] { show(ml, std::cout); });

    auto* exportCmd = lib->add_subcommand("export", "Export library to YAML");
    auto* exportPath = exportCmd->add_option("output,-o,--output", "Output YAML file path")->required();
    auto* exportMode = exportCmd->add_option("-m,--mode", "Export mode (minimum, metadata, full)")->default_val("full");
    exportCmd->callback([&ml, exportPath, exportMode]
                        { exportLib(ml, exportPath->as<std::string>(), exportMode->as<std::string>(), std::cout); });

    auto* importCmd = lib->add_subcommand("import", "Import library from YAML");
    auto* importPath = importCmd->add_option("input,-i,--input", "Input YAML file path")->required();
    importCmd->callback([&ml, importPath] { importLib(ml, importPath->as<std::string>(), std::cout); });
  }
}
