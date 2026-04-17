// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "LibCommand.h"
#include "BasicCommand.h"

#include <iomanip>
#include <sstream>

namespace
{
  namespace bpo = boost::program_options;
  using namespace rs;

  std::string formatUuid(std::array<std::byte, 16> const& id)
  {
    std::ostringstream oss;
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
    std::ostringstream oss;
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
}

LibCommand::LibCommand(core::MusicLibrary& ml) : _ml{ml}
{
  addCommand<BasicCommand>("show").setExecutor(
    [this]([[maybe_unused]] auto const& vm, auto& os) { return show(_ml, os); });
}
