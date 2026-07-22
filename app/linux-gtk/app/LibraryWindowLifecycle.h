// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/MainWindow.h"
#include <ao/Error.h>

#include <glibmm/refptr.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace Gtk
{
  class Application;
}

namespace ao::gtk
{
  class AppConfigStore;
  class ShellLayoutComponentStateStore;
  class ShellLayoutStore;

  struct LibraryWindowPaths final
  {
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
  };

  Glib::RefPtr<MainWindow> prepareLibraryWindow(LibraryWindowPaths paths,
                                                std::shared_ptr<AppConfigStore> appConfigStorePtr,
                                                std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                                                std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr);

  void activateLibraryWindow(Gtk::Application& app,
                             Glib::RefPtr<MainWindow> const& windowPtr,
                             MainWindow::PlaybackRestoreMode restoreMode);

  enum class LibraryWindowOpenOutcome : std::uint8_t
  {
    Reused,
    Replaced,
  };

  struct LibraryWindowReplacementCallbacks final
  {
    std::function<void()> prepareCandidate;
    std::function<void()> configureCandidate;
    std::function<Result<>()> retireActive;
    std::function<void()> activateCandidate;
    std::function<void()> replaceActiveSlot;
    std::function<void()> releaseRetired;
    std::function<void()> persistSelectedPath;
    std::function<void()> scanActive;
    std::function<void()> presentActive;
  };

  Result<LibraryWindowOpenOutcome> openLibraryWindow(std::filesystem::path const& activeRoot,
                                                     std::filesystem::path const& requestedRoot,
                                                     bool scanAfterOpen,
                                                     LibraryWindowReplacementCallbacks const& callbacks);
} // namespace ao::gtk
