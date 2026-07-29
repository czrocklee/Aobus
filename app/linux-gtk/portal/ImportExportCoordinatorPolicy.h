// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/library/LibraryYamlExporter.h>

#include <gtkmm/error.h>

#include <cstdint>

namespace ao::gtk::portal
{
  inline bool isExpectedNativeChooserCancellation(Gtk::DialogError::Code code) noexcept
  {
    return code == Gtk::DialogError::CANCELLED || code == Gtk::DialogError::DISMISSED;
  }

  rt::ExportMode exportModeForSelection(std::uint32_t selectedIndex);
} // namespace ao::gtk::portal
