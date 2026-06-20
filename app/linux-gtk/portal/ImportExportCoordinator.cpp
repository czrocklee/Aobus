// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "app/ThemeCoordinator.h"
#include "layout/LayoutConstants.h"
#include "portal/LibraryTaskProgressDialog.h"
#include <ao/Exception.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/utility/Log.h>

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

#include <cstdint>
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
                                                   ImportExportCallbacks callbacks,
                                                   ThemeCoordinator& themeController)
    : _parent{parent}, _runtime{runtime}, _callbacks{std::move(callbacks)}, _themeController{themeController}
  {
  }

  ImportExportCoordinator::~ImportExportCoordinator() = default;

  void ImportExportCoordinator::openLibrary()
  {
    auto dialogPtr = Gtk::FileDialog::create();
    dialogPtr->set_title("Open Music Library");

    dialogPtr->select_folder(_parent,
                             [this, dialogPtr](Glib::RefPtr<Gio::AsyncResult>& result)
                             {
                               try
                               {
                                 if (auto const folderPtr = dialogPtr->select_folder_finish(result); folderPtr)
                                 {
                                   auto const path = std::filesystem::path{folderPtr->get_path()};

                                   if (auto const libPath = path / "data.mdb"; std::filesystem::exists(libPath))
                                   {
                                     openMusicLibrary(path);
                                   }
                                   else
                                   {
                                     // Initial scan for new library
                                     openMusicLibrary(path);
                                     scanLibrary();
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
    APP_LOG_INFO("Starting library scan...");

    _runtime.async().spawnWithLifetime(
      &_tasks,
      [](ImportExportCoordinator* self) -> async::Task<void>
      {
        // 1. Build Plan
        auto plan = co_await self->_runtime.library().tasks().buildScanPlanAsync();

        auto const newCount = plan.count(library::ScanClassification::New);
        auto const changedCount = plan.count(library::ScanClassification::Changed);
        auto const missingCount = plan.count(library::ScanClassification::Missing);

        if (newCount == 0 && changedCount == 0 && missingCount == 0)
        {
          self->_runtime.notifications().post(rt::NotificationSeverity::Info, "Library is up to date");
          co_return;
        }

        APP_LOG_INFO("Scan plan: {} new, {} changed, {} missing", newCount, changedCount, missingCount);

        if (self->_libraryTaskDialogPtr == nullptr)
        {
          self->_libraryTaskDialogPtr =
            std::make_unique<LibraryTaskProgressDialog>(static_cast<std::int32_t>(plan.items.size()), self->_parent);
          self->_optLibraryTaskThemeToken = self->_themeController.registerToplevel(*self->_libraryTaskDialogPtr);
        }

        auto* const dialog = self->_libraryTaskDialogPtr.get();
        self->_libraryTaskDialogPtr->signal_response().connect([dialog](std::int32_t /*responseId*/)
                                                               { dialog->close(); });

        self->_libraryTaskProgressSub = self->_runtime.library().changes().onLibraryTaskProgress(
          [dialog](auto const& ev) { dialog->updateProgress(ev.message, ev.fraction); });

        self->_libraryTaskDialogPtr->show();

        try
        {
          co_await self->_runtime.library().tasks().applyScanPlanAsync(std::move(plan));
          dialog->ready();
          self->onImportFinished();
        }
        catch (std::exception const& e)
        {
          APP_LOG_ERROR("Scan failed: {}", e.what());
          self->_runtime.notifications().post(rt::NotificationSeverity::Error, "Scan failed");
        }

        self->_libraryTaskProgressSub.reset();
      }(this));
  }

  void ImportExportCoordinator::onImportFinished() const
  {
    _runtime.notifications().post(rt::NotificationSeverity::Info, "Library scan complete");
  }

  void ImportExportCoordinator::openMusicLibrary(std::filesystem::path const& path) const
  {
    if (_callbacks.onOpenNewLibrary)
    {
      _callbacks.onOpenNewLibrary(path);
    }
  }

  void ImportExportCoordinator::exportLibrary()
  {
    auto* const dialog = Gtk::make_managed<AppDialog>();
    dialog->set_title("Select Export Mode");
    dialog->set_transient_for(_parent);

    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingMedium);

    auto* const label = Gtk::make_managed<Gtk::Label>("Choose what to include in the backup:");
    label->set_halign(Gtk::Align::START);
    box->append(*label);

    auto* modeCombo = Gtk::make_managed<Gtk::DropDown>();
    auto modeStringsPtr = Gtk::StringList::create({"Delta (Sync user edits, Tags, Lists)",
                                                   "Metadata (Curated text + Cover Art, no technical stats)",
                                                   "Full (Disaster Recovery: Everything)",
                                                   "List Only (Sync playlists without touching tracks)"});
    modeCombo->set_model(modeStringsPtr);
    modeCombo->set_selected(2); // Default to Full

    auto* list = Gtk::make_managed<FormBoxedList>();
    list->addRow("Include", *modeCombo);
    box->append(*list);

    dialog->setContentWidget(*box);
    dialog->addCancelAction("Cancel", Gtk::ResponseType::CANCEL);
    dialog->addPrimaryAction("Next", Gtk::ResponseType::OK);

    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeController.registerToplevel(*dialog));

    dialog->signal_response().connect([this, modeCombo, dialog, tokenPtr](std::int32_t responseId)
                                      { onExportModeConfirmed(responseId, modeCombo, dialog); });
    dialog->show();
  }

  void ImportExportCoordinator::onExportModeConfirmed(std::int32_t responseId,
                                                      Gtk::DropDown* modeCombo,
                                                      AppDialog* dialog)
  {
    if (responseId != Gtk::ResponseType::OK)
    {
      dialog->close();
      return;
    }

    int constexpr kDeltaIndex = 0;
    int constexpr kMetadataIndex = 1;
    int constexpr kFullIndex = 2;
    int constexpr kListOnlyIndex = 3;

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

    auto fileDialogPtr = Gtk::FileDialog::create();
    fileDialogPtr->set_title("Export Library to YAML");
    fileDialogPtr->set_initial_name("library_backup.yaml");

    auto filterPtr = Gtk::FileFilter::create();
    filterPtr->set_name("YAML files");
    filterPtr->add_pattern("*.yaml");
    filterPtr->add_pattern("*.yml");
    auto filtersPtr = Gio::ListStore<Gtk::FileFilter>::create();
    filtersPtr->append(filterPtr);
    fileDialogPtr->set_filters(filtersPtr);

    fileDialogPtr->save(_parent,
                        [this, mode, fileDialogPtr](Glib::RefPtr<Gio::AsyncResult>& result)
                        { onExportFileSelected(result, mode, fileDialogPtr); });
  }

  void ImportExportCoordinator::onExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                                                     rt::ExportMode mode,
                                                     Glib::RefPtr<Gtk::FileDialog> const& fileDialog)
  {
    try
    {
      if (auto const filePtr = fileDialog->save_finish(result); filePtr)
      {
        exportLibraryTo(filePtr->get_path(), mode);
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting export file: {}", e.what());
    }
  }

  void ImportExportCoordinator::exportLibraryTo(std::filesystem::path path, rt::ExportMode mode)
  {
    _runtime.async().spawnWithLifetime(
      &_tasks,
      [](
        ImportExportCoordinator* self, std::filesystem::path exportPath, rt::ExportMode exportMode) -> async::Task<void>
      {
        try
        {
          co_await self->_runtime.library().tasks().exportLibraryAsync(std::move(exportPath), exportMode);
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
      }(this, std::move(path), mode));
  }

  void ImportExportCoordinator::importLibrary()
  {
    auto fileDialogPtr = Gtk::FileDialog::create();
    fileDialogPtr->set_title("Import Library from YAML");

    auto filterPtr = Gtk::FileFilter::create();
    filterPtr->set_name("YAML files");
    filterPtr->add_pattern("*.yaml");
    filterPtr->add_pattern("*.yml");
    auto filtersPtr = Gio::ListStore<Gtk::FileFilter>::create();
    filtersPtr->append(filterPtr);
    fileDialogPtr->set_filters(filtersPtr);

    fileDialogPtr->open(_parent,
                        [this, fileDialogPtr](Glib::RefPtr<Gio::AsyncResult>& result)
                        { onLibraryImportSelected(result, fileDialogPtr); });
  }

  async::Task<void> ImportExportCoordinator::importLibraryTask(std::filesystem::path importPath)
  {
    try
    {
      co_await _runtime.library().tasks().importLibraryAsync(std::move(importPath));

      if (_callbacks.onLibraryDataMutated)
      {
        _callbacks.onLibraryDataMutated();
      }

      _runtime.notifications().post(rt::NotificationSeverity::Info, "Library imported successfully");
    }
    catch (ao::Exception const& e)
    {
      APP_LOG_CRITICAL("Import failed (internal error): {} (at {}:{})", e.what(), e.file(), e.line());
      _runtime.notifications().post(rt::NotificationSeverity::Error, "Import failed: Internal error");
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Import failed: {}", e.what());
      _runtime.notifications().post(rt::NotificationSeverity::Error, std::format("Import failed: {}", e.what()));
    }
  }

  void ImportExportCoordinator::importLibraryFrom(std::filesystem::path path)
  {
    _runtime.async().spawnWithLifetime(
      &_tasks,
      [](ImportExportCoordinator* self, std::filesystem::path importPath) -> async::Task<void>
      { co_await self->importLibraryTask(std::move(importPath)); }(this, std::move(path)));
  }

  void ImportExportCoordinator::onLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& result,
                                                        Glib::RefPtr<Gtk::FileDialog> const& dialog)
  {
    try
    {
      if (auto const filePtr = dialog->open_finish(result); filePtr)
      {
        importLibraryFrom(filePtr->get_path());
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting import file: {}", e.what());
    }
  }
} // namespace ao::gtk::portal
