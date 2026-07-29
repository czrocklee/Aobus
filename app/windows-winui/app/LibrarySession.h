// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
  class ConfigStore;
}

namespace ao::uimodel
{
  class ListPresentationPreferenceLifecycle;
  class PlaybackCommandSurface;
}

namespace ao::winui
{
  struct LibrarySessionCallbacks final
  {
    std::function<void()> onLibraryChanging;
    std::function<void()> onLibraryChanged;
    std::function<void(std::shared_ptr<rt::AppRuntime>)> onLibraryTaskRuntimeChanged;
    std::function<void()> onPlaybackChanging;
    std::function<void()> onPlaybackChanged;
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

    rt::AppRuntime& libraryRuntime() const noexcept;
    std::shared_ptr<rt::AppRuntime> libraryRuntimePtr() const noexcept { return _libraryRuntimePtr; }
    rt::AppRuntime& playbackRuntime() const noexcept;
    std::shared_ptr<rt::AppRuntime> playbackRuntimePtr() const noexcept { return _playbackRuntimePtr; }
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
    Result<> saveSettings();

    void setCallbacks(LibrarySessionCallbacks callbacks);
    void openLibrary(std::filesystem::path root);
    void rescan();
    void cancelLibraryOperation();
    Result<> playTrack(rt::ViewId viewId, TrackId trackId);

  private:
    struct CallbackLifetime final
    {};

    enum class LibraryPreparationMode : std::uint8_t
    {
      OpenExisting,
      ScanCandidate,
      RescanActive
    };

    std::shared_ptr<rt::AppRuntime> createRuntime(std::filesystem::path const& root);
    void bindPresentationPreferenceLifecycle();
    void bindPlaybackRuntime(std::shared_ptr<rt::AppRuntime> runtimePtr);
    void retainPlaybackUntilIdle();
    void scheduleAdoptLibraryPlayback();
    void requestPlaySelection();
    bool candidateRootInUse(std::filesystem::path const& root) const;
    void releaseCandidateRoot(std::filesystem::path const& root);
    void prepareAndSwap(std::filesystem::path root, LibraryPreparationMode mode);
    static bool workflowRetired(LibrarySession const* owner,
                                std::weak_ptr<CallbackLifetime> const& lifetimePtr,
                                std::uint64_t operationGeneration) noexcept;
    static void completeLibraryPreparation(LibrarySession* owner,
                                           std::shared_ptr<rt::AppRuntime>& candidatePtr,
                                           std::filesystem::path const& root,
                                           bool replaceLibrary,
                                           std::uint64_t operationGeneration);
    static void completeFailedLibraryPreparation(LibrarySession* owner,
                                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher,
                                                 std::shared_ptr<rt::AppRuntime> candidatePtr,
                                                 bool cancelled,
                                                 std::optional<Error> const& optFailure);
    static async::Task<void> prepareAndSwapWorkflow(LibrarySession* owner,
                                                    std::weak_ptr<CallbackLifetime> lifetimePtr,
                                                    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                                    std::shared_ptr<rt::AppRuntime> candidatePtr,
                                                    std::filesystem::path root,
                                                    LibraryPreparationMode mode,
                                                    std::uint64_t operationGeneration,
                                                    std::stop_token stopToken);

    std::filesystem::path _stateRoot;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
    std::unique_ptr<rt::ConfigStore> _settingsStorePtr;
    std::unique_ptr<rt::ConfigStore> _playbackStorePtr;
    uimodel::WindowsDesktopSettings _settings{};
    uimodel::TrackColumnLayoutState _columnLayouts{};
    uimodel::ListPresentationPreferenceState _presentationPreferences{};
    std::shared_ptr<rt::AppRuntime> _libraryRuntimePtr;
    std::shared_ptr<rt::AppRuntime> _playbackRuntimePtr;
    std::unique_ptr<uimodel::ListPresentationPreferenceLifecycle> _presentationPreferenceLifecyclePtr;
    std::unique_ptr<uimodel::PlaybackCommandSurface> _playbackCommandsPtr;
    LibrarySessionCallbacks _callbacks{};
    async::Subscription _retainedPlaybackSub;
    async::TaskHandle _libraryTask;
    std::vector<std::filesystem::path> _candidateRoots;
    std::optional<std::filesystem::path> _optOperationRoot;
    std::shared_ptr<CallbackLifetime> _callbackLifetimePtr = std::make_shared<CallbackLifetime>();
    std::uint64_t _operationGeneration = 0;
    bool _adoptScheduled = false;
    bool _operationActive = false;
  };
} // namespace ao::winui
