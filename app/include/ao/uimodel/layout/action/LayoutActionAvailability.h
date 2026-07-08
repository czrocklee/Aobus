// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>

namespace ao::uimodel
{
  struct LayoutActionAvailability final
  {
    bool enabled = true;
    std::string disabledReason;
  };
} // namespace ao::uimodel
