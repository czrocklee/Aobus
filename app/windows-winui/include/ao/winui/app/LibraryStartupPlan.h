// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/app/StartupOptions.h>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace ao::winui
{
  enum class LibraryStartupRootSource : std::uint8_t
  {
    Explicit,
    Persisted,
    EmptyLibraryFallback,
  };

  /**
   * @brief The selected root a successor may commit after activation.
   *
   * Planning only carries this value. It does not modify DesktopSettings; the
   * caller applies it after the window and process-wide adapters are active.
   */
  struct SelectedRootCommit final
  {
    std::filesystem::path root;

    void apply(DesktopSettings& settings) const;

    friend bool operator==(SelectedRootCommit const&, SelectedRootCommit const&) = default;
  };

  struct LibraryStartupPlan final
  {
    std::filesystem::path libraryRoot;
    LibraryStartupRootSource source = LibraryStartupRootSource::EmptyLibraryFallback;
    std::optional<SelectedRootCommit> optSelectedRootCommit{};

    friend bool operator==(LibraryStartupPlan const&, LibraryStartupPlan const&) = default;
  };

  /**
   * @brief Selects the root for one successor process without changing settings.
   *
   * An explicit root is strict: it must resolve to an accessible directory.
   * A persisted root is best effort and falls back to @p emptyLibraryRoot when
   * it is missing, inaccessible, or malformed. Only an explicit root produces a
   * pending selected-root commit.
   */
  Result<LibraryStartupPlan> planLibraryStartup(StartupOptions const& options,
                                                DesktopSettings const& settings,
                                                std::filesystem::path emptyLibraryRoot);

  /// Applies the pending explicit-root commit, if the plan contains one.
  void commitSelectedRoot(LibraryStartupPlan const& plan, DesktopSettings& settings);
} // namespace ao::winui
