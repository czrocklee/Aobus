// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/MainWindow.h"
#include <ao/Error.h>

#include <glibmm/refptr.h>

#include <filesystem>
#include <memory>

namespace Gtk
{
  class Application;
}

namespace ao::uimodel
{
  class PresentationTextCatalog;
}

namespace ao::rt
{
  class TextOrderingPolicy;
}

namespace ao::gtk
{
  class AppConfigStore;
  class GtkTextCatalog;
  class ShellLayoutComponentStateStore;
  class ShellLayoutStore;

  struct LibraryWindowPaths final
  {
    std::filesystem::path musicRoot;
    std::filesystem::path databasePath;
  };

  Result<Glib::RefPtr<MainWindow>> prepareLibraryWindow(
    LibraryWindowPaths paths,
    std::shared_ptr<AppConfigStore> appConfigStorePtr,
    std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
    uimodel::PresentationTextCatalog const& textCatalog,
    GtkTextCatalog const& gtkTextCatalog,
    rt::TextOrderingPolicy const* textOrderingPolicy = nullptr);

  Result<> activateLibraryWindow(Gtk::Application& app,
                                 Glib::RefPtr<MainWindow> const& windowPtr,
                                 MainWindow::PlaybackRestoreMode restoreMode);
} // namespace ao::gtk
