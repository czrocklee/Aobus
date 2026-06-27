// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::gtk::portal
{
  class ImportExportActions
  {
  public:
    ImportExportActions() = default;
    ImportExportActions(ImportExportActions const&) = delete;
    ImportExportActions& operator=(ImportExportActions const&) = delete;
    ImportExportActions(ImportExportActions&&) = delete;
    ImportExportActions& operator=(ImportExportActions&&) = delete;
    virtual ~ImportExportActions() = default;

    virtual void openLibrary() = 0;
    virtual void scanLibrary() = 0;
    virtual void importLibrary() = 0;
    virtual void exportLibrary() = 0;
  };
} // namespace ao::gtk::portal
