// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellDocument.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocumentLoader.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/LayoutDialect.h>

#include <string_view>

namespace ao::winui
{
  std::string_view shellPresetId(ShellPreset const preset) noexcept
  {
    return preset == ShellPreset::Classic ? "windows.classic" : "windows.modern";
  }

  std::string_view shellPresetResource(ShellPreset const preset) noexcept
  {
    return preset == ShellPreset::Classic ? "windows_classic_layout.yaml" : "windows_modern_layout.yaml";
  }

  Result<uimodel::PreparedLayout> prepareShellPresetDocument(std::string_view const yaml,
                                                             std::string_view const sourceName)
  {
    return uimodel::prepareShellDocument(yaml, sourceName, layoutCatalog(), layoutActionCatalog(), layoutDialect());
  }
} // namespace ao::winui
