// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/ImportExportCoordinator.h"

#include "app/AppDialog.h"
#include "app/FormBuilder.h"
#include "app/ThemeCoordinator.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/LayoutConstants.h"
#include "portal/ImportExportCallbacks.h"
#include "portal/ImportExportCoordinatorPolicy.h"
#include "portal/LibraryImportExportWorkflow.h"
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/utility/Path.h>

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
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace ao::gtk::portal
{
  ImportExportCoordinator::ImportExportCoordinator(Gtk::Window& parent,
                                                   rt::AppRuntime& runtime,
                                                   uimodel::PresentationTextCatalog const& textCatalog,
                                                   GtkTextCatalog gtkTextCatalog,
                                                   ImportExportCallbacks callbacks,
                                                   ThemeCoordinator& themeCoordinator)
    : _parent{parent}
    , _callbacks{std::move(callbacks)}
    , _themeCoordinator{themeCoordinator}
    , _gtkTextCatalog{std::move(gtkTextCatalog)}
    , _workflow{runtime, _callbacks, textCatalog}
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
    dialogPtr->set_title(_gtkTextCatalog.text(GtkTextId::LibraryOpenMusicLibrary));

    dialogPtr->select_folder(
      _parent,
      _callbackScope.guard(
        [this, dialogPtr](Glib::RefPtr<Gio::AsyncResult>& resultPtr)
        {
          try
          {
            if (auto const folderPtr = dialogPtr->select_folder_finish(resultPtr); folderPtr)
            {
              auto const path = utility::pathFromNative(folderPtr->get_path());
              auto const libraryPaths = rt::LibraryPaths{path};

              openMusicLibrary(path, !libraryPaths.hasExistingDatabase());
            }
          }
          catch (Gtk::DialogError const& e)
          {
            if (!isExpectedNativeChooserCancellation(e.code()))
            {
              APP_LOG_ERROR("Error selecting folder: {}", e.what());
              presentFileDialogError(_gtkTextCatalog.text(GtkTextId::LibraryCouldNotSelectMusicFolder), e.what());
            }
          }
          catch (Glib::Error const& e)
          {
            APP_LOG_ERROR("Error selecting folder: {}", e.what());
            presentFileDialogError(_gtkTextCatalog.text(GtkTextId::LibraryCouldNotSelectMusicFolder), e.what());
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
    dialog->set_title(_gtkTextCatalog.text(GtkTextId::LibrarySelectExportMode));
    dialog->configureForParent(_parent);

    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingMedium);

    auto* const label = Gtk::make_managed<Gtk::Label>(_gtkTextCatalog.text(GtkTextId::LibraryChooseBackupContents));
    label->set_halign(Gtk::Align::START);
    box->append(*label);

    auto* modeCombo = Gtk::make_managed<Gtk::DropDown>();
    auto modeStringsPtr = Gtk::StringList::create({_gtkTextCatalog.text(GtkTextId::LibraryExportModeDelta),
                                                   _gtkTextCatalog.text(GtkTextId::LibraryExportModeMetadata),
                                                   _gtkTextCatalog.text(GtkTextId::LibraryExportModeFull),
                                                   _gtkTextCatalog.text(GtkTextId::LibraryExportModeListOnly)});
    modeCombo->set_model(modeStringsPtr);
    modeCombo->set_selected(2); // Default to Full

    auto* list = Gtk::make_managed<FormBoxedList>();
    list->addRow(_gtkTextCatalog.text(GtkTextId::LibraryInclude), *modeCombo);
    box->append(*list);

    dialog->setContentWidget(*box);
    dialog->addCancelAction(_gtkTextCatalog.text(GtkTextId::CommonCancel), Gtk::ResponseType::CANCEL);
    dialog->addPrimaryAction(_gtkTextCatalog.text(GtkTextId::LibraryNext), Gtk::ResponseType::OK);

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
    fileDialogPtr->set_title(_gtkTextCatalog.text(GtkTextId::LibraryExportYaml));
    fileDialogPtr->set_initial_name("library_backup.yaml");

    auto filterPtr = Gtk::FileFilter::create();
    filterPtr->set_name(_gtkTextCatalog.text(GtkTextId::LibraryYamlFiles));
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
        exportLibraryTo(utility::pathFromNative(filePtr->get_path()), mode);
      }
    }
    catch (Gtk::DialogError const& e)
    {
      if (!isExpectedNativeChooserCancellation(e.code()))
      {
        APP_LOG_ERROR("Error selecting export file: {}", e.what());
        presentFileDialogError(_gtkTextCatalog.text(GtkTextId::LibraryCouldNotSelectExportFile), e.what());
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting export file: {}", e.what());
      presentFileDialogError(_gtkTextCatalog.text(GtkTextId::LibraryCouldNotSelectExportFile), e.what());
    }
  }

  void ImportExportCoordinator::exportLibraryTo(std::filesystem::path path, rt::ExportMode mode)
  {
    _workflow.exportTo(std::move(path), mode);
  }

  void ImportExportCoordinator::importLibrary()
  {
    auto fileDialogPtr = Gtk::FileDialog::create();
    fileDialogPtr->set_title(_gtkTextCatalog.text(GtkTextId::LibraryImportYaml));

    auto filterPtr = Gtk::FileFilter::create();
    filterPtr->set_name(_gtkTextCatalog.text(GtkTextId::LibraryYamlFiles));
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
    auto const actionId = report.targetScope == rt::ImportTargetScope::Library ? GtkTextId::LibraryRestoreLibrary
                                                                               : GtkTextId::LibraryRestoreLists;
    auto const message = _gtkTextCatalog.libraryRestoreConfirmation(report);

    auto* const dialog =
      AppDialog::presentMessage(_parent,
                                _gtkTextCatalog.text(GtkTextId::LibraryConfirmRestore),
                                message,
                                {AppDialogAction{.label = std::string{_gtkTextCatalog.text(GtkTextId::CommonCancel)},
                                                 .responseId = Gtk::ResponseType::CANCEL,
                                                 .role = AppDialogActionRole::Cancel},
                                 AppDialogAction{.label = std::string{_gtkTextCatalog.text(actionId)},
                                                 .responseId = Gtk::ResponseType::OK,
                                                 .role = AppDialogActionRole::Primary}},
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
        importLibraryFrom(utility::pathFromNative(filePtr->get_path()));
      }
    }
    catch (Gtk::DialogError const& e)
    {
      if (!isExpectedNativeChooserCancellation(e.code()))
      {
        APP_LOG_ERROR("Error selecting import file: {}", e.what());
        presentFileDialogError(_gtkTextCatalog.text(GtkTextId::LibraryCouldNotSelectBackup), e.what());
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting import file: {}", e.what());
      presentFileDialogError(_gtkTextCatalog.text(GtkTextId::LibraryCouldNotSelectBackup), e.what());
    }
  }

  void ImportExportCoordinator::presentFileDialogError(std::string_view operation, std::string_view message)
  {
    auto* const dialog =
      AppDialog::presentMessage(_parent,
                                _gtkTextCatalog.text(GtkTextId::LibraryFileSelectionFailed),
                                _gtkTextCatalog.fileSelectionError(operation, message),
                                {AppDialogAction{.label = std::string{_gtkTextCatalog.text(GtkTextId::CommonClose)},
                                                 .responseId = Gtk::ResponseType::CLOSE,
                                                 .role = AppDialogActionRole::Cancel}},
                                Gtk::ResponseType::CLOSE);
    auto tokenPtr = std::make_shared<ThemeRegistrationToken>(_themeCoordinator.registerToplevel(*dialog));
    dialog->signal_hide().connect([tokenPtr] { (*tokenPtr).reset(); });
  }
} // namespace ao::gtk::portal
