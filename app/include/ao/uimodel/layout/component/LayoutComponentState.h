// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/document/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutDocument;
  class PreparedLayout;
  inline constexpr std::uint32_t kStateFileVersion = 1;
  inline constexpr std::uint32_t kStateEntryVersion = 1;

  struct LayoutComponentStateEntry final
  {
    std::string type{};
    std::uint32_t stateVersion = kStateEntryVersion;
    std::string baselineHash{};
    LayoutValueMap state{};
  };

  struct LayoutComponentStateDocument final
  {
    std::uint32_t version = kStateFileVersion;
    std::string preset{};
    std::map<std::string, LayoutComponentStateEntry, std::less<>> components{};
  };

  std::string componentBaselineHash(LayoutNode const& node);

  std::optional<LayoutComponentStateEntry> resolveComponentState(LayoutComponentStateDocument const& stateDoc,
                                                                 std::string_view componentId,
                                                                 std::string_view componentType,
                                                                 std::string_view baselineHash);

  std::optional<LayoutComponentStateEntry> resolveComponentState(LayoutComponentStateDocument const& stateDoc,
                                                                 LayoutNode const& node);

  void pruneComponentState(LayoutComponentStateDocument& stateDoc, PreparedLayout const& layout);
} // namespace ao::uimodel
