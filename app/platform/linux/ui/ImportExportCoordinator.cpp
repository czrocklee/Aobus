// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 RockStudio Contributors

#include "platform/linux/ui/ImportExportCoordinator.h"
#include "platform/linux/ui/LayoutConstants.h"
#include <rs/library/Importer.h>
#include <rs/utility/Log.h>
#include <rs/utility/ThreadUtils.h>

namespace app::ui
{
  ImportExportCoordinator::ImportExportCoordinator(Gtk::Window& parent, ImportExportCallbacks callbacks)
    : _parent(parent), _callbacks(std::move(callbacks))
  {
  }

  ImportExportCoordinator::~ImportExportCoordinator() = default;

  LibrarySession* ImportExportCoordinator::currentSession() const
  {
    if (_callbacks.getCurrentSession)
    {
      return _callbacks.getCurrentSession();
    }

    return nullptr;
  }

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

        // Scan for music files
        auto files = std::vector<std::filesystem::path>{};
        scanDirectory(path, files);

        if (files.empty())
        {
          if (_callbacks.onStatusMessage)
          {
            _callbacks.onStatusMessage("No music files found");
          }

          return;
        }

        auto pendingSession = std::shared_ptr<std::unique_ptr<LibrarySession>>{};

        if (currentSession() == nullptr)
        {
          pendingSession = std::make_shared<std::unique_ptr<LibrarySession>>(makeLibrarySession(path));
        }

        executeImportTask(files, std::move(pendingSession));
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting folder: {}", e.what());
    }
  }

  void ImportExportCoordinator::executeImportTask(std::vector<std::filesystem::path> const& files,
                                                  std::shared_ptr<std::unique_ptr<LibrarySession>> pendingSession)
  {
    auto* sessionPtr = pendingSession ? pendingSession->get() : currentSession();

    if (sessionPtr == nullptr)
    {
      if (_callbacks.onStatusMessage)
      {
        _callbacks.onStatusMessage("No music library open");
      }

      return;
    }

    _importDialog = std::make_unique<ImportProgressDialog>(static_cast<int>(files.size()), _parent);
    auto* dialogPtr = _importDialog.get();
    _importDialog->signal_response().connect([dialogPtr](int /*responseId*/) { dialogPtr->close(); });

    _importWorker = std::make_unique<rs::library::ImportWorker>(
      *sessionPtr->musicLibrary,
      files,
      [this, dialogPtr, total = files.size()](std::filesystem::path const& filePath, int index)
      {
        Glib::MainContext::get_default()->invoke(
          [this, dialogPtr, filePath, index, total]()
          {
            dialogPtr->onNewTrack(filePath.string(), index);
            if (_callbacks.onProgressUpdated)
            {
              double fraction = static_cast<double>(index) / static_cast<double>(total);
              _callbacks.onProgressUpdated(fraction, "Importing: " + filePath.filename().string());
            }
            return false;
          });
      },
      [dialogPtr]()
      {
        Glib::MainContext::get_default()->invoke(
          [dialogPtr]()
          {
            dialogPtr->ready();
            return false;
          });
      });

    auto* workerPtr = _importWorker.get();
    _importThread = std::jthread(
      [this, workerPtr, pendingSession = std::move(pendingSession)]() mutable
      {
        rs::setCurrentThreadName("FileImport");
        workerPtr->run();

        Glib::MainContext::get_default()->invoke(
          [this, pendingSession = std::move(pendingSession)]() mutable
          {
            auto importedLibraryTitle = std::string{};

            if (pendingSession)
            {
              importedLibraryTitle = pendingSession->get()->musicLibrary->rootPath().string();
              pendingSession->get()->rowDataProvider->loadAll();

              auto txn = pendingSession->get()->musicLibrary->readTransaction();
              pendingSession->get()->allTrackIds->reloadFromStore(txn);
            }

            onImportFinished();

            if (pendingSession && _callbacks.onLibrarySessionCreated)
            {
              _callbacks.onLibrarySessionCreated(std::move(*pendingSession));
            }

            if (!pendingSession && _callbacks.onLibraryDataMutated)
            {
              _callbacks.onLibraryDataMutated();
            }

            if (_callbacks.onProgressUpdated)
            {
              _callbacks.onProgressUpdated(1.0, "Import complete");
            }

            if (pendingSession && _callbacks.onTitleChanged)
            {
              _callbacks.onTitleChanged("RockStudio [" + importedLibraryTitle + "]");
            }

            return false;
          });
      });

    _importDialog->show();
  }

  void ImportExportCoordinator::onImportProgress(std::string const& /*filePath*/, int /*index*/)
  {
  }

  void ImportExportCoordinator::onImportFinished() const
  {
    if (_callbacks.onStatusMessage)
    {
      _callbacks.onStatusMessage("Import complete");
    }
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
    try
    {
      auto newSession = makeLibrarySession(path);
      if (_callbacks.onLibrarySessionCreated)
      {
        _callbacks.onLibrarySessionCreated(std::move(newSession));
      }
      if (_callbacks.onTitleChanged)
      {
        _callbacks.onTitleChanged("RockStudio [" + path.string() + "]");
      }
    }
    catch (std::exception const& e)
    {
      APP_LOG_ERROR("Error opening library: {}", e.what());
    }
  }

  void ImportExportCoordinator::importFilesFromPath(std::filesystem::path const& path)
  {
    auto files = std::vector<std::filesystem::path>{};
    scanDirectory(path, files);

    if (!files.empty())
    {
      auto pendingSession = std::make_shared<std::unique_ptr<LibrarySession>>(makeLibrarySession(path));
      executeImportTask(files, std::move(pendingSession));
    }
    else if (_callbacks.onStatusMessage)
    {
      _callbacks.onStatusMessage("No music files found");
    }
  }

  void ImportExportCoordinator::exportLibrary()
  {
    if (currentSession() == nullptr)
    {
      return;
    }

    auto* dialog = Gtk::make_managed<Gtk::Dialog>();
    dialog->set_title("Select Export Mode");
    dialog->set_transient_for(_parent);
    dialog->set_modal(true);

    auto* contentArea = dialog->get_content_area();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, Layout::kMarginMedium);
    box->set_margin(Layout::kMarginMedium);

    auto* label = Gtk::make_managed<Gtk::Label>("Choose what to include in the backup:");
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

    auto mode = rs::library::ExportMode::Metadata;

    switch (modeCombo->get_selected())
    {
      case 0: mode = rs::library::ExportMode::Minimum; break;
      case 1: mode = rs::library::ExportMode::Metadata; break;
      case 2: mode = rs::library::ExportMode::Full; break;
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
                                                     rs::library::ExportMode mode,
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

  void ImportExportCoordinator::executeExportTask(std::filesystem::path const& path, rs::library::ExportMode mode)
  {
    auto* session = currentSession();

    if (session == nullptr)
    {
      return;
    }

    auto* musicLibrary = session->musicLibrary.get();
    std::thread(
      [this, musicLibrary, path, mode]()
      {
        rs::setCurrentThreadName("LibraryExport");

        try
        {
          auto exporter = rs::library::Exporter{*musicLibrary};
          exporter.exportToYaml(path, mode);

          Glib::MainContext::get_default()->invoke(
            [this]()
            {
              if (_callbacks.onStatusMessage)
              {
                _callbacks.onStatusMessage("Library exported successfully");
              }

              return false;
            });
        }
        catch (std::exception const& e)
        {
          auto const errorText = std::string{e.what()};
          Glib::MainContext::get_default()->invoke(
            [this, errorText]()
            {
              APP_LOG_ERROR("Export failed: {}", errorText);

              if (_callbacks.onStatusMessage)
              {
                _callbacks.onStatusMessage("Export failed: " + errorText);
              }

              return false;
            });
        }
      })
      .detach();
  }

  void ImportExportCoordinator::importLibrary()
  {
    if (currentSession() == nullptr)
    {
      return;
    }

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
        auto* session = currentSession();

        if (session == nullptr)
        {
          return;
        }

        std::thread([this, path, session]() { runLibraryImportTask(path, session); }).detach();
      }
    }
    catch (Glib::Error const& e)
    {
      APP_LOG_ERROR("Error selecting import file: {}", e.what());
    }
  }

  void ImportExportCoordinator::runLibraryImportTask(std::filesystem::path const& path, LibrarySession* session)
  {
    rs::setCurrentThreadName("LibraryImport");

    try
    {
      auto importer = rs::library::Importer{*session->musicLibrary};
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
    Glib::MainContext::get_default()->invoke(
      [this, success, errorText]()
      {
        if (success)
        {
          if (_callbacks.onLibraryDataMutated)
          {
            _callbacks.onLibraryDataMutated();
          }

          if (_callbacks.onStatusMessage)
          {
            _callbacks.onStatusMessage("Library imported successfully");
          }
        }
        else
        {
          APP_LOG_ERROR("Import failed: {}", errorText);

          if (_callbacks.onStatusMessage)
          {
            _callbacks.onStatusMessage("Import failed: " + errorText);
          }
        }

        return false;
      });
  }
} // namespace app::ui
