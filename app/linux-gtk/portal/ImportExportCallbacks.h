// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace ao::gtk::portal
{
  /**
   * ImportExportCallbacks defines the notifications from the import/export layer to its host.
   */
  struct ImportExportCallbacks final
  {
    std::function<void(std::filesystem::path const&, bool scanAfterOpen)> onOpenNewLibrary;
    std::function<void()> onLibraryDataMutated;
    std::function<void(std::string const&)> onTitleChanged;
  };
} // namespace ao::gtk::portal
