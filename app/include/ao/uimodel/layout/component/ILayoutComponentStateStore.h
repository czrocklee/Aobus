// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <optional>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutComponentStateDocument;
  struct LayoutDocument;

  /**
   * @brief Storage backend for per-preset layout component runtime state.
   */
  class ILayoutComponentStateStore
  {
  public:
    ILayoutComponentStateStore() = default;
    virtual ~ILayoutComponentStateStore() = default;

    ILayoutComponentStateStore(ILayoutComponentStateStore const&) = delete;
    ILayoutComponentStateStore& operator=(ILayoutComponentStateStore const&) = delete;
    ILayoutComponentStateStore(ILayoutComponentStateStore&&) = delete;
    ILayoutComponentStateStore& operator=(ILayoutComponentStateStore&&) = delete;

    virtual std::optional<LayoutComponentStateDocument> load(std::string_view presetId) const = 0;
    virtual void save(std::string_view presetId, LayoutComponentStateDocument const& doc) = 0;
    virtual bool prune(std::string_view presetId, LayoutDocument const& effectiveDoc) = 0;
    virtual bool removePreset(std::string_view presetId) = 0;
  };
} // namespace ao::uimodel
