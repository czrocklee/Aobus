// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/DesktopApplication.h"

#import <AppKit/AppKit.h>

#include <cstdint>
#include <filesystem>

namespace ao::appkit::test::desktop
{
  using ao::appkit::test::kIndexedFixtureTrackCount;
  extern NSString* const kPendingFixtureTrack;

  NSString* nativePath(std::filesystem::path const& path);
  NSWindow* findDesktopWindow();
  NSTableView* findTrackTable(NSView* view);
  std::int32_t selectableRowCount(NSTableView* table);
  NSInteger firstSelectableRow(NSTableView* table);
  NSString* rowIdentity(NSTableView* table, NSInteger row);
  NSInteger trackRow(NSTableView* table, NSString* title);
  NSMenuItem* findMenuItem(NSMenu* menu,
                           SEL action,
                           bool matchTag = false,
                           NSInteger tag = 0,
                           NSString* representedObject = nil);
  NSMenuItem* findRecentLibraryItem(std::filesystem::path const& root);
  void activateMenuItem(NSMenuItem* item, NSString* obligation);
  NSControl* findActionControl(NSView* view, SEL action);
  NSView* findAccessibilityView(NSView* view, NSString* identifier);
  NSSearchField* findSearchField(NSWindow* window);
  bool hasSavedLibrary(std::filesystem::path const& stateRoot, std::filesystem::path const& expected);
  void writeMarker(std::filesystem::path const& path, NSString* contents);
  NSDictionary* missingOutputRoute();
  void requireIndexedOpenWithoutScan(NSTableView* table, char const* context);
  void preseedSettings(DesktopLaunch const& launch);
  bool hasUnknownSetting(std::filesystem::path const& stateRoot);
} // namespace ao::appkit::test::desktop
