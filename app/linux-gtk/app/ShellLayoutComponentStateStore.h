// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutComponentStateDocument;
  struct LayoutDocument;
}

namespace ao::gtk
{
  class ShellLayoutComponentStateStore final : public uimodel::LayoutComponentStateStore
  {
  public:
    explicit ShellLayoutComponentStateStore(std::filesystem::path stateDir);
    ~ShellLayoutComponentStateStore() override = default;

    ShellLayoutComponentStateStore(ShellLayoutComponentStateStore const&) = delete;
    ShellLayoutComponentStateStore& operator=(ShellLayoutComponentStateStore const&) = delete;
    ShellLayoutComponentStateStore(ShellLayoutComponentStateStore&&) = delete;
    ShellLayoutComponentStateStore& operator=(ShellLayoutComponentStateStore&&) = delete;

    std::optional<uimodel::LayoutComponentStateDocument> load(std::string_view presetId) const override;
    void save(std::string_view presetId, uimodel::LayoutComponentStateDocument const& doc) override;
    bool prune(std::string_view presetId, uimodel::LayoutDocument const& effectiveDoc) override;
    bool removePreset(std::string_view presetId) override;

  private:
    std::filesystem::path filePath(std::string_view presetId) const;

    std::optional<uimodel::LayoutComponentStateDocument> loadUnlocked(std::string_view presetId) const;
    bool saveUnlocked(std::string_view presetId, uimodel::LayoutComponentStateDocument const& doc);
    bool removePresetUnlocked(std::string_view presetId);

    mutable std::mutex _mutex;
    std::filesystem::path _stateDir;
  };
} // namespace ao::gtk
