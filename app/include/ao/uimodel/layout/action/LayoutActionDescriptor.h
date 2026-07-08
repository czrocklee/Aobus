// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>

#include <string>

namespace ao::uimodel
{
  struct LayoutActionDescriptor final
  {
    std::string id;
    std::string label;
    std::string category;
    LayoutActionCapabilities capabilities = LayoutActionCapability::None;
  };
} // namespace ao::uimodel
