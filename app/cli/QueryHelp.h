// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>

namespace ao::cli
{
  std::string queryFilterUsageHint();
  std::string formatExpressionUsageHint();
  std::string trackShowHelpFooter();
  std::string trackUpdateHelpFooter();
  std::string trackHelpFooter();
  std::string listCreateHelpFooter();
} // namespace ao::cli
