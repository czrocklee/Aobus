// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/MainWindow.h"
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>

#include <glibmm/refptr.h>

#include <filesystem>
#include <memory>

namespace Gtk
{
  class Application;
}

namespace ao::rt
{
  class CompletionAliasPolicy;
  class TextOrderingPolicy;
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

  // The returned shared handle owns the C++ window and keeps its runtime alive
  // through wrapper destruction; a native GObject reference is not that owner.
  Result<Glib::RefPtr<MainWindow>> prepareLibraryWindow(
    LibraryWindowPaths paths,
    std::shared_ptr<AppConfigStore> appConfigStorePtr,
    std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
    i18n::MessageCatalog const& textCatalog,
    rt::TextOrderingPolicy const* textOrderingPolicy = nullptr,
    rt::CompletionAliasPolicy const* completionAliasPolicy = nullptr);

  Result<> activateLibraryWindow(Gtk::Application& app,
                                 Glib::RefPtr<MainWindow> const& windowPtr,
                                 MainWindow::PlaybackRestoreMode restoreMode);
} // namespace ao::gtk
