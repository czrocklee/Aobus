// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "app/macos-appkit/DesktopApplication.h"
#include "app/macos-appkit/LibrarySession.h"
#include <ao/Error.h>

#import <AppKit/AppKit.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace ao::appkit::test
{
  inline constexpr std::int32_t kFixtureTrackCount = 20;
  inline constexpr std::int32_t kIndexedFixtureTrackCount = kFixtureTrackCount - 1;
  inline constexpr auto kDefaultFixtureDuration = std::chrono::milliseconds{12'000};
  inline constexpr auto kMediaSeekFixtureDuration = std::chrono::milliseconds{16'002};
  inline constexpr auto kMediaInteriorSeekFixtureDuration = std::chrono::milliseconds{15'992};

  bool tryWaitUntil(std::function<bool()> predicate, std::chrono::seconds timeout = std::chrono::seconds{15});
  void requireWaitUntil(std::function<bool()> predicate,
                        char const* obligation,
                        std::chrono::seconds timeout = std::chrono::seconds{15});
  Result<> prepareAppKitMediaLibrary(std::filesystem::path const& musicRoot,
                                     std::chrono::milliseconds duration,
                                     std::optional<std::chrono::milliseconds> optLastTrackDuration = std::nullopt);
  Result<> prepareAppKitFixtureRoots(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  void preindexLibraryWithPendingTrack(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  void settleNativeCallbacks();
  NSView* findView(NSView* view, NSString* identifier);
  NSControl* findControl(NSView* view, NSString* identifier);
  void editText(NSWindow* window, NSString* identifier, NSString* text);
  void captureView(NSView* view, std::filesystem::path const& path);
  void exerciseActivityExpiration();
  void exerciseSoulOcclusion();
  void exerciseSelectedArtwork(LibrarySession& session, NSWindow* window, TrackId trackId);

  class SessionFixture final
  {
  public:
    enum class AudioOutput : std::uint8_t
    {
      Silent,
      CoreAudio,
    };

    SessionFixture(std::filesystem::path const& musicRoot,
                   std::filesystem::path const& stateRoot,
                   AudioOutput output = AudioOutput::CoreAudio);
    ~SessionFixture();
    SessionFixture(SessionFixture const&) = delete;
    SessionFixture& operator=(SessionFixture const&) = delete;
    SessionFixture(SessionFixture&&) = delete;
    SessionFixture& operator=(SessionFixture&&) = delete;
    LibrarySession& session();

  private:
    std::unique_ptr<LibrarySession> _sessionPtr;
  };

  std::int32_t runDesktopScenario(DesktopLaunch launch, i18n::MessageCatalog catalog, bool successor);
  std::int32_t runInitialIndexScenario(DesktopLaunch launch, i18n::MessageCatalog catalog);
  std::int32_t runMembershipScenario(DesktopLaunch launch, i18n::MessageCatalog catalog);
  std::int32_t runMediaScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  std::int32_t runAuthoringScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  std::int32_t runCreditsControlsScenario(std::filesystem::path const& musicRoot,
                                          std::filesystem::path const& stateRoot);
  std::int32_t runCreditsPreviewsScenario(std::filesystem::path const& musicRoot,
                                          std::filesystem::path const& stateRoot);
  std::int32_t runPresentationScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
} // namespace ao::appkit::test
