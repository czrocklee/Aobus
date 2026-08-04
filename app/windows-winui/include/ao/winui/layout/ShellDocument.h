// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <cstdint>
#include <string_view>

namespace ao::winui
{
  /// Built-in Windows shell documents. Windows ships exactly these two presets.
  enum class ShellPreset : std::uint8_t
  {
    Modern,
    Classic,
  };

  /// Preset id stored with component runtime state.
  std::string_view shellPresetId(ShellPreset preset) noexcept;

  /// Resource name of a built-in preset document, relative to the Windows layout resource folder.
  std::string_view shellPresetResource(ShellPreset preset) noexcept;

  /**
   * @brief Folder the built-in preset documents are packaged into, beside the executable.
   *
   * Named here rather than in the frontend so the packaging step and the code
   * that reads it cannot drift apart without failing a test.
   */
  inline constexpr std::string_view kShellPresetFolder = "Assets/Layout";

  /**
   * @brief Turns built-in Windows shell YAML into a validated candidate.
   *
   * Parsing, template expansion, budget limits, and Windows catalog validation
   * are one step because a built-in document is one candidate: any defect
   * rejects it entirely rather than degrading part of the shell. @p sourceName
   * only labels diagnostics.
   */
  Result<uimodel::PreparedLayout> prepareShellPresetDocument(std::string_view yaml, std::string_view sourceName);
} // namespace ao::winui
