// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::layout
{
  struct LayoutDocument;
  inline constexpr std::uint32_t kLayoutComponentStateFileVersion = 1;
  inline constexpr std::uint32_t kLayoutComponentStateEntryVersion = 1;

  struct LayoutComponentStateEntry final
  {
    std::string type{};
    std::uint32_t stateVersion = kLayoutComponentStateEntryVersion;
    std::string baselineHash{};
    std::map<std::string, LayoutValue, std::less<>> state{};
  };

  struct LayoutComponentStateDocument final
  {
    std::uint32_t version = kLayoutComponentStateFileVersion;
    std::string preset{};
    std::map<std::string, LayoutComponentStateEntry, std::less<>> components{};
  };

  std::string layoutComponentBaselineHash(LayoutNode const& node);

  std::optional<LayoutComponentStateEntry> resolveLayoutComponentState(LayoutComponentStateDocument const& stateDoc,
                                                                       std::string_view componentId,
                                                                       std::string_view componentType,
                                                                       std::string_view baselineHash);

  std::optional<LayoutComponentStateEntry> resolveLayoutComponentState(LayoutComponentStateDocument const& stateDoc,
                                                                       LayoutNode const& node);

  void pruneLayoutComponentState(LayoutComponentStateDocument& stateDoc, LayoutDocument const& effectiveDoc);
} // namespace ao::uimodel::layout
