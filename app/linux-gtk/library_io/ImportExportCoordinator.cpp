// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "library_io/ImportExportCoordinator.h"
#include "layout/LayoutConstants.h"
#include <ao/library/Importer.h>
#include <ao/utility/Log.h>
#include <ao/utility/ThreadUtils.h>
#include <runtime/AppSession.h>
#include <runtime/LibraryMutationService.h>
#include <runtime/NotificationService.h>

namespace ao::gtk
{
  ImportExportCoordinator::ImportExportCoordinator(Gtk::Window& parent,
                                                   rt::AppSession& session,
                                                   ImportExportCallbacks callbacks)
    : _parent{parent}, _session{session}, _callbacks{std::move(callbacks)}
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
                                std::filesystem::path path(folder->get_path());
                                auto libPath = path / "data.mdb";

                                if (std::filesystem::exists(libPath))
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

        auto files = std::vector<std::filesystem::path>{};
        scanDirectory(path, files);

        if (files.empty())
        {
          _session.notifications().post(rt::NotificationSeverity::Info, "No music files found");
          return;
        }

        executeImportTask(files, false);
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

    _importProgressSub = _session.mutation().onImportProgress(
      [dialogPtr, total = files.size()](auto const& ev)
      { dialogPtr->onNewTrack(ev.message, static_cast<int>(ev.fraction * static_cast<double>(total))); });

    _importCompleteSub = _session.mutation().onImportCompleted(
      [this, dialogPtr, isNewLibrary](auto)
      {
        dialogPtr->ready();
        onImportFinished();

        if (isNewLibrary && _callbacks.onLibraryDataMutated)
        {
          _callbacks.onLibraryDataMutated();
        }

        _importProgressSub.reset();
        _importCompleteSub.reset();
      });

    _session.mutation().importFiles(files);
    _importDialog->show();
  }

  void ImportExportCoordinator::onImportFinished() const
  {
    _session.notifications().post(rt::NotificationSeverity::Info, "Import complete");
  }

  void ImportExportCoordinator::scanDirectory(std::filesystem::path const& dir,
                                              std::vector<std::filesystem::path>& files) const
  {
    try
    {
      for (auto const& entry : std::filesystem::recursive_directory_iterator(dir))
      {
        if (entry.is_regular_file())
        {
          auto ext = entry.path().extension().string();
          std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return std::tolower(ch); });

          if (ext == ".flac" || ext == ".m4a" || ext == ".mp3" || ext == ".wav")
          {
            files.push_back(entry.path());
          }
        }
      }
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Error scanning directory: {}", e.what());
    }
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

    auto files = std::vector<std::filesystem::path>{};
    scanDirectory(path, files);

    if (!files.empty())
    {
      executeImportTask(files, true);
    }
    else
    {
      _session.notifications().post(rt::NotificationSeverity::Info, "No music files found");
    }
  }

  void ImportExportCoordinator::exportLibrary()
  {
    auto* const dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Select Export Mode");
    dialog->set_transient_for(_parent);
    dialog->set_modal(true);

    auto* const contentArea = dialog->get_content_area();
    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kMarginMedium);
    box->set_margin(layout::kMarginMedium);

    auto* const label = Gtk::make_managed<Gtk::Label>("Choose what to include in the backup:");
    label->set_halign(Gtk::Align::START);
    box->append(*label);

    auto* modeCombo = Gtk::make_managed<Gtk::DropDown>();
    auto modeStrings = Gtk::StringList::create({"App-Only (Tags, Ratings, Lists)",
                                                "Metadata + App Data (Title, Artist, Tags, etc.)",
                                                "Full Backup (All Metadata + Audio Properties + Cover Art)"});
    modeCombo->set_model(modeStrings);
    modeCombo->set_selected(1);
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

    auto mode = library::ExportMode::Metadata;

    switch (modeCombo->get_selected())
    {
      case 0: mode = library::ExportMode::Minimum; break;
      case 1: mode = library::ExportMode::Metadata; break;
      case 2: mode = library::ExportMode::Full; break;
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
                                                     library::ExportMode mode,
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

  void ImportExportCoordinator::executeExportTask(std::filesystem::path const& path, library::ExportMode mode)
  {
    if (_exportThread.joinable())
    {
      return;
    }

    auto& library = _session.musicLibrary();
    _exportThread = std::jthread(
      [this, &library, path, mode]
      {
        setCurrentThreadName("LibraryExport");

        try
        {
          auto exporter = library::Exporter{library};
          exporter.exportToYaml(path, mode);

          _session.executor().dispatch(
            [this] { _session.notifications().post(rt::NotificationSeverity::Info, "Library exported successfully"); });
        }
        catch (std::exception const& e)
        {
          auto const errorText = std::string{e.what()};
          _session.executor().dispatch(
            [this, errorText]
            {
              APP_LOG_ERROR("Export failed: {}", errorText);
              _session.notifications().post(rt::NotificationSeverity::Error, "Export failed: " + errorText);
            });
        }
      });
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

        if (_importTaskThread.joinable())
        {
          return;
        }

        _importTaskThread = std::jthread([this, path] { runLibraryImportTask(path); });
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting import file: {}", e.what());
    }
  }

  void ImportExportCoordinator::runLibraryImportTask(std::filesystem::path const& path)
  {
    setCurrentThreadName("LibraryImport");

    try
    {
      auto importer = library::Importer{_session.musicLibrary()};
      importer.importFromYaml(path);
      reportImportResult(true, "");
    }
    catch (std::exception const& e)
    {
      reportImportResult(false, e.what());
    }
  }

  void ImportExportCoordinator::reportImportResult(bool success, std::string const& errorText)
  {
    _session.executor().dispatch(
      [this, success, errorText]
      {
        if (success)
        {
          if (_callbacks.onLibraryDataMutated)
          {
            _callbacks.onLibraryDataMutated();
          }

          _session.notifications().post(rt::NotificationSeverity::Info, "Library imported successfully");
        }
        else
        {
          APP_LOG_ERROR("Import failed: {}", errorText);
          _session.notifications().post(rt::NotificationSeverity::Error, "Import failed: " + errorText);
        }
      });
  }
} // namespace ao::gtk
