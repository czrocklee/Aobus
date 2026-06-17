// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/document/LayoutDocument.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/state/ILayoutComponentStateStore.h"
#include "layout/state/LayoutComponentState.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

namespace ao::gtk
{
  class ShellLayoutComponentStateStore final : public layout::ILayoutComponentStateStore
  {
  public:
    explicit ShellLayoutComponentStateStore(std::filesystem::path stateDir);
    ~ShellLayoutComponentStateStore() override = default;

    ShellLayoutComponentStateStore(ShellLayoutComponentStateStore const&) = delete;
    ShellLayoutComponentStateStore& operator=(ShellLayoutComponentStateStore const&) = delete;
    ShellLayoutComponentStateStore(ShellLayoutComponentStateStore&&) = delete;
    ShellLayoutComponentStateStore& operator=(ShellLayoutComponentStateStore&&) = delete;

    std::optional<layout::LayoutComponentStateDocument> load(std::string_view presetId) const override;
    void save(layout::LayoutComponentStateDocument const& doc, std::string_view presetId) override;
    bool prune(std::string_view presetId, layout::LayoutDocument const& effectiveDoc) override;
    bool removePreset(std::string_view presetId) override;

  private:
    std::filesystem::path filePath(std::string_view presetId) const;

    std::optional<layout::LayoutComponentStateDocument> loadUnlocked(std::string_view presetId) const;
    bool saveUnlocked(layout::LayoutComponentStateDocument const& doc, std::string_view presetId);
    bool removePresetUnlocked(std::string_view presetId);

    mutable std::mutex _mutex;
    std::filesystem::path _stateDir;
  };
} // namespace ao::gtk
