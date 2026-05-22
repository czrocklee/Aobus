// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "ao/Exception.h"
#include "ao/library/LibraryScanner.h"
#include "ao/utility/Log.h"
#include "layout/LayoutConstants.h"
#include "portal/ImportProgressDialog.h"
#include "runtime/AppRuntime.h"
#include "runtime/LibraryExporter.h"
#include "runtime/LibraryMutationService.h"
#include "runtime/NotificationService.h"
#include "runtime/StateTypes.h"
#include "runtime/async/LifetimeScope.h"
#include "runtime/async/Runtime.h"
#include "runtime/async/Task.h"

#include <giomm/asyncresult.h>
#include <giomm/liststore.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/enums.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <utility>
#include <vector>

namespace ao::gtk::portal
{
  ImportExportCoordinator::ImportExportCoordinator(Gtk::Window& parent,
                                                   rt::AppRuntime& runtime,
                                                   ImportExportCallbacks callbacks)
    : _parent{parent}, _runtime{runtime}, _callbacks{std::move(callbacks)}
  {
  }

  ImportExportCoordinator::~ImportExportCoordinator() = default;

  void ImportExportCoordinator::openLibrary()
  {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open Music Library");

    dialog->select_folder(_parent,
                          [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result)
                          {
                            try
                            {
                              if (auto const folder = dialog->select_folder_finish(result); folder)
                              {
                                auto const path = std::filesystem::path{folder->get_path()};

                                if (auto const libPath = path / "data.mdb"; std::filesystem::exists(libPath))
                                {
                                  openMusicLibrary(path);
                                }
                                else
                                {
                                  importFilesFromPath(path);
                                }
                              }
                            }
                            catch (Glib::Error const& e)
                            {
                              APP_LOG_ERROR("Error selecting folder: {}", e.what());
                            }
                          });
  }

  void ImportExportCoordinator::scanLibrary()
  {
    APP_LOG_INFO("Starting full library scan...");

    _runtime.async().spawnWithLifetime(
      &_tasks,
      [](ImportExportCoordinator* self) -> rt::async::Task<void>
      {
        auto plan = co_await self->_runtime.mutation().buildScanPlanAsync();

        auto const newCount = plan.count(library::ScanClassification::New);
        auto const changedCount = plan.count(library::ScanClassification::Changed);
        auto const missingCount = plan.count(library::ScanClassification::Missing);

        if (newCount == 0 && changedCount == 0 && missingCount == 0)
        {
          self->_runtime.notifications().post(rt::NotificationSeverity::Info, "Library is up to date");
          co_return;
        }

        APP_LOG_INFO("Scan plan: {} new, {} changed, {} missing", newCount, changedCount, missingCount);

        // For the first implementation, we apply NEW and CHANGED automatically.
        // MISSING is logged but not yet handled in UI.
        co_await self->_runtime.mutation().applyScanPlanAsync(std::move(plan));
      }(this));
  }

  void ImportExportCoordinator::importFiles()
  {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import Music Files");

    dialog->select_folder(
      _parent, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) { onImportFolderSelected(result, dialog); });
  }

  void ImportExportCoordinator::onImportFolderSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                                                       Glib::RefPtr<Gtk::FileDialog> const& dialog)
  {
    try
    {
      if (auto const folder = dialog->select_folder_finish(result); folder)
      {
        auto const pathStr = folder->get_path();
        auto const path = std::filesystem::path{pathStr};
        APP_LOG_INFO("Importing from: {}", pathStr);

        _runtime.async().spawnWithLifetime(
          &_tasks,
          [](
            ImportExportCoordinator* self, std::filesystem::path importPath, bool isNewLibrary) -> rt::async::Task<void>
          {
            auto files = co_await self->_runtime.mutation().scanLibraryAsync(importPath);

            if (files.empty())
            {
              self->_runtime.notifications().post(rt::NotificationSeverity::Info, "No music files found");
              co_return;
            }

            self->executeImportTask(files, isNewLibrary);
          }(this, path, false));
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting folder: {}", e.what());
    }
  }

  void ImportExportCoordinator::executeImportTask(std::vector<std::filesystem::path> const& files, bool isNewLibrary)
  {
    _importDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), _parent);
    auto* const dialogPtr = _importDialog.get();
    _importDialog->signal_response().connect([dialogPtr](int /*responseId*/) { dialogPtr->close(); });

    _importProgressSub = _runtime.mutation().onImportProgress(
      [dialogPtr, total = files.size()](auto const& ev)
      { dialogPtr->onNewTrack(ev.message, static_cast<int>(ev.fraction * static_cast<double>(total))); });

    _importDialog->show();

    _runtime.async().spawnWithLifetime(&_tasks,
                                       [](ImportExportCoordinator* self,
                                          std::vector<std::filesystem::path> paths,
                                          bool importToNewLibrary,
                                          ImportProgressDialog* dialog) -> rt::async::Task<void>
                                       {
                                         co_await self->_runtime.mutation().importFilesAsync(std::move(paths));

                                         dialog->ready();
                                         self->onImportFinished();

                                         if (importToNewLibrary && self->_callbacks.onLibraryDataMutated)
                                         {
                                           self->_callbacks.onLibraryDataMutated();
                                         }

                                         self->_importProgressSub.reset();
                                       }(this, files, isNewLibrary, dialogPtr));
  }

  void ImportExportCoordinator::onImportFinished() const
  {
    _runtime.notifications().post(rt::NotificationSeverity::Info, "Import complete");
  }

  void ImportExportCoordinator::openMusicLibrary(std::filesystem::path const& path) const
  {
    if (_callbacks.onOpenNewLibrary)
    {
      _callbacks.onOpenNewLibrary(path);
    }
  }

  void ImportExportCoordinator::importFilesFromPath(std::filesystem::path const& path)
  {
    if (_callbacks.onOpenNewLibrary)
    {
      _callbacks.onOpenNewLibrary(path);
    }

    _runtime.async().spawnWithLifetime(
      &_tasks,
      [](ImportExportCoordinator* self, std::filesystem::path importPath, bool isNewLibrary) -> rt::async::Task<void>
      {
        auto files = co_await self->_runtime.mutation().scanLibraryAsync(importPath);

        if (files.empty())
        {
          self->_runtime.notifications().post(rt::NotificationSeverity::Info, "No music files found");
          co_return;
        }

        self->executeImportTask(files, isNewLibrary);
      }(this, path, true));
  }

  void ImportExportCoordinator::exportLibrary()
  {
    auto* const dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Select Export Mode");
    dialog->set_transient_for(_parent);
    dialog->set_modal(true);

    auto* const contentArea = dialog->get_content_area();
    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingMedium);
    box->add_css_class("ao-dialog-content");

    auto* const label = Gtk::make_managed<Gtk::Label>("Choose what to include in the backup:");
    label->set_halign(Gtk::Align::START);
    box->append(*label);

    auto* modeCombo = Gtk::make_managed<Gtk::DropDown>();
    auto modeStrings = Gtk::StringList::create({"Delta (Sync user edits, Tags, Ratings, Lists)",
                                                "Metadata (Curated text + Cover Art, no technical stats)",
                                                "Full (Disaster Recovery: Everything)",
                                                "List Only (Sync playlists without touching tracks)"});
    modeCombo->set_model(modeStrings);
    modeCombo->set_selected(2); // Default to Full
    box->append(*modeCombo);

    contentArea->append(*box);
    dialog->add_button("Cancel", Gtk::ResponseType::CANCEL);
    dialog->add_button("Next", Gtk::ResponseType::OK);

    dialog->signal_response().connect([this, modeCombo, dialog](int responseId)
                                      { onExportModeConfirmed(responseId, modeCombo, dialog); });
    dialog->show();
  }

  void ImportExportCoordinator::onExportModeConfirmed(int responseId, Gtk::DropDown* modeCombo, Gtk::Dialog* dialog)
  {
    if (responseId != Gtk::ResponseType::OK)
    {
      dialog->close();
      return;
    }

    auto constexpr kDeltaIndex = 0;
    auto constexpr kMetadataIndex = 1;
    auto constexpr kFullIndex = 2;
    auto constexpr kListOnlyIndex = 3;

    auto mode = rt::ExportMode::Metadata;

    switch (modeCombo->get_selected())
    {
      case kDeltaIndex: mode = rt::ExportMode::Delta; break;
      case kMetadataIndex: mode = rt::ExportMode::Metadata; break;
      case kFullIndex: mode = rt::ExportMode::Full; break;
      case kListOnlyIndex: mode = rt::ExportMode::ListOnly; break;
      default: break;
    }

    dialog->close();

    auto fileDialog = Gtk::FileDialog::create();
    fileDialog->set_title("Export Library to YAML");
    fileDialog->set_initial_name("library_backup.yaml");

    auto filter = Gtk::FileFilter::create();
    filter->set_name("YAML files");
    filter->add_pattern("*.yaml");
    filter->add_pattern("*.yml");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    fileDialog->set_filters(filters);

    fileDialog->save(_parent,
                     [this, mode, fileDialog](Glib::RefPtr<Gio::AsyncResult>& result)
                     { onExportFileSelected(result, mode, fileDialog); });
  }

  void ImportExportCoordinator::onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                                                     rt::ExportMode mode,
                                                     Glib::RefPtr<Gtk::FileDialog> const& fileDialog)
  {
    try
    {
      if (auto const file = fileDialog->save_finish(result); file)
      {
        executeExportTask(file->get_path(), mode);
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting export file: {}", e.what());
    }
  }

  void ImportExportCoordinator::executeExportTask(std::filesystem::path const& path, rt::ExportMode mode)
  {
    _runtime.async().spawnWithLifetime(
      &_tasks,
      [](ImportExportCoordinator* self,
         std::filesystem::path exportPath,
         rt::ExportMode exportMode) -> rt::async::Task<void>
      {
        try
        {
          co_await self->_runtime.mutation().exportLibraryAsync(std::move(exportPath), exportMode);
          self->_runtime.notifications().post(rt::NotificationSeverity::Info, "Library exported successfully");
        }
        catch (ao::Exception const& e)
        {
          APP_LOG_CRITICAL("Export failed (internal error): {} (at {}:{})", e.what(), e.file(), e.line());
          self->_runtime.notifications().post(rt::NotificationSeverity::Error, "Export failed: Internal error");
        }
        catch (std::exception const& e)
        {
          APP_LOG_ERROR("Export failed: {}", e.what());
          self->_runtime.notifications().post(
            rt::NotificationSeverity::Error, std::format("Export failed: {}", e.what()));
        }
      }(this, path, mode));
  }

  void ImportExportCoordinator::importLibrary()
  {
    auto fileDialog = Gtk::FileDialog::create();
    fileDialog->set_title("Import Library from YAML");

    auto filter = Gtk::FileFilter::create();
    filter->set_name("YAML files");
    filter->add_pattern("*.yaml");
    filter->add_pattern("*.yml");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter);
    fileDialog->set_filters(filters);

    fileDialog->open(_parent,
                     [this, fileDialog](Glib::RefPtr<Gio::AsyncResult>& result)
                     { onLibraryImportSelected(result, fileDialog); });
  }

  void ImportExportCoordinator::onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                                                        Glib::RefPtr<Gtk::FileDialog> const& dialog)
  {
    try
    {
      if (auto const file = dialog->open_finish(result); file)
      {
        auto const path = std::filesystem::path{file->get_path()};

        _runtime.async().spawnWithLifetime(
          &_tasks,
          [](ImportExportCoordinator* self, std::filesystem::path importPath) -> rt::async::Task<void>
          {
            try
            {
              co_await self->_runtime.mutation().importLibraryAsync(std::move(importPath));

              if (self->_callbacks.onLibraryDataMutated)
              {
                self->_callbacks.onLibraryDataMutated();
              }

              self->_runtime.notifications().post(rt::NotificationSeverity::Info, "Library imported successfully");
            }
            catch (ao::Exception const& e)
            {
              APP_LOG_CRITICAL("Import failed (internal error): {} (at {}:{})", e.what(), e.file(), e.line());
              self->_runtime.notifications().post(rt::NotificationSeverity::Error, "Import failed: Internal error");
            }
            catch (std::exception const& e)
            {
              APP_LOG_ERROR("Import failed: {}", e.what());
              self->_runtime.notifications().post(
                rt::NotificationSeverity::Error, std::format("Import failed: {}", e.what()));
            }
          }(this, path));
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting import file: {}", e.what());
    }
  }
} // namespace ao::gtk::portal
