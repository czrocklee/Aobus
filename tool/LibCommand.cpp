// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "LibCommand.h"
#include <rs/core/LibraryExporter.h>
#include <rs/core/LibraryImporter.h>

#include <iomanip>
#include <sstream>

namespace rs::tool
{
  namespace
  {
    constexpr std::size_t kUuidByteCount = 16;
    constexpr std::size_t kUuidDashIndices[] = {3, 5, 7, 9};

    std::string formatUuid(std::array<std::byte, kUuidByteCount> const& id)
    {
      auto oss = std::ostringstream{};
      oss << std::hex << std::setfill('0');

      for (std::size_t i = 0; i < id.size(); ++i)
      {
        oss << std::setw(2) << static_cast<int>(id[i]);

        if (i == kUuidDashIndices[0] || i == kUuidDashIndices[1] || i == kUuidDashIndices[2] ||
            i == kUuidDashIndices[3])
          oss << "-";
      }

      return oss.str();
    }

    std::string formatTimestamp(std::uint64_t unixMs)
    {
      auto const tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds{unixMs});
      auto const time = std::chrono::system_clock::to_time_t(tp);
      auto oss = std::ostringstream{};
      oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
      return oss.str();
    }

    void show(core::MusicLibrary& ml, std::ostream& os)
    {
      auto const& header = ml.metaHeader();

      os << "Library ID:        " << formatUuid(header.libraryId) << "\n";
      os << "Library Version:   " << header.libraryVersion << "\n";
      os << "Flags:             0x" << std::hex << header.flags << std::dec << "\n";
      os << "Created:           " << formatTimestamp(header.createdAtUnixMs) << "\n";
      os << "Migrated:          " << formatTimestamp(header.migratedAtUnixMs) << "\n";
    }

    void exportLib(core::MusicLibrary& ml, std::string const& path, std::string const& modeStr, std::ostream& os)
    {
      auto mode = core::ExportMode::Full;

      if (modeStr == "minimum")
        mode = core::ExportMode::Minimum;

      else if (modeStr == "metadata")
        mode = core::ExportMode::Metadata;

      else if (modeStr == "full")
        mode = core::ExportMode::Full;
      else
      {
        os << "Error: Invalid export mode '" << modeStr << "'. Valid modes are: minimum, metadata, full.\n";
        return;
      }

      auto exporter = core::LibraryExporter{ml};
      exporter.exportToYaml(path, mode);
      os << "Library exported to '" << path << "' using mode '" << modeStr << "'.\n";
    }

    void importLib(core::MusicLibrary& ml, std::string const& path, std::ostream& os)
    {
      auto importer = core::LibraryImporter{ml};
      importer.importFromYaml(path);
      os << "Library imported from '" << path << "'.\n";
    }
  }

  void setupLibCommand(CLI::App& app, core::MusicLibrary& ml)
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
