// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ao::tui
{
  std::string selectionSummary(i18n::MessageCatalog const& textCatalog,
                               std::size_t trackCount,
                               std::int32_t selectedIndex,
                               std::size_t markedCount = 0);
  std::int32_t moveSelection(std::int32_t selectedIndex, std::int32_t delta, std::size_t itemCount);
  std::size_t clampSelection(std::size_t selection, std::size_t itemCount);
} // namespace ao::tui
