// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitText.h"
#include "DesktopApplicationInternal.h"
#include "LibraryBrowser.h"
#include "LibrarySession.h"
#include "PlaybackBar.h"
#include "TrackInspector.h"
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/uimodel/playback/command/PlaybackCommandText.h>

#import <AppKit/AppKit.h>
#include <CoreFoundation/CFRunLoop.h>

#include <array>
#include <cstddef>

namespace
{
  using ao::appkit::kBodyFontSize;
  using ao::appkit::kCaptionFontSize;
  using ao::appkit::label;
  using ao::appkit::nativeCallback;
  using ao::appkit::nativeText;
  using ao::i18n::MessageId;
  constexpr auto kToolbarSearchWidth = 240;

  NSButton* symbolButton(NSString* symbol, NSString* title, id target, SEL action)
  {
    auto* const image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    auto* const button = image != nil ? [NSButton buttonWithImage:image target:target action:action]
                                      : [NSButton buttonWithTitle:title target:target action:action];
    button.bordered = NO;
    button.toolTip = title;
    button.accessibilityLabel = title;
    return button;
  }
} // namespace

@implementation AobusDesktopDelegate (CommandSurface)
- (void)buildMenu
{
  auto* const main = [[NSMenu alloc] initWithTitle:@"Aobus"];
  auto addMenu = [&](NSString* title)
  {
    auto* const item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.submenu = [[NSMenu alloc] initWithTitle:title];
    [main addItem:item];
    return item.submenu;
  };
  auto addItem = [&](NSMenu* menu, NSString* title, SEL action, NSString* key)
  {
    auto* const item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.target = self;
    return item;
  };
  auto addStandardItem = [](NSMenu* menu,
                            NSString* title,
                            SEL action,
                            NSString* key,
                            NSEventModifierFlags modifiers = NSEventModifierFlagCommand)
  {
    auto* const item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.target = nil;

    if (key.length > 0)
    {
      item.keyEquivalentModifierMask = modifiers;
    }

    return item;
  };
  auto* const app = addMenu(@"Aobus");
  addStandardItem(app, [self text:MessageId::AppKitAbout], @selector(orderFrontStandardAboutPanel:), @"");
  [app addItem:NSMenuItem.separatorItem];
  auto* const servicesItem = [app addItemWithTitle:[self text:MessageId::AppKitServices] action:nil keyEquivalent:@""];
  servicesItem.submenu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitServices]];
  NSApp.servicesMenu = servicesItem.submenu;
  [app addItem:NSMenuItem.separatorItem];
  addStandardItem(app, [self text:MessageId::AppKitHideApplication], @selector(hide:), @"h");
  addStandardItem(app,
                  [self text:MessageId::AppKitHideOthers],
                  @selector(hideOtherApplications:),
                  @"h",
                  NSEventModifierFlagCommand | NSEventModifierFlagOption);
  addStandardItem(app, [self text:MessageId::AppKitShowAll], @selector(unhideAllApplications:), @"");
  [app addItem:NSMenuItem.separatorItem];
  addStandardItem(app, [self text:MessageId::AppKitQuit], @selector(terminate:), @"q");
  auto* const file = addMenu([self text:MessageId::AppKitMenuFile]);
  addItem(file, [self text:MessageId::AppKitOpenLibraryAction], @selector(openLibrary:), @"o");
  auto* const recent = [file addItemWithTitle:[self text:MessageId::AppKitOpenRecent] action:nil keyEquivalent:@""];
  _recentLibrariesMenu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitOpenRecent]];
  _recentLibrariesMenu.delegate = self;
  recent.submenu = _recentLibrariesMenu;
  addItem(file, [self text:MessageId::AppKitRescanLibrary], @selector(rescan:), @"r");
  [file addItem:NSMenuItem.separatorItem];
  addStandardItem(file, [self text:MessageId::AppKitCloseWindow], @selector(performClose:), @"w");
  auto* const edit = addMenu([self text:MessageId::AppKitMenuEdit]);
  addStandardItem(edit, [self text:MessageId::AppKitUndo], @selector(undo:), @"z");
  addStandardItem(edit,
                  [self text:MessageId::AppKitRedo],
                  @selector(redo:),
                  @"z",
                  NSEventModifierFlagCommand | NSEventModifierFlagShift);
  [edit addItem:NSMenuItem.separatorItem];
  addStandardItem(edit, [self text:MessageId::AppKitCut], @selector(cut:), @"x");
  addStandardItem(edit, [self text:MessageId::AppKitCopy], @selector(copy:), @"c");
  addStandardItem(edit, [self text:MessageId::AppKitPaste], @selector(paste:), @"v");
  addStandardItem(edit,
                  [self text:MessageId::AppKitPasteMatchStyle],
                  @selector(pasteAsPlainText:),
                  @"v",
                  NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagShift);
  addStandardItem(edit, [self text:MessageId::AppKitDelete], @selector(delete:), @"");
  addStandardItem(edit, [self text:MessageId::AppKitSelectAll], @selector(selectAll:), @"a");
  [edit addItem:NSMenuItem.separatorItem];
  addItem(edit, [self text:MessageId::AppKitFilterTracks], @selector(focusSearch:), @"f");
  addItem(edit, [self text:MessageId::AppKitPropertiesAction], @selector(showProperties:), @"i");
  auto* const lists = addMenu([self text:MessageId::AppKitLists]);
  addItem(lists, [self text:MessageId::AppKitNewListAction], @selector(newList:), @"n");
  addItem(lists, [self text:MessageId::AppKitNewChildListAction], @selector(newChildList:), @"");
  addItem(lists, [self text:MessageId::AppKitEditListAction], @selector(editList:), @"");
  addItem(lists, [self text:MessageId::AppKitDeleteListAction], @selector(deleteList:), @"");
  auto* const view = addMenu([self text:MessageId::AppKitMenuView]);
  auto* const modern = addItem(view, [self text:MessageId::AppKitModern], @selector(selectMode:), @"1");
  modern.tag = 1;
  addItem(view, [self text:MessageId::AppKitClassic], @selector(selectMode:), @"2");
  [view addItem:NSMenuItem.separatorItem];
  auto* const inspector = addItem(view, [self text:MessageId::AppKitShowInspector], @selector(toggleInspector:), @"i");
  inspector.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  auto* const sidebar = addItem(view, [self text:MessageId::AppKitHideSidebar], @selector(toggleSidebar:), @"s");
  sidebar.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  addItem(view, [self text:MessageId::AppKitShowActivity], @selector(showActivity:), @"");
  addStandardItem(view,
                  [self text:MessageId::AppKitEnterFullScreen],
                  @selector(toggleFullScreen:),
                  @"f",
                  NSEventModifierFlagCommand | NSEventModifierFlagControl);
  addItem(view, [self text:MessageId::AppKitCustomizeToolbar], @selector(customizeToolbar:), @"");
  [view addItem:NSMenuItem.separatorItem];

  for (auto const& title : std::array<NSString*, 3>{@"Follow System", @"Light", @"Dark"})
  {
    auto message = MessageId::AppKitAppearanceSystem;

    if ([title isEqual:@"Light"] != NO)
    {
      message = MessageId::AppKitAppearanceLight;
    }
    else if ([title isEqual:@"Dark"] != NO)
    {
      message = MessageId::AppKitAppearanceDark;
    }

    auto* const item = addItem(view, [self text:message], @selector(selectAppearance:), @"");
    item.representedObject = title;
  }

  _playbackMenu = addMenu([self text:MessageId::AppKitMenuPlayback]);

  for (auto command : ao::uimodel::playbackCommands())
  {
    auto* const item = addItem(_playbackMenu,
                               nativeText(ao::uimodel::playbackActionLabel(_sessionPtr->catalog(), command)),
                               @selector(transport:),
                               @"");
    item.tag = static_cast<NSInteger>(command);
  }

  addItem(_playbackMenu, [self text:MessageId::AppKitOutputDeviceAction], @selector(showOutput:), @"");
  auto* const window = addMenu([self text:MessageId::AppKitMenuWindow]);
  addStandardItem(window, [self text:MessageId::AppKitMinimize], @selector(performMiniaturize:), @"m");
  addStandardItem(window, [self text:MessageId::AppKitZoom], @selector(performZoom:), @"");
  [window addItem:NSMenuItem.separatorItem];
  addItem(window, [self text:MessageId::AppKitShowWindow], @selector(showMainWindow:), @"0");
  addStandardItem(window, [self text:MessageId::AppKitBringAllToFront], @selector(arrangeInFront:), @"");
  NSApp.windowsMenu = window;
  auto* const help = addMenu([self text:MessageId::AppKitMenuHelp]);
  addItem(help, [self text:MessageId::AppKitHelp], @selector(openHelp:), @"");
  NSApp.helpMenu = help;
  NSApp.mainMenu = main;
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarAllowedItemIdentifiers:(NSToolbar*) [[maybe_unused]] toolbar
{
  return @[
    NSToolbarToggleSidebarItemIdentifier,
    NSToolbarSidebarTrackingSeparatorItemIdentifier,
    @"destination",
    NSToolbarFlexibleSpaceItemIdentifier,
    NSToolbarSpaceItemIdentifier,
    @"search",
    @"activity",
    @"open",
    NSToolbarInspectorTrackingSeparatorItemIdentifier,
    NSToolbarToggleInspectorItemIdentifier
  ];
}

- (NSArray<NSToolbarItemIdentifier>*)toolbarDefaultItemIdentifiers:(NSToolbar*) [[maybe_unused]] toolbar
{
  return @[
    NSToolbarToggleSidebarItemIdentifier,
    NSToolbarSidebarTrackingSeparatorItemIdentifier,
    @"destination",
    NSToolbarFlexibleSpaceItemIdentifier,
    @"search",
    @"activity",
    NSToolbarInspectorTrackingSeparatorItemIdentifier,
    NSToolbarToggleInspectorItemIdentifier
  ];
}

- (NSSet<NSToolbarItemIdentifier>*)toolbarImmovableItemIdentifiers:(NSToolbar*) [[maybe_unused]] toolbar
{
  return [NSSet setWithArray:@[
    NSToolbarToggleSidebarItemIdentifier,
    NSToolbarSidebarTrackingSeparatorItemIdentifier,
    @"destination",
    NSToolbarInspectorTrackingSeparatorItemIdentifier
  ]];
}

- (NSToolbarItem*)toolbar:(NSToolbar*) [[maybe_unused]] toolbar
      itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
  willBeInsertedIntoToolbar:(BOOL)flag
{
  if ([identifier isEqual:@"search"] != NO)
  {
    auto* const search = [[NSSearchField alloc] initWithFrame:NSZeroRect];
    search.placeholderString = [self text:MessageId::AppKitFilterPlaceholder];
    search.target = self;
    search.action = @selector(filterTracks:);
    search.delegate = self;
    search.sendsSearchStringImmediately = YES;
    auto* const item = [[NSSearchToolbarItem alloc] initWithItemIdentifier:identifier];
    item.label = [self text:MessageId::AppKitFilter];
    item.paletteLabel = [self text:MessageId::AppKitFilterThisView];
    item.searchField = search;
    item.preferredWidthForSearchField = kToolbarSearchWidth;

    if (flag != NO)
    {
      _toolbarSearch = search;
      _toolbarSearchItem = item;
    }

    return item;
  }

  auto* const item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
  item.target = self;

  if ([identifier isEqual:@"destination"] != NO)
  {
    auto* const title = label([self text:MessageId::LibraryAllTracks], kBodyFontSize);
    title.font = [NSFont systemFontOfSize:kBodyFontSize weight:NSFontWeightSemibold];
    auto* const count = label(@"", kCaptionFontSize, YES);
    auto* const stack = [NSStackView stackViewWithViews:@[title, count]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 0;
    item.label = [self text:MessageId::AppKitCurrentView];
    item.paletteLabel = [self text:MessageId::AppKitCurrentView];
    item.view = stack;

    if (flag != NO)
    {
      _toolbarTitle = title;
      _toolbarCount = count;
    }
  }
  else if ([identifier isEqual:@"activity"] != NO)
  {
    auto* const activityButton =
      symbolButton(@"checkmark.circle", [self text:MessageId::AppKitActivity], self, @selector(showActivity:));
    item.label = [self text:MessageId::AppKitActivity];
    item.paletteLabel = [self text:MessageId::AppKitActivity];
    item.view = activityButton;

    if (flag != NO)
    {
      _activityButton = activityButton;
      _toolbarActivityItem = item;
    }
  }
  else if ([identifier isEqual:@"open"] != NO)
  {
    item.label = [self text:MessageId::AppKitOpenLibrary];
    item.paletteLabel = [self text:MessageId::AppKitOpenLibrary];
    item.image = [NSImage imageWithSystemSymbolName:@"folder.badge.plus" accessibilityDescription:item.label];
    item.action = @selector(openLibrary:);
  }
  else
  {
    return nil;
  }

  return item;
}

- (void)toolbarWillAddItem:(NSNotification*)notification
{
  auto* const item = static_cast<NSToolbarItem*>(notification.userInfo[@"item"]);

  if ([item.itemIdentifier isEqual:NSToolbarToggleSidebarItemIdentifier] != NO)
  {
    item.target = self;
    item.action = @selector(toggleSidebar:);
  }
  else if ([item.itemIdentifier isEqual:NSToolbarToggleInspectorItemIdentifier] != NO)
  {
    item.target = self;
    item.action = @selector(toggleInspector:);
  }

  [self persistToolbarConfiguration];
}

- (void)toolbarDidRemoveItem:(NSNotification*)notification
{
  auto* const item = static_cast<NSToolbarItem*>(notification.userInfo[@"item"]);

  if (item == _toolbarSearchItem)
  {
    _toolbarSearchItem = nil;
    _toolbarSearch = nil;
  }
  else if (item == _toolbarActivityItem)
  {
    _toolbarActivityItem = nil;
    _activityButton = nil;
  }

  [self persistToolbarConfiguration];
}

- (void)showPlaybackOptions:(id) [[maybe_unused]] sender
{
  if (_closing != NO)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      NSMenu* const menu = [_playbackMenu copy];
      _presentingMenu = YES;
      [_playbackBar presentPlaybackOptionsMenu:menu];
      _presentingMenu = NO;
      [self requestService];
    });
}

- (void)showOutput:(id) [[maybe_unused]] sender
{
  if (_closing != NO)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto* const menu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitOutputDevice]];
      auto const& rows = _sessionPtr->state().output.rows;

      for (auto const& row : rows)
      {
        auto* const item = [menu addItemWithTitle:nativeText(row.title)
                                           action:@selector(selectOutput:)
                                    keyEquivalent:@""];
        item.target = self;
        item.representedObject = @{
          @"backend": nativeText(row.backendId.raw()),
          @"device": nativeText(row.deviceId.raw()),
          @"profile": nativeText(row.profileId.raw())
        };
        item.enabled = static_cast<BOOL>(row.kind == ao::uimodel::OutputDeviceRow::Kind::DeviceProfile);
        item.state = row.isActive ? NSControlStateValueOn : NSControlStateValueOff;
      }

      menu.autoenablesItems = NO;
      _presentingMenu = YES;
      [_playbackBar presentOutputMenu:menu];
      _presentingMenu = NO;
      [self requestService];
    });
}

- (void)refreshRecentLibrariesMenu
{
  if (_recentLibrariesMenu == nil)
  {
    return;
  }

  [_recentLibrariesMenu removeAllItems];
  NSArray* const saved = _settings[@"recentLibraries"];

  if ([saved isKindOfClass:NSArray.class] == NO || saved.count == 0)
  {
    auto* const empty = [_recentLibrariesMenu addItemWithTitle:[self text:MessageId::AppKitNoRecentLibraries]
                                                        action:nil
                                                 keyEquivalent:@""];
    empty.enabled = NO;
    return;
  }

  for (NSUInteger index = 0; index < saved.count; ++index)
  {
    id const value = saved[index];

    if ([value isKindOfClass:NSString.class] == NO)
    {
      continue;
    }

    NSString* const path = value;
    auto* const item = [_recentLibrariesMenu addItemWithTitle:path.lastPathComponent
                                                       action:@selector(openRecentLibrary:)
                                                keyEquivalent:@""];
    item.target = self;
    item.representedObject = path;
    item.toolTip = path;
  }

  if (_recentLibrariesMenu.numberOfItems == 0)
  {
    auto* const empty = [_recentLibrariesMenu addItemWithTitle:[self text:MessageId::AppKitNoRecentLibraries]
                                                        action:nil
                                                 keyEquivalent:@""];
    empty.enabled = NO;
    return;
  }

  [_recentLibrariesMenu addItem:NSMenuItem.separatorItem];
  auto* const clear = [_recentLibrariesMenu addItemWithTitle:[self text:MessageId::AppKitClearRecentMenu]
                                                      action:@selector(clearRecentLibraries:)
                                               keyEquivalent:@""];
  clear.target = self;
}

- (void)openHelp:(id) [[maybe_unused]] sender
{
  auto* const url = [NSURL URLWithString:@"https://github.com/czrocklee/Aobus#readme"];

  if (url != nil)
  {
    [NSWorkspace.sharedWorkspace openURL:url];
  }
}

- (void)customizeToolbar:(id) [[maybe_unused]] sender
{
  if (_closing == NO && _window.attachedSheet == nil)
  {
    [_window.toolbar runCustomizationPalette:self];
  }
}

- (void)persistToolbarConfiguration
{
  __weak AobusDesktopDelegate* weakSelf = self;
  ::dispatch_async(::dispatch_get_main_queue(), ^{
    auto* const owner = weakSelf;

    if (owner == nil || owner->_settings == nil || owner->_closing != NO)
    {
      return;
    }

    auto* const toolbar = owner->_window.toolbar;

    if (auto* const identifiers = toolbar.itemIdentifiers; identifiers != nil)
    {
      owner->_settings[@"toolbarConfiguration"] =
        @{@"itemIdentifiers": identifiers,
          @"displayMode": @(toolbar.displayMode)};
      [owner saveSettings];
    }
  });
}

- (void)menuNeedsUpdate:(NSMenu*)menu
{
  nativeCallback(
    [&]
    {
      if (menu == _recentLibrariesMenu)
      {
        [self refreshRecentLibrariesMenu];
        return;
      }

      [menu removeAllItems];

      if ([_libraryBrowser prepareContextMenu:menu] == 0)
      {
        return;
      }

      auto add = [&](NSString* title, SEL action)
      {
        auto* const item = [menu addItemWithTitle:title action:action keyEquivalent:@""];
        item.target = self;
        return item;
      };

      if (menu == _listMenu)
      {
        add([self text:MessageId::AppKitNewListAction], @selector(newList:));
        auto const listId = [_libraryBrowser clickedListId];
        auto const authoringListId = ao::rt::isVirtualListId(listId) ? ao::kInvalidListId : listId;
        auto* const payload = @{@"list": @(authoringListId.raw())};
        auto* const child = add([self text:MessageId::AppKitNewChildListAction], @selector(newChildList:));
        child.representedObject = payload;
        auto* const edit = add([self text:MessageId::AppKitEditListAction], @selector(editList:));
        edit.representedObject = payload;
        auto* const remove = add([self text:MessageId::AppKitDeleteListAction], @selector(deleteList:));
        remove.representedObject = payload;
        return;
      }

      auto* const tracks = [NSMutableArray array];

      for (auto id : [_libraryBrowser selectedTrackIds])
      {
        [tracks addObject:@(id.raw())];
      }

      add([self text:MessageId::PlaybackControlPlay], @selector(playSelection:));
      auto* const properties = add([self text:MessageId::AppKitPropertiesAction], @selector(showProperties:));
      properties.representedObject = @{@"tracks": tracks};
      auto* const reveal = add([self text:MessageId::AppKitRevealInFinder], @selector(revealSelection:));
      reveal.representedObject = @{@"tracks": tracks};
      auto* const targets = add([self text:MessageId::AppKitAddToList], nil);
      targets.submenu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitAddToList]];

      for (auto const& target : ao::uimodel::writableTagListTargets(
             _sessionPtr->runtime().library().snapshot().lists(), _sessionPtr->runtime().textOrderingPolicy()))
      {
        auto* const item = [targets.submenu addItemWithTitle:nativeText(target.name)
                                                      action:@selector(changeMembership:)
                                               keyEquivalent:@""];
        item.target = self;
        item.representedObject = @{@"list": @(target.listId.raw()), @"tracks": tracks};

        if (target.listId == [self activeListId])
        {
          auto* const remove = add([self text:MessageId::AppKitRemoveFromCurrentList], @selector(changeMembership:));
          remove.tag = 1;
          remove.representedObject = item.representedObject;
        }
      }
    });
}

- (void)menuWillOpen:(NSMenu*) [[maybe_unused]] menu
{
  _presentingMenu = YES;
}

- (void)menuDidClose:(NSMenu*) [[maybe_unused]] menu
{
  _presentingMenu = NO;
  [self requestService];
}

- (BOOL)tryValidateAuthoringMenuItem:(NSMenuItem*)item result:(BOOL*)result
{
  auto* const action = item.action;
  auto const canEdit =
    _window.attachedSheet == nil && _sessionPtr->editor().state().kind == ao::appkit::LibraryEditorKind::None;

  if (action == @selector(showProperties:) || action == @selector(changeMembership:))
  {
    *result = static_cast<BOOL>(canEdit && ![self capturedTracks:item].empty());
  }
  else if (action == @selector(newList:))
  {
    *result = static_cast<BOOL>(canEdit);
  }
  else if (action == @selector(newChildList:) || action == @selector(editList:) || action == @selector(deleteList:))
  {
    *result = static_cast<BOOL>(
      canEdit && _sessionPtr->runtime().library().snapshot().listNode([self capturedList:item]).has_value());
  }
  else
  {
    return NO;
  }

  return YES;
}

- (BOOL)tryValidateViewMenuItem:(NSMenuItem*)item result:(BOOL*)result
{
  auto* const action = item.action;
  auto const canEdit =
    _window.attachedSheet == nil && _sessionPtr->editor().state().kind == ao::appkit::LibraryEditorKind::None;

  if (action == @selector(selectMode:))
  {
    item.state = static_cast<BOOL>(item.tag != 0) == _modern ? NSControlStateValueOn : NSControlStateValueOff;
    *result = static_cast<BOOL>(canEdit);
  }
  else if (action == @selector(selectAppearance:))
  {
    NSString* const selected = _settings[@"appearance"] != nil ? _settings[@"appearance"] : @"Follow System";
    item.state = ([item.representedObject isEqual:selected] != NO) ? NSControlStateValueOn : NSControlStateValueOff;
    *result = YES;
  }
  else if (action == @selector(toggleSidebar:))
  {
    item.title = _sidebarItem.collapsed != NO ? [self text:MessageId::AppKitShowSidebar]
                                              : [self text:MessageId::AppKitHideSidebar];
    *result = static_cast<BOOL>(_window.attachedSheet == nil);
  }
  else if (action == @selector(toggleInspector:))
  {
    auto const shown = _trackInspector.sheet != nil || (_inspectorVisible != NO && _root.bounds.size.width >= 1000);
    item.title = shown ? [self text:MessageId::AppKitHideInspector] : [self text:MessageId::AppKitShowInspector];
    *result = static_cast<BOOL>(_window.attachedSheet == nil || _trackInspector.sheet != nil);
  }
  else if (action == @selector(showActivity:))
  {
    item.title = _activityPopover.shown != NO ? [self text:MessageId::AppKitHideActivity]
                                              : [self text:MessageId::AppKitShowActivity];
    *result = static_cast<BOOL>(_window.attachedSheet == nil);
  }
  else if (action == @selector(showMainWindow:))
  {
    *result = static_cast<BOOL>(_closing == NO && !_optPendingSuccessor);
  }
  else
  {
    return NO;
  }

  return YES;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
  BOOL enabled = NO;
  nativeCallback(
    [&]
    {
      if (_closing != NO || _optPendingSuccessor || !_sessionPtr)
      {
        return;
      }

      if ([self tryValidateAuthoringMenuItem:item result:&enabled] != 0)
      {
        return;
      }

      if ([self tryValidateViewMenuItem:item result:&enabled] != 0)
      {
        return;
      }

      if (auto* const action = item.action; action == @selector(openLibrary:) ||
                                            action == @selector(openRecentLibrary:) ||
                                            action == @selector(clearRecentLibraries:))
      {
        enabled = static_cast<BOOL>(_window.attachedSheet == nil);
      }
      else if (action == @selector(transport:))
      {
        enabled = static_cast<BOOL>(_sessionPtr->state().transport.at(static_cast<std::size_t>(item.tag)).enabled);
      }
      else if (action == @selector(revealSelection:))
      {
        enabled = static_cast<BOOL>([self revealURL:item] != nil);
      }
      else
      {
        enabled = YES;
      }
    });
  return enabled;
}

- (BOOL)validateToolbarItem:(NSToolbarItem*)item
{
  if (_closing != NO || _optPendingSuccessor || !_sessionPtr)
  {
    return NO;
  }

  if ([item.itemIdentifier isEqual:@"open"] != NO)
  {
    return static_cast<BOOL>(_window.attachedSheet == nil);
  }

  if ([item.itemIdentifier isEqual:NSToolbarToggleSidebarItemIdentifier] != NO)
  {
    item.label = _sidebarItem.collapsed != NO ? [self text:MessageId::AppKitShowSidebar]
                                              : [self text:MessageId::AppKitHideSidebar];
  }
  else if ([item.itemIdentifier isEqual:NSToolbarToggleInspectorItemIdentifier] != NO)
  {
    auto const shown = _trackInspector.sheet != nil || (_inspectorVisible != NO && _root.bounds.size.width >= 1000);
    item.label = shown ? [self text:MessageId::AppKitHideInspector] : [self text:MessageId::AppKitShowInspector];
  }

  return YES;
}

@end
