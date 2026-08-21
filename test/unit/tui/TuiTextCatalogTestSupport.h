// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "tui/TuiTextCatalog.h"

#include <string_view>

namespace ao::tui::test
{
  TuiTextCatalog const& englishTuiTextCatalog();
  TuiTextCatalog tuiTextCatalog(std::string_view locale);
} // namespace ao::tui::test
