// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace Gtk
{
  class ApplicationWindow;
}

namespace ao::gtk
{
  namespace portal
  {
    class ImportExportActions;
  }

  class WindowActionRegistry final
  {
  public:
    struct Callbacks final
    {
      std::function<void()> onEditLayout;
      std::function<void()> onResetRuntimeLayoutState;
      std::function<void()> onSaveCurrentPanelSizesAsLayoutDefaults;
    };

    WindowActionRegistry(portal::ImportExportActions& importExport, Callbacks callbacks);

    static constexpr char const* kOpenLibrary = "open-library";
    static constexpr char const* kScanLibrary = "scan-library";
    static constexpr char const* kImportLibrary = "import-library";
    static constexpr char const* kExportLibrary = "export-library";
    static constexpr char const* kEditLayout = "edit-layout";
    static constexpr char const* kSavePanelSizesAsLayoutDefaults = "save-panel-sizes-as-layout-defaults";
    static constexpr char const* kResetRuntimeLayoutState = "reset-runtime-layout-state";

    static std::string detailedWindowAction(std::string_view actionId);

    void install(Gtk::ApplicationWindow& window);

  private:
    portal::ImportExportActions& _importExport;
    Callbacks _callbacks;
  };
} // namespace ao::gtk
