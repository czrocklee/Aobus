// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/desktop/LibraryStartupPlanner.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>
#include <ao/uimodel/presentation/PresentationText.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
  class CompletionAliasPolicy;
  class ConfigStore;
  class TextOrderingPolicy;
}

namespace ao::uimodel
{
  class PlaybackCommandSurface;
  class TrackPresentationCatalog;
}

namespace ao::winui
{
  class DispatcherQueueExecutor;

  struct LibrarySessionCallbacks final
  {
    std::function<void(std::string)> onStatus;
    std::function<void(Error const&)> onFailure;
  };

  class [[nodiscard]] LibrarySession final
  {
  public:
    static Result<std::unique_ptr<LibrarySession>> create(
      std::filesystem::path stateRoot,
      winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
      i18n::MessageCatalog textCatalog,
      rt::TextOrderingPolicy const& textOrderingPolicy,
      rt::CompletionAliasPolicy const& completionAliasPolicy,
      std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest);
    ~LibrarySession();

    LibrarySession(LibrarySession const&) = delete;
    LibrarySession& operator=(LibrarySession const&) = delete;
    LibrarySession(LibrarySession&&) = delete;
    LibrarySession& operator=(LibrarySession&&) = delete;

    rt::AppRuntime& runtime() const noexcept;
    std::filesystem::path const& musicRoot() const noexcept;
    bool scanAfterOpen() const noexcept { return _scanAfterOpen; }
    bool operationActive() const noexcept { return _operationActive; }
    uimodel::PlaybackCommandSurface& playbackCommands() const noexcept;
    i18n::MessageCatalog const& textCatalog() const noexcept { return _textCatalog; }

    DesktopSettings const& settings() const noexcept { return _settings; }
    DesktopSettings& settings() noexcept { return _settings; }
    uimodel::TrackColumnLayoutState const& columnLayouts() const noexcept { return _columnLayouts; }
    uimodel::TrackColumnLayoutState& columnLayouts() noexcept { return _columnLayouts; }
    uimodel::ListPresentationPreferenceState const& presentationPreferences() const noexcept
    {
      return _presentationPreferences;
    }
    uimodel::ListPresentationPreferenceState& presentationPreferences() noexcept { return _presentationPreferences; }
    uimodel::TrackPresentationCatalog& presentationCatalog() const noexcept { return *_presentationCatalogPtr; }
    uimodel::ListPresentationPreferenceStore& presentationPreferenceStore() const noexcept
    {
      return *_presentationPreferenceStorePtr;
    }
    /// The effective keyboard map: the shipped defaults with the user's overrides merged in.
    uimodel::KeymapModel const& keymap() const noexcept { return _keymap; }
    std::filesystem::path const& stateRoot() const noexcept { return _stateRoot; }
    rt::TrackPresentationSpec presentationForList(ListId listId) const;
    Result<> saveSettings();
    void setPreferredOutputSelection(audio::OutputDeviceSelection const& selection) noexcept;
    /// Apply and save an explicit startup root only after native activation.
    Result<> commitSelectedRoot();
    /// Remove playback intent and seal persistence before a destructive restart.
    Result<> retirePlaybackSessionForLibrarySwitch();

    void setCallbacks(LibrarySessionCallbacks callbacks);
    void rescan() noexcept;
    Result<> playTrack(rt::ViewId viewId, TrackId trackId);

  private:
    LibrarySession(std::filesystem::path stateRoot,
                   winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                   i18n::MessageCatalog textCatalog,
                   rt::TextOrderingPolicy const& textOrderingPolicy,
                   rt::CompletionAliasPolicy const& completionAliasPolicy);

    struct CallbackLifetime final
    {};

    enum class PlaybackPersistenceAdmission : std::uint8_t
    {
      Ready,
      AwaitingRootCommit,
      Sealed,
      Retired,
    };

    Result<> initialize(std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest);
    /// Quiesce all callback and runtime-facing owners before releasing the runtime.
    void shutdown() noexcept;

    Result<std::unique_ptr<rt::AppRuntime>> createRuntime(std::filesystem::path const& root);
    Result<> saveSettingsCandidate(DesktopSettings const& settings);
    void bindRuntimeServices();
    void startActiveScan();
    void finishActiveScan(
      std::expected<uimodel::LibraryScanWorkflowResult, uimodel::LibraryScanWorkflowFailure> result);
    void reportStatus(std::string status);
    void reportScanFailure(uimodel::LibraryScanOutcome const& outcome, std::string message);
    void reportBusy();
    void reportReady(std::filesystem::path const& root);
    void requestPlaySelection();

    std::filesystem::path _stateRoot;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    i18n::MessageCatalog _textCatalog;
    rt::TextOrderingPolicy const& _textOrderingPolicy;
    rt::CompletionAliasPolicy const& _completionAliasPolicy;
    std::unique_ptr<rt::ConfigStore> _settingsStorePtr;
    std::unique_ptr<rt::ConfigStore> _playbackStorePtr;
    DesktopSettings _settings{};
    uimodel::TrackColumnLayoutState _columnLayouts{};
    uimodel::ListPresentationPreferenceState _presentationPreferences{};
    uimodel::KeymapModel _keymap{};
    std::optional<std::filesystem::path> _optSelectedRootCommit;
    std::unique_ptr<rt::AppRuntime> _runtimePtr;
    DispatcherQueueExecutor* _dispatcherExecutor = nullptr;
    std::unique_ptr<uimodel::TrackPresentationCatalog> _presentationCatalogPtr;
    std::unique_ptr<uimodel::ListPresentationPreferenceStore> _presentationPreferenceStorePtr;
    async::Subscription _presentationPreferenceSub;
    std::unique_ptr<uimodel::PlaybackCommandSurface> _playbackCommandsPtr;
    LibrarySessionCallbacks _callbacks{};
    async::TaskHandle _libraryTask;
    std::shared_ptr<CallbackLifetime> _callbackLifetimePtr = std::make_shared<CallbackLifetime>();
    std::string_view _operationStatusKey;
    bool _operationActive = false;
    bool _scanAfterOpen = false;
    bool _shutdown = false;
    PlaybackPersistenceAdmission _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Ready;
  };
} // namespace ao::winui
