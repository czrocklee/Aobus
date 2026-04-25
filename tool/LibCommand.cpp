// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "LibCommand.h"
#include "BasicCommand.h"
#include <rs/core/LibraryExporter.h>
#include <rs/core/LibraryImporter.h>

#include <iomanip>
#include <sstream>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  std::string formatUuid(std::array<std::byte, 16> const& id)
  {
    auto oss = std::ostringstream{};
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < id.size(); ++i)
    {
      oss << std::setw(2) << static_cast<int>(id[i]);
      if (i == 3 || i == 5 || i == 7 || i == 9)
        oss << "-";
    }
    return oss.str();
  }

  std::string formatTimestamp(std::uint64_t unixMs)
  {
    auto const tp = std::chrono::time_point<std::chrono::system_clock>(
        std::chrono::milliseconds{unixMs});
    auto const time = std::chrono::system_clock::to_time_t(tp);
    auto oss = std::ostringstream{};
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return oss.str();
  }

  void show(core::MusicLibrary& ml, std::ostream& os)
  {
    auto const& header = ml.metaHeader();

    os << "Library ID:        " << formatUuid(header.libraryId) << "\n";
    os << "Schema Version:    " << header.librarySchemaVersion << "\n";
    os << "Track Layout Ver:  " << header.trackLayoutVersion << "\n";
    os << "List Layout Ver:   " << header.listLayoutVersion << "\n";
    os << "Flags:             0x" << std::hex << header.flags << std::dec << "\n";
    os << "Created:           " << formatTimestamp(header.createdAtUnixMs) << "\n";
    os << "Migrated:          " << formatTimestamp(header.migratedAtUnixMs) << "\n";
  }

  void exportLib(core::MusicLibrary& ml, bpo::variables_map const& vm, std::ostream& os)
  {
    if (!vm.count("output"))
    {
      os << "Error: Output path is required for export.\n";
      return;
    }

    auto const path = vm["output"].as<std::string>();
    auto mode = core::ExportMode::Full;

    if (vm.count("mode"))
    {
      auto const modeStr = vm["mode"].as<std::string>();
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
    }

    auto exporter = core::LibraryExporter{ml};
    exporter.exportToYaml(path, mode);
    os << "Library exported to '" << path << "' using mode '" << vm["mode"].as<std::string>() << "'.\n";
  }

  void importLib(core::MusicLibrary& ml, bpo::variables_map const& vm, std::ostream& os)
  {
    if (!vm.count("input"))
    {
      os << "Error: Input path is required for import.\n";
      return;
    }

    auto const path = vm["input"].as<std::string>();
    auto importer = core::LibraryImporter{ml};
    importer.importFromYaml(path);
    os << "Library imported from '" << path << "'.\n";
  }
}

LibCommand::LibCommand(core::MusicLibrary& ml) : _ml{ml}
{
  addCommand<BasicCommand>("show").setExecutor(
      [this]([[maybe_unused]] auto const& vm, auto& os) { return show(_ml, os); });

  addCommand<BasicCommand>("export")
      .addOption("output,o", bpo::value<std::string>(), "Output YAML file path", 1)
      .addOption("mode,m", bpo::value<std::string>()->default_value("full"), "Export mode (minimum, metadata, full)")
      .setExecutor([this](auto const& vm, auto& os) { return exportLib(_ml, vm, os); });

  addCommand<BasicCommand>("import")
      .addOption("input,i", bpo::value<std::string>(), "Input YAML file path", 1)
      .setExecutor([this](auto const& vm, auto& os) { return importLib(_ml, vm, os); });
}
