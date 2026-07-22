// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibraryWindowLifecycle.h"

#include "app/AppConfigStore.h"
#include "app/GtkMainContextExecutor.h"
#include "app/MainWindow.h"
#include "app/ShellLayoutComponentStateStore.h"
#include "app/ShellLayoutStore.h"
#include "platform/AudioBackendBootstrap.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>

#include <gtkmm/application.h>

#include <exception>
#include <expected>
#include <memory>
#include <utility>

namespace ao::gtk
{
  Glib::RefPtr<MainWindow> prepareLibraryWindow(LibraryWindowPaths paths,
                                                std::shared_ptr<AppConfigStore> appConfigStorePtr,
                                                std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
                                                std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr)
  {
    auto asyncExceptionHandler = rt::Log::asyncExceptionHandler();
    auto executorPtr = std::make_unique<GtkMainContextExecutor>();

    auto const workspaceConfigPath = paths.databasePath / "workspace.yaml";
    auto workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(workspaceConfigPath);

    auto appRuntimePtr = std::make_unique<rt::AppRuntime>(
      rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                 .musicRoot = std::move(paths.musicRoot),
                                 .databasePath = std::move(paths.databasePath),
                                 .workspaceConfigStorePtr = std::move(workspaceConfigStorePtr),
                                 .playbackSessionConfigStore = &appConfigStorePtr->playbackSessionStore(),
                                 .asyncExceptionHandler = std::move(asyncExceptionHandler)});

    registerPlatformAudioBackends(*appRuntimePtr);

    auto windowPtr = Glib::make_refptr_for_instance<MainWindow>(
      new MainWindow{*appRuntimePtr, appConfigStorePtr, shellLayoutStorePtr, componentStateStorePtr});

    // Frontend observers are members of MainWindow, while the runtime is attached
    // to its GObject. Finalization therefore releases the observers before the
    // runtime storage borrowed by those observers.
    windowPtr->set_data("app-runtime",
                        new std::unique_ptr<rt::AppRuntime>{std::move(appRuntimePtr)},
                        [](void* data) { delete static_cast<std::unique_ptr<rt::AppRuntime>*>(data); });

    if (auto const prepared = windowPtr->prepareSession(); !prepared)
    {
      throwException<Exception>(prepared.error().message);
    }

    return windowPtr;
  }

  void activateLibraryWindow(Gtk::Application& app,
                             Glib::RefPtr<MainWindow> const& windowPtr,
                             MainWindow::PlaybackRestoreMode const restoreMode)
  {
    app.add_window(*windowPtr);

    if (auto const activated = windowPtr->activateSession(restoreMode); !activated)
    {
      app.remove_window(*windowPtr);
      throwException<Exception>(activated.error().message);
    }

    windowPtr->present();
  }

  Result<LibraryWindowOpenOutcome> openLibraryWindow(std::filesystem::path const& activeRoot,
                                                     std::filesystem::path const& requestedRoot,
                                                     bool const scanAfterOpen,
                                                     LibraryWindowReplacementCallbacks const& callbacks)
  {
    if (activeRoot == requestedRoot)
    {
      if (scanAfterOpen)
      {
        callbacks.scanActive();
      }

      callbacks.presentActive();
      return LibraryWindowOpenOutcome::Reused;
    }

    callbacks.prepareCandidate();
    callbacks.configureCandidate();

    if (auto const retired = callbacks.retireActive(); !retired)
    {
      return std::unexpected<Error>{retired.error()};
    }

    callbacks.activateCandidate();
    callbacks.replaceActiveSlot();
    callbacks.releaseRetired();

    try
    {
      callbacks.persistSelectedPath();
    }
    catch (std::exception const& e)
    {
      APP_LOG_WARN("Failed to persist the selected GTK library path: {}", e.what());
    }
    catch (...)
    {
      APP_LOG_WARN("Failed to persist the selected GTK library path: unknown exception");
    }

    if (scanAfterOpen)
    {
      callbacks.scanActive();
    }

    return LibraryWindowOpenOutcome::Replaced;
  }
} // namespace ao::gtk
