// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
  class ConfigStore;
}

namespace ao::uimodel
{
  class ListPresentationPreferenceLifecycle;
  class PlaybackCommandSurface;
  class TrackPresentationCatalog;
}

namespace ao::winui
{
  struct LibrarySessionCallbacks final
  {
    std::move_only_function<void() noexcept> onRuntimeChanging;
    std::move_only_function<void() noexcept> onRuntimeChanged;
    std::function<void(std::string)> onStatus;
    std::function<void(Error const&)> onFailure;
  };

  class [[nodiscard]] LibrarySession final
  {
  public:
    LibrarySession(std::filesystem::path stateRoot, winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);
    ~LibrarySession();

    LibrarySession(LibrarySession const&) = delete;
    LibrarySession& operator=(LibrarySession const&) = delete;
    LibrarySession(LibrarySession&&) = delete;
    LibrarySession& operator=(LibrarySession&&) = delete;

    rt::AppRuntime& runtime() const noexcept;
    std::shared_ptr<rt::AppRuntime> runtimePtr() const noexcept { return _runtimePtr; }
    uimodel::PlaybackCommandSurface& playbackCommands() const noexcept;

    uimodel::WindowsDesktopSettings const& settings() const noexcept { return _settings; }
    uimodel::WindowsDesktopSettings& settings() noexcept { return _settings; }
    uimodel::TrackColumnLayoutState const& columnLayouts() const noexcept { return _columnLayouts; }
    uimodel::TrackColumnLayoutState& columnLayouts() noexcept { return _columnLayouts; }
    uimodel::ListPresentationPreferenceState const& presentationPreferences() const noexcept
    {
      return _presentationPreferences;
    }
    uimodel::ListPresentationPreferenceState& presentationPreferences() noexcept { return _presentationPreferences; }
    std::filesystem::path const& stateRoot() const noexcept { return _stateRoot; }
    rt::TrackPresentationSpec presentationForList(ListId listId) const;
    Result<> saveSettings();

    void setCallbacks(LibrarySessionCallbacks callbacks);
    void openLibrary(std::filesystem::path root);
    void rescan();
    Result<> playTrack(rt::ViewId viewId, TrackId trackId);

  private:
    struct CallbackLifetime final
    {};

    Result<std::shared_ptr<rt::AppRuntime>> createRuntime(std::filesystem::path const& root);
    void bindRuntimeServices();
    void installRuntime(std::shared_ptr<rt::AppRuntime> nextPtr,
                        std::filesystem::path const& root,
                        bool scanAfterInstall) noexcept;
    void startActiveScan();
    void finishActiveScan(
      std::expected<uimodel::LibraryScanWorkflowResult, uimodel::LibraryScanWorkflowFailure> result) noexcept;
    void reportStatus(std::string status) noexcept;
    void reportFailure(Error const& error) noexcept;
    void reportBusy() noexcept;
    void reportReady(std::filesystem::path const& root) noexcept;
    void requestPlaySelection();

    std::filesystem::path _stateRoot;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::unique_ptr<rt::ConfigStore> _settingsStorePtr;
    std::unique_ptr<rt::ConfigStore> _playbackStorePtr;
    uimodel::WindowsDesktopSettings _settings{};
    uimodel::TrackColumnLayoutState _columnLayouts{};
    uimodel::ListPresentationPreferenceState _presentationPreferences{};
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    std::unique_ptr<uimodel::TrackPresentationCatalog> _presentationCatalogPtr;
    std::unique_ptr<uimodel::ListPresentationPreferenceLifecycle> _presentationPreferenceLifecyclePtr;
    std::unique_ptr<uimodel::PlaybackCommandSurface> _playbackCommandsPtr;
    LibrarySessionCallbacks _callbacks{};
    async::TaskHandle _libraryTask;
    std::shared_ptr<CallbackLifetime> _callbackLifetimePtr = std::make_shared<CallbackLifetime>();
    std::string_view _operationStatusKey;
    bool _operationActive = false;
  };
} // namespace ao::winui
