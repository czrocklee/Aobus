// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/ConfigStore.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <filesystem>
#include <memory>

namespace ao::rt
{
  struct AppPrefsState;
  struct AppSessionState;
}

namespace ao::gtk
{
  struct WindowState;

  /**
   * @brief Manages persistence of global application configuration.
   *
   * This class exclusively owns the global `config.yaml` file.
   */
  class AppConfigStore final
  {
  public:
    explicit AppConfigStore(std::filesystem::path const& configPath);

    /**
     * @brief A store for a session with no configuration directory to own.
     *
     * Everything reads as unset and every write keeps nothing, so the window
     * opens on defaults and forgets them on exit. See
     * `rt::ConfigStore::NoLocation` for why writes still report success.
     */
    explicit AppConfigStore(rt::ConfigStore::NoLocation /*noLocation*/);
    ~AppConfigStore();

    AppConfigStore(AppConfigStore const&) = delete;
    AppConfigStore& operator=(AppConfigStore const&) = delete;
    AppConfigStore(AppConfigStore&&) noexcept;
    AppConfigStore& operator=(AppConfigStore&&) noexcept;

    void loadWindow(WindowState& state) const;
    void saveWindow(WindowState const& state);

    void loadAppPrefs(rt::AppPrefsState& state) const;
    void saveAppPrefs(rt::AppPrefsState const& state);
    void loadAppSession(rt::AppSessionState& state) const;
    Result<> saveAppSession(rt::AppSessionState const& state);

    /** Store used by the active library's application-level playback session. */
    rt::ConfigStore& playbackSessionStore() noexcept;

    /// Loads the effective keyboard map: persisted overrides merged onto @p defaults.
    uimodel::KeymapModel loadKeymap(uimodel::KeymapBindings defaults) const;
    /// Persists the keymap's delta-from-defaults into the `shortcuts` group.
    void saveKeymap(uimodel::KeymapModel const& keymap);

  private:
    std::unique_ptr<rt::ConfigStore> _storePtr;
  };

  /**
   * @brief The intent that records a chosen output device as the user's preference.
   *
   * A surface that lets the user pick a device is stating a preference for the
   * next start, not a one-off route, so recording writes only
   * `preferredOutputSelection` back into the existing preferences. The store is
   * shared rather than borrowed because the recorder outlives the call that
   * builds it.
   */
  uimodel::OutputDeviceIntent preferredOutputDeviceRecorder(std::shared_ptr<AppConfigStore> configStorePtr);
} // namespace ao::gtk
