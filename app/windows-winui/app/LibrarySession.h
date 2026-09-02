// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/desktop/LibraryStartupPlanner.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
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
  class PlaybackActions;
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
    bool scanAfterOpen() const noexcept;
    bool operationActive() const noexcept;
    uimodel::PlaybackActions& playbackActions() const noexcept;
    i18n::MessageCatalog const& textCatalog() const noexcept;

    DesktopSettings const& settings() const noexcept;
    DesktopSettings& settings() noexcept;
    uimodel::TrackColumnLayouts const& columnLayouts() const noexcept;
    uimodel::TrackColumnLayouts& columnLayouts() noexcept;
    uimodel::TrackPresentationCatalog& presentationCatalog() const noexcept;
    uimodel::ListPresentations& listPresentations() const noexcept;
    /// The effective keyboard map: the shipped defaults with the user's overrides merged in.
    uimodel::KeymapModel const& keymap() const noexcept;
    std::filesystem::path const& stateRoot() const noexcept;
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

    Result<> initialize(std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest);
    /// Quiesce all callback and runtime-facing owners before releasing the runtime.
    void shutdown() noexcept;

    Result<> emplaceRuntimeGraph(std::filesystem::path const& root);
    Result<> saveSettingsCandidate(DesktopSettings const& settings);
    void bindRuntimeServices();
    void startActiveScan();
    void finishActiveScan(uimodel::LibraryScanOutcome outcome);
    void reportStatus(std::string status);
    void reportScanFailure(uimodel::LibraryScanOutcome const& outcome, std::string message);
    void reportBusy();
    void reportReady(std::filesystem::path const& root);
    void requestPlaySelection();

    struct Storage;
    std::unique_ptr<Storage> _storagePtr;
  };
} // namespace ao::winui
