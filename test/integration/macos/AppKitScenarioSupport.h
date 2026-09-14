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

namespace ao::appkit::test
{
  bool tryWaitUntil(std::function<bool()> predicate, std::chrono::seconds timeout = std::chrono::seconds{15});
  void requireWaitUntil(std::function<bool()> predicate,
                        char const* obligation,
                        std::chrono::seconds timeout = std::chrono::seconds{15});
  Result<> prepareAppKitMediaLibrary(std::filesystem::path const& musicRoot);
  Result<> prepareAppKitFixtureRoots(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  void settleNativeCallbacks();
  NSView* findView(NSView* view, NSString* identifier);
  NSControl* findControl(NSView* view, NSString* identifier);
  void editText(NSWindow* window, NSString* identifier, NSString* text);
  void captureView(NSView* view, std::filesystem::path const& path);
  void exerciseActivityExpiration();
  void exerciseSelectedArtwork(LibrarySession& session, NSWindow* window, TrackId trackId);

  class SessionFixture final
  {
  public:
    SessionFixture(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
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
  std::int32_t runMediaScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  std::int32_t runAuthoringScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
  std::int32_t runPresentationScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot);
} // namespace ao::appkit::test
