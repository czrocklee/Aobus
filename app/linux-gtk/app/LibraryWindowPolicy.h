// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::gtk
{
  enum class LibraryWindowKind : std::uint8_t
  {
    Library,
    FallbackEmptyLibrary,
  };

  enum class OpenLibraryWindowMode : std::uint8_t
  {
    AddWindow,
    ReplaceSourceWindow,
  };

  constexpr OpenLibraryWindowMode openLibraryWindowModeFor(LibraryWindowKind const kind) noexcept
  {
    return kind == LibraryWindowKind::FallbackEmptyLibrary ? OpenLibraryWindowMode::ReplaceSourceWindow
                                                           : OpenLibraryWindowMode::AddWindow;
  }
} // namespace ao::gtk
