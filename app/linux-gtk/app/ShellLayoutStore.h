// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <filesystem>
#include <optional>
#include <string_view>

namespace ao::uimodel
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
   * Existing rejected files are preserved instead of being rewritten.
   */
  class ShellLayoutStore final
  {
  public:
    explicit ShellLayoutStore(std::filesystem::path layoutsDir,
                              uimodel::LayoutDocumentLimits limits = uimodel::LayoutDocumentLimits{});

    /**
     * @brief A store for a session with no configuration directory to own.
     *
     * Every preset reads as not customized and every write keeps nothing, so
     * the shell runs on its built-in presets. Without this the empty directory
     * would resolve preset files relative to whatever directory the process was
     * launched from, which is a worse answer than keeping nothing.
     */
    explicit ShellLayoutStore(rt::ConfigStore::NoLocation /*noLocation*/,
                              uimodel::LayoutDocumentLimits limits = uimodel::LayoutDocumentLimits{});
    ~ShellLayoutStore();

    ShellLayoutStore(ShellLayoutStore const&) = delete;
    ShellLayoutStore& operator=(ShellLayoutStore const&) = delete;
    ShellLayoutStore(ShellLayoutStore&&) noexcept;
    ShellLayoutStore& operator=(ShellLayoutStore&&) noexcept;

    Result<std::optional<uimodel::LayoutDocument>> load(std::string_view presetId) const;
    Result<> save(uimodel::LayoutDocument const& doc, std::string_view presetId);
    Result<> remove(std::string_view presetId);

    uimodel::LayoutDocumentLimits const& limits() const noexcept { return _limits; }

  private:
    std::filesystem::path filePath(std::string_view presetId) const;

    std::filesystem::path _layoutsDir;
    uimodel::LayoutDocumentLimits _limits;
    bool _hasLocation = true;
  };
} // namespace ao::gtk
