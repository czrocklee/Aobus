// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace ao::uimodel::layout
{
  struct LayoutDocument;
}

namespace ao::gtk
{
  /**
   * @brief Persistence for customized shell layouts.
   *
   * Exclusively owns the layouts/ directory next to the global config.
   * One file per preset: layouts/<presetId>.yaml. Absence of a file means
   * "not customized" — callers fall back to the built-in preset.
   */
  class ShellLayoutStore final
  {
  public:
    explicit ShellLayoutStore(std::filesystem::path layoutsDir);
    ~ShellLayoutStore();

    ShellLayoutStore(ShellLayoutStore const&) = delete;
    ShellLayoutStore& operator=(ShellLayoutStore const&) = delete;
    ShellLayoutStore(ShellLayoutStore&&) noexcept;
    ShellLayoutStore& operator=(ShellLayoutStore&&) noexcept;

    std::optional<uimodel::layout::LayoutDocument> load(std::string_view presetId) const;
    void save(uimodel::layout::LayoutDocument const& doc, std::string_view presetId);
    void remove(std::string_view presetId);

  private:
    std::filesystem::path filePath(std::string_view presetId) const;

    std::filesystem::path _layoutsDir;
  };
} // namespace ao::gtk
