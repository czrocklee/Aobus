// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace ao::rt
{
  struct ImportReport;
}

namespace ao::gtk::portal
{
  /**
   * ImportExportCallbacks defines the notifications from the import/export layer to its host.
   */
  struct ImportExportCallbacks final
  {
    std::function<void(std::filesystem::path const&, bool scanAfterOpen)> onOpenNewLibrary = {};
    std::function<void()> onLibraryDataMutated = {};
    std::function<void(std::string const&)> onTitleChanged = {};
    // The host must invoke this completion on the same GTK main context that
    // requested confirmation; the guarded callback accesses workflow state.
    std::function<void(rt::ImportReport const&, std::function<void(bool)>)> requestLibraryRestoreConfirmation = {};
  };
} // namespace ao::gtk::portal
