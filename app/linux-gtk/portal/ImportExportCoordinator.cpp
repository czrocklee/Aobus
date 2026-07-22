// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "app/ThemeCoordinator.h"
#include "layout/LayoutConstants.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/ImportExportCoordinatorPolicy.h"
#include "portal/LibraryImportExportWorkflow.h"
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>

#include <giomm/asyncresult.h>
#include <giomm/liststore.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/enums.h>
#include <gtkmm/error.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk::portal
{
  ImportExportCoordinator::ImportExportCoordinator(Gtk::Window& parent,
                                                   rt::AppRuntime& runtime,
                                                   ImportExportCallbacks callbacks,
                                                   ThemeCoordinator& themeCoordinator)
    : _parent{parent}
    , _callbacks{std::move(callbacks)}
    , _themeCoordinator{themeCoordinator}
    , _workflow{runtime, _callbacks}
    , _fileDialogCancellablePtr{Gio::Cancellable::create()}
    , _callbackScope{[cancellablePtr = _fileDialogCancellablePtr] { cancellablePtr->cancel(); }}
  {
    _callbacks.requestLibraryRestoreConfirmation =
      [this](rt::ImportReport const& report, std::function<void(bool)> completion)
    { presentLibraryRestoreConfirmation(report, std::move(completion)); };
  }

  void ImportExportCoordinator::openLibrary()
  {
    auto dialogPtr = Gtk::FileDialog::create();
    dialogPtr->set_title("Open Music Library");

    dialogPtr->select_folder(_parent,
                             _callbackScope.guard(
                               [this, dialogPtr](Glib::RefPtr<Gio::AsyncResult>& resultPtr)
                               {
                                 try
                                 {
                                   if (auto const folderPtr = dialogPtr->select_folder_finish(resultPtr); folderPtr)
                                   {
                                     auto const path = std::filesystem::path{folderPtr->get_path()};
                                     auto const libraryPaths = rt::LibraryPaths{path};

                                     openMusicLibrary(path, !libraryPaths.hasExistingDatabase());
                                   }
                                 }
                                 catch (Gtk::DialogError const& e)
                                 {
                                   if (!isExpectedNativeChooserCancellation(e.code()))
                                   {
                                     APP_LOG_ERROR("Error selecting folder: {}", e.what());
                                     presentFileDialogError("Could not select a music library folder", e.what());
                                   }
                                 }
                                 catch (Glib::Error const& e)
                                 {
                                   APP_LOG_ERROR("Error selecting folder: {}", e.what());
                                   presentFileDialogError("Could not select a music library folder", e.what());
                                 }
                               }),
                             _fileDialogCancellablePtr);
  }

  void ImportExportCoordinator::scanLibrary()
  {
    scanLibrary(ScanRequestMode::Eager);
  }

  void ImportExportCoordinator::scanLibrary(ScanRequestMode mode)
  {
    _workflow.scan(mode);
  }

  void ImportExportCoordinator::openMusicLibrary(std::filesystem::path const& path, bool const scanAfterOpen) const
  {
    if (_callbacks.onOpenNewLibrary)
    {
      _callbacks.onOpenNewLibrary(path, scanAfterOpen);
    }
  }

  void ImportExportCoordinator::exportLibrary()
  {
    auto* const dialog = Gtk::make_managed<AppDialog>();
    dialog->set_title("Select Export Mode");
    dialog->configureForParent(_parent);

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

    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));

    dialog->signal_response().connect(
      _callbackScope.guard([this, modeCombo, dialog, tokenPtr](std::int32_t responseId)
                           { handleExportModeConfirmed(responseId, modeCombo, dialog); }));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
    dialog->present();
  }

  void ImportExportCoordinator::handleExportModeConfirmed(std::int32_t responseId,
                                                          Gtk::DropDown* modeCombo,
                                                          AppDialog* dialog)
  {
    if (responseId != Gtk::ResponseType::OK)
    {
      dialog->close();
      return;
    }

    auto const mode = exportModeForSelection(modeCombo->get_selected());

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
                        _callbackScope.guard([this, mode, fileDialogPtr](Glib::RefPtr<Gio::AsyncResult>& resultPtr)
                                             { handleExportFileSelected(resultPtr, mode, fileDialogPtr); }),
                        _fileDialogCancellablePtr);
  }

  void ImportExportCoordinator::handleExportFileSelected(Glib::RefPtr<Gio::AsyncResult>& resultPtr,
                                                         rt::ExportMode mode,
                                                         Glib::RefPtr<Gtk::FileDialog> const& fileDialogPtr)
  {
    try
    {
      if (auto const filePtr = fileDialogPtr->save_finish(resultPtr); filePtr)
      {
        exportLibraryTo(filePtr->get_path(), mode);
      }
    }
    catch (Gtk::DialogError const& e)
    {
      if (!isExpectedNativeChooserCancellation(e.code()))
      {
        APP_LOG_ERROR("Error selecting export file: {}", e.what());
        presentFileDialogError("Could not select an export file", e.what());
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting export file: {}", e.what());
      presentFileDialogError("Could not select an export file", e.what());
    }
  }

  void ImportExportCoordinator::exportLibraryTo(std::filesystem::path path, rt::ExportMode mode)
  {
    _workflow.exportTo(std::move(path), mode);
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
                        _callbackScope.guard([this, fileDialogPtr](Glib::RefPtr<Gio::AsyncResult>& resultPtr)
                                             { handleLibraryImportSelected(resultPtr, fileDialogPtr); }),
                        _fileDialogCancellablePtr);
  }

  void ImportExportCoordinator::importLibraryFrom(std::filesystem::path path)
  {
    _workflow.importFrom(std::move(path));
  }

  void ImportExportCoordinator::presentLibraryRestoreConfirmation(rt::ImportReport const& report,
                                                                  std::function<void(bool)> completion)
  {
    auto const* const scopeName =
      report.targetScope == rt::ImportTargetScope::Library ? "library tracks and lists" : "lists";
    auto const* const actionLabel =
      report.targetScope == rt::ImportTargetScope::Library ? "Restore Library" : "Restore Lists";
    auto const message = std::format("This restore will replace the current {}.\n\n"
                                     "Payload: YAML v{}, mode '{}'.\n"
                                     "Preview: {} created, {} updated, {} deleted; {} lists created, {} deleted; "
                                     "{} dangling references ignored.\n\n"
                                     "Continue only if this matches the selected backup.",
                                     scopeName,
                                     report.payloadVersion,
                                     rt::exportModeName(report.payloadMode),
                                     uimodel::formatTrackCount(report.tracksCreated),
                                     uimodel::formatTrackCount(report.tracksUpdated),
                                     uimodel::formatTrackCount(report.tracksDeleted),
                                     report.listsCreated,
                                     report.listsDeleted,
                                     report.danglingReferencesIgnored);

    auto* const dialog = AppDialog::presentMessage(
      _parent,
      "Confirm Library Restore",
      message,
      {AppDialogAction{.label = "Cancel", .responseId = Gtk::ResponseType::CANCEL, .role = AppDialogActionRole::Cancel},
       AppDialogAction{
         .label = actionLabel, .responseId = Gtk::ResponseType::OK, .role = AppDialogActionRole::Primary}},
      Gtk::ResponseType::CANCEL,
      _callbackScope.guard([completion = std::move(completion)](std::int32_t const responseId)
                           { completion(responseId == Gtk::ResponseType::OK); }));
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
  }

  void ImportExportCoordinator::handleLibraryImportSelected(Glib::RefPtr<Gio::AsyncResult>& resultPtr,
                                                            Glib::RefPtr<Gtk::FileDialog> const& dialogPtr)
  {
    try
    {
      if (auto const filePtr = dialogPtr->open_finish(resultPtr); filePtr)
      {
        importLibraryFrom(filePtr->get_path());
      }
    }
    catch (Gtk::DialogError const& e)
    {
      if (!isExpectedNativeChooserCancellation(e.code()))
      {
        APP_LOG_ERROR("Error selecting import file: {}", e.what());
        presentFileDialogError("Could not select a library backup", e.what());
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting import file: {}", e.what());
      presentFileDialogError("Could not select a library backup", e.what());
    }
  }

  void ImportExportCoordinator::presentFileDialogError(std::string_view operation, std::string_view message)
  {
    auto* const dialog = AppDialog::presentMessage(
      _parent,
      "File Selection Failed",
      std::format("{}: {}", operation, message),
      {AppDialogAction{.label = "Close", .responseId = Gtk::ResponseType::CLOSE, .role = AppDialogActionRole::Cancel}},
      Gtk::ResponseType::CLOSE);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
  }
} // namespace ao::gtk::portal
