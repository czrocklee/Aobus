// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <string>
#include <vector>

namespace ao::uimodel
{
  class LayoutActionCatalog;
  struct LayoutDocument;

  struct LayoutDiagnostic final
  {
    std::string componentId;
    std::string propertyName;
    std::string actionId;
    std::string message;
  };

  std::vector<LayoutDiagnostic> validateLayoutActions(LayoutDocument const& doc,
                                                      LayoutComponentCatalog const& components,
                                                      LayoutActionCatalog const& actions);
} // namespace ao::uimodel
