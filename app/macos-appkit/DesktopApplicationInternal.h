// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

// Implementation of one coordinator across native shell and command-surface files.
// Presentation components must not depend on this header.
#include "ActivityPopover.h"
#include "AppKitText.h"
#include "DesktopApplication.h"
#include "DesktopControls.h"
#include "LibraryBrowser.h"
#include "LibraryEditor.h"
#include "LibrarySession.h"
#include "PlaybackBar.h"
#include "TrackInspector.h"
#include <ao/Contract.h>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::appkit
{
  template<typename Callback>
  void nativeCallback(Callback&& callback) noexcept
  {
    try
    {
      std::forward<Callback>(callback)();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "AppKit desktop callback");
    }
  }
  NSTextField* label(NSString* text, CGFloat size, BOOL secondary = NO);
  NSAppearance* nativeAppearance(NSString* name);
  inline constexpr auto kContentInset = 16;
  inline constexpr auto kCaptionFontSize = 10;
  inline constexpr auto kBodyFontSize = 13;
  inline constexpr auto kMinimumWindowWidth = 760;
  inline constexpr auto kMinimumWindowHeight = 520;
  inline constexpr auto kInitialWindowWidth = 1240;
  inline constexpr auto kInitialWindowHeight = 800;
} // namespace ao::appkit

@interface AobusDesktopDelegate
  : NSObject<NSApplicationDelegate, NSWindowDelegate, AobusLibraryDropDestination, AobusLibraryBrowserDelegate> {
  ao::appkit::DesktopLaunch _launch;
  std::unique_ptr<ao::appkit::LibrarySession> _sessionPtr;
  std::optional<ao::desktop::LibrarySwitchRequest> _optPendingSuccessor;
  NSMutableDictionary* _settings;
  NSWindow* _window;
  NSViewController* _rootController;
  NSSplitViewController* _splitController;
  NSSplitViewItem* _sidebarItem;
  NSSplitViewItem* _browserItem;
  NSSplitViewItem* _inspectorItem;
  AobusRootSurface* _root;
  NSSplitView* _split;
  NSLayoutConstraint* _splitTopConstraint;
  NSLayoutConstraint* _splitBottomConstraint;
  NSView* _sidebar;
  AobusSurface* _browser;
  NSView* _inspector;
  AobusTrackInspector* _trackInspector;
  NSStackView* _modernHeader;
  AobusPlaybackBar* _playbackBar;
  NSMenu* _playbackMenu;
  NSArray<NSLayoutConstraint*>* _modernBrowserConstraints;
  AobusLibraryBrowser* _libraryBrowser;
  NSSearchField* _search;
  NSSearchField* _toolbarSearch;
  NSSearchToolbarItem* _toolbarSearchItem;
  NSToolbarItem* _toolbarActivityItem;
  NSPopUpButton* _presentation;
  NSTextField* _libraryLabel;
  NSTextField* _count;
  NSTextField* _toolbarTitle;
  NSTextField* _toolbarCount;
  NSButton* _activityButton;
  AobusActivityPopover* _activityPopover;
  NSTextField* _status;
  NSTextField* _empty;
  NSTextField* _emptyHint;
  NSButton* _emptyAction;
  NSTextField* _browserTitle;
  NSImageView* _sidebarCover;
  BOOL _modern;
  BOOL _inspectorVisible;
  BOOL _closing;
  BOOL _layoutPending;
  BOOL _presentingMenu;
  BOOL _libraryDirty;
  BOOL _playbackDirty;
  NSMenu* _trackMenu;
  NSMenu* _listMenu;
  NSMenu* _recentLibrariesMenu;
}
- (instancetype)initWithLaunch:(ao::appkit::DesktopLaunch)launch catalog:(ao::i18n::MessageCatalog)catalog;
- (void)applicationDidFinishLaunching:(NSNotification*) [[maybe_unused]] notification;
- (NSString*)text:(ao::i18n::MessageId)message;
- (void)windowDidMiniaturize:(NSNotification*) [[maybe_unused]] notification;
- (void)windowDidDeminiaturize:(NSNotification*) [[maybe_unused]] notification;
- (void)windowDidChangeOcclusionState:(NSNotification*) [[maybe_unused]] notification;
- (void)applicationDidHide:(NSNotification*) [[maybe_unused]] notification;
- (void)applicationDidUnhide:(NSNotification*) [[maybe_unused]] notification;
- (void)invalidate:(ao::appkit::DesktopInvalidation)invalidation;
- (void)invalidateAll;
- (void)requestService;
- (void)requestEditorCloseForLifecycle;
- (BOOL)tryServiceLibrarySwitch;
- (BOOL)tryServiceQuit;
- (BOOL)tryFinishClosing;
- (void)serviceViewUpdates;
- (void)service;
- (void)updateFrameTimer;
- (BOOL)isWindowVisibleForRendering;
- (void)frameTick:(NSTimer*) [[maybe_unused]] timer;
- (void)sheetDidEnd:(NSNotification*) [[maybe_unused]] notification;
- (void)refreshLibrary;
- (void)refreshPlayback;
- (void)refreshPlaybackProgress;
- (void)refreshInspector;
- (void)refreshInspectorWithSelection:(std::vector<ao::TrackId> const&)selection;
- (void)transport:(NSControl*)sender;
- (void)seek:(NSSlider*)sender;
- (void)volume:(NSSlider*)sender;
- (void)rescan:(id) [[maybe_unused]] sender;
- (void)showActivity:(id) [[maybe_unused]] sender;
- (void)refreshActivity;
- (void)selectOutput:(NSMenuItem*)sender;
- (void)revealSelection:(id)sender;
- (NSURL*)revealURL:(id)sender;
- (BOOL)saveSettings;
- (void)openLibrary:(id) [[maybe_unused]] sender;
- (void)rememberLibraryURL:(NSURL*)url;
- (void)openRecentLibrary:(NSMenuItem*)sender;
- (void)clearRecentLibraries:(id) [[maybe_unused]] sender;
- (NSDragOperation)validateLibraryDrop:(id<NSDraggingInfo>)sender;
- (BOOL)performLibraryDrop:(id<NSDraggingInfo>)sender;
- (void)showMainWindow:(id) [[maybe_unused]] sender;
- (void)openLibraryAtURL:(NSURL*)url;
- (void)prepareLibrarySwitch;
- (BOOL)windowShouldClose:(NSWindow*)window;
- (BOOL)applicationShouldHandleReopen:(NSApplication*) [[maybe_unused]] app
                    hasVisibleWindows:(BOOL) [[maybe_unused]] visible;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*) [[maybe_unused]] app;
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*) [[maybe_unused]] app;
- (void)requestQuit:(id) [[maybe_unused]] sender;
- (BOOL)prepareToClose;
- (void)finishClosing;
- (ao::ListId)activeListId;
- (ao::ListId)capturedList:(id)sender;
- (std::vector<ao::TrackId>)capturedTracks:(id)sender;
- (BOOL)isLibraryBrowserClosing:(AobusLibraryBrowser*)browser;
- (BOOL)isLibraryBrowserSheetBlocked:(AobusLibraryBrowser*)browser;
- (void)libraryBrowserSelectionDidChange:(AobusLibraryBrowser*)browser;
- (BOOL)libraryBrowser:(AobusLibraryBrowser*)browser
  requestMembershipForTracks:(std::vector<ao::TrackId> const&)trackIds
                      listId:(ao::ListId)listId;
- (void)showProperties:(id)sender;
- (void)newList:(id) [[maybe_unused]] sender;
- (void)newChildList:(id)sender;
- (void)editList:(id)sender;
- (void)deleteList:(id)sender;
- (void)changeMembership:(NSMenuItem*)sender;
- (void)presentEditor;
- (void)refreshEditor;
- (void)playSelection:(id) [[maybe_unused]] sender;
- (std::int32_t)exitCode;
@end

@interface AobusDesktopDelegate (Shell)<NSSearchFieldDelegate>
- (void)buildWindow;
- (void)layoutContent;
- (void)windowDidResize:(NSNotification*) [[maybe_unused]] notification;
- (NSSize)windowWillResize:(NSWindow*) [[maybe_unused]] window toSize:(NSSize)size;
- (void)splitViewDidResizeSubviews:(NSNotification*) [[maybe_unused]] notification;
- (void)selectMode:(NSMenuItem*)sender;
- (void)selectAppearance:(NSMenuItem*)sender;
- (void)toggleInspector:(id) [[maybe_unused]] sender;
- (void)dismissInspector:(id) [[maybe_unused]] sender;
- (void)toggleSidebar:(id) [[maybe_unused]] sender;
- (void)focusSearch:(id) [[maybe_unused]] sender;
- (void)selectPresentation:(id) [[maybe_unused]] sender;
- (void)emptyAction:(id) [[maybe_unused]] sender;
- (void)filterTracks:(id) [[maybe_unused]] sender;
- (void)showVolume:(id) [[maybe_unused]] sender;
@end

@interface AobusDesktopDelegate (
  CommandSurface)<NSToolbarDelegate, NSMenuDelegate, NSMenuItemValidation, NSToolbarItemValidation>
- (void)buildMenu;
- (NSArray<NSToolbarItemIdentifier>*)toolbarAllowedItemIdentifiers:(NSToolbar*) [[maybe_unused]] toolbar;
- (NSArray<NSToolbarItemIdentifier>*)toolbarDefaultItemIdentifiers:(NSToolbar*) [[maybe_unused]] toolbar;
- (NSSet<NSToolbarItemIdentifier>*)toolbarImmovableItemIdentifiers:(NSToolbar*) [[maybe_unused]] toolbar;
- (NSToolbarItem*)toolbar:(NSToolbar*) [[maybe_unused]] toolbar
      itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
  willBeInsertedIntoToolbar:(BOOL)flag;
- (void)toolbarWillAddItem:(NSNotification*)notification;
- (void)toolbarDidRemoveItem:(NSNotification*)notification;
- (void)showPlaybackOptions:(id) [[maybe_unused]] sender;
- (void)showOutput:(id) [[maybe_unused]] sender;
- (void)refreshRecentLibrariesMenu;
- (void)openHelp:(id) [[maybe_unused]] sender;
- (void)customizeToolbar:(id) [[maybe_unused]] sender;
- (void)persistToolbarConfiguration;
- (void)menuNeedsUpdate:(NSMenu*)menu;
- (void)menuWillOpen:(NSMenu*) [[maybe_unused]] menu;
- (void)menuDidClose:(NSMenu*) [[maybe_unused]] menu;
- (BOOL)tryValidateAuthoringMenuItem:(NSMenuItem*)item result:(BOOL*)result;
- (BOOL)tryValidateViewMenuItem:(NSMenuItem*)item result:(BOOL*)result;
- (BOOL)validateMenuItem:(NSMenuItem*)item;
- (BOOL)validateToolbarItem:(NSToolbarItem*)item;
@end
