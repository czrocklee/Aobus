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
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/utility/PlatformDirectories.h>

#include <gtkmm/application.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <utility>

namespace ao::gtk
{
  Result<Glib::RefPtr<MainWindow>> prepareLibraryWindow(
    LibraryWindowPaths paths,
    std::shared_ptr<AppConfigStore> appConfigStorePtr,
    std::shared_ptr<ShellLayoutStore> shellLayoutStorePtr,
    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
    i18n::MessageCatalog const& textCatalog,
    rt::TextOrderingPolicy const* textOrderingPolicy,
    rt::CompletionAliasPolicy const* completionAliasPolicy)
  {
    auto executorPtr = std::make_unique<GtkMainContextExecutor>();

    auto const workspaceConfigPath = paths.databasePath / "workspace.yaml";
    auto workspaceConfigStorePtr = std::make_unique<rt::ConfigStore>(workspaceConfigPath);

    // Nothing names a home or profile location. What lives there is derived, so
    // the session opens without a cache rather than refusing to start: cover
    // reads then re-extract from the media files, which costs latency only.
    auto const cacheDirRes = utility::applicationCacheDirectory();

    if (!cacheDirRes)
    {
      APP_LOG_WARN("Aobus caches no cover art this session: {}", cacheDirRes.error().message);
    }

    auto runtimeRes = rt::AppRuntime::create(
      rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                 .musicRoot = std::move(paths.musicRoot),
                                 .databasePath = std::move(paths.databasePath),
                                 .cacheDirectory = cacheDirRes ? *cacheDirRes : std::filesystem::path{},
                                 .workspaceConfigStorePtr = std::move(workspaceConfigStorePtr),
                                 .playbackSessionConfigStore = &appConfigStorePtr->playbackSessionStore(),
                                 .textOrderingPolicy = textOrderingPolicy,
                                 .completionAliasPolicy = completionAliasPolicy});

    if (!runtimeRes)
    {
      return makeError(runtimeRes.error().code, "Failed to open library: " + runtimeRes.error().message);
    }

    auto appRuntimePtr = std::move(*runtimeRes);

    registerPlatformAudioBackends(*appRuntimePtr);

    auto windowPtr = Glib::make_refptr_for_instance<MainWindow>(
      new MainWindow{*appRuntimePtr, appConfigStorePtr, shellLayoutStorePtr, textCatalog, componentStateStorePtr});

    // Frontend observers are members of MainWindow, while the runtime is attached
    // to its GObject. Finalization therefore releases the observers before the
    // runtime storage borrowed by those observers.
    windowPtr->set_data("app-runtime",
                        new std::unique_ptr<rt::AppRuntime>{std::move(appRuntimePtr)},
                        [](void* data) { delete static_cast<std::unique_ptr<rt::AppRuntime>*>(data); });

    if (auto const preparedRes = windowPtr->prepareSession(); !preparedRes)
    {
      return std::unexpected{preparedRes.error()};
    }

    return windowPtr;
  }

  Result<> activateLibraryWindow(Gtk::Application& app,
                                 Glib::RefPtr<MainWindow> const& windowPtr,
                                 MainWindow::PlaybackRestoreMode const restoreMode)
  {
    app.add_window(*windowPtr);

    if (auto const activatedRes = windowPtr->activateSession(restoreMode); !activatedRes)
    {
      app.remove_window(*windowPtr);
      return std::unexpected{activatedRes.error()};
    }

    windowPtr->present();
    return {};
  }
} // namespace ao::gtk
