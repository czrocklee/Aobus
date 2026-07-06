// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/library/LibraryYamlExporter.h>

#include <cstdint>
#include <filesystem>

namespace ao::gtk::portal
{
  inline std::filesystem::path defaultLibraryDatabasePath(std::filesystem::path const& libraryPath)
  {
    return libraryPath / ".aobus" / "library";
  }

  inline bool shouldScanAfterOpen(std::filesystem::path const& libraryPath)
  {
    return !std::filesystem::exists(defaultLibraryDatabasePath(libraryPath) / "data.mdb");
  }

  inline rt::ExportMode exportModeForSelection(std::uint32_t selectedIndex)
  {
    switch (selectedIndex)
    {
      case 0U: return rt::ExportMode::Delta;
      case 1U: return rt::ExportMode::Metadata;
      case 2U: return rt::ExportMode::Full;
      case 3U: return rt::ExportMode::ListOnly;
      default: return rt::ExportMode::Metadata;
    }
  }
} // namespace ao::gtk::portal
