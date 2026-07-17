// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/library/LibraryYamlExporter.h>

#include <cstdint>

namespace ao::gtk::portal
{
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
