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
  class LayoutComponentStateStore
  {
  public:
    LayoutComponentStateStore() = default;
    virtual ~LayoutComponentStateStore() = default;

    LayoutComponentStateStore(LayoutComponentStateStore const&) = delete;
    LayoutComponentStateStore& operator=(LayoutComponentStateStore const&) = delete;
    LayoutComponentStateStore(LayoutComponentStateStore&&) = delete;
    LayoutComponentStateStore& operator=(LayoutComponentStateStore&&) = delete;

    virtual std::optional<LayoutComponentStateDocument> load(std::string_view presetId) const = 0;
    virtual void save(std::string_view presetId, LayoutComponentStateDocument const& doc) = 0;
    virtual bool prune(std::string_view presetId, LayoutDocument const& effectiveDoc) = 0;
    virtual bool removePreset(std::string_view presetId) = 0;
  };
} // namespace ao::uimodel
