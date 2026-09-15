// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitText.h"
#include "ArtworkView.h"
#include "DesktopApplicationInternal.h"
#include "DesktopControls.h"
#include "LibraryBrowser.h"
#include "LibrarySession.h"
#include "PlaybackBar.h"
#include "TrackInspector.h"
#include <ao/rt/VirtualListIds.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>

namespace
{
  using ao::appkit::kBodyFontSize;
  using ao::appkit::kCaptionFontSize;
  using ao::appkit::kContentInset;
  using ao::appkit::kInitialWindowHeight;
  using ao::appkit::kInitialWindowWidth;
  using ao::appkit::kMinimumWindowHeight;
  using ao::appkit::kMinimumWindowWidth;
  using ao::appkit::label;
  using ao::appkit::nativeAppearance;
  using ao::appkit::nativeCallback;
  using ao::appkit::nativeText;
  using ao::appkit::utf8;
  using ao::i18n::MessageId;
  constexpr auto kInspectorSheetWidth = 420;
  constexpr auto kClassicStatusAreaHeight = 24;
  constexpr auto kEmptyActionWidth = 152;
  constexpr auto kEmptyActionOffset = 76;
  constexpr auto kPresentationWidth = 116;
  constexpr auto kClassicPresentationTrailing = 132;
  constexpr auto kClassicSearchTrailing = 152;
  constexpr auto kClassicSearchLeading = 10;
  constexpr auto kClassicBrowserControlTop = 13;
  constexpr auto kClassicBrowserHeaderHeight = 50;
  constexpr auto kClassicSidebarVerticalInsets = 110;
  constexpr auto kClassicSidebarTop = 42;
  constexpr auto kSidebarArtworkHeight = 50;
  constexpr auto kSidebarRowHeight = 34;
  constexpr auto kSidebarLabelHeight = 18;
  constexpr auto kSidebarLabelLeading = 18;
  constexpr auto kPreferredInspectorFraction = 0.22;
  constexpr auto kMinimumInspectorWidth = 230;
  constexpr auto kMaximumSidebarWidth = 300.0;
  constexpr auto kInitialSidebarWidth = 210;
  constexpr auto kMinimumSidebarWidth = 160.0;
  constexpr auto kMinimumBrowserWidth = 430.0;
  constexpr auto kInitialBrowserWidth = 740;
  constexpr auto kInitialPaneHeight = 600;
  constexpr auto kControlHeight = 28;
  constexpr auto kLargeTitleFontSize = 20;
  constexpr auto kSecondaryFontSize = 11;
  constexpr auto kOuterInset = 20;
  constexpr auto kContentGap = 12;
  constexpr auto kCellInset = 6;

  NSViewController* viewController(NSView* view)
  {
    auto* const controller = [[NSViewController alloc] init];
    controller.view = view;
    return controller;
  }

  void detachView(NSView* view)
  {
    if ([view.superview isKindOfClass:NSStackView.class] != NO)
    {
      [static_cast<NSStackView*>(view.superview) removeArrangedSubview:view];
    }

    [view removeFromSuperview];
  }

  void setFrame(NSView* view, CGFloat left, CGFloat top, CGFloat width, CGFloat height)
  {
    view.frame = NSMakeRect(left, top, std::max(0.0, width), std::max(0.0, height));
  }

  NSRect clampedWindowFrame(NSRect frame)
  {
    NSScreen* target = nil;
    CGFloat largestIntersection = 0;

    auto* const screens = NSScreen.screens;

    for (NSUInteger index = 0; index < screens.count; ++index)
    {
      auto* const screen = screens[index];
      auto const intersection = ::NSIntersectionRect(frame, screen.visibleFrame);
      auto const area = intersection.size.width * intersection.size.height;

      if (area > largestIntersection)
      {
        largestIntersection = area;
        target = screen;
      }
    }

    if (target == nil)
    {
      target = NSScreen.mainScreen;

      if (target == nil)
      {
        target = NSScreen.screens.firstObject;
      }
    }

    if (target == nil)
    {
      return frame;
    }

    auto const visible = target.visibleFrame;
    frame.size.width =
      std::min(std::max(frame.size.width, static_cast<CGFloat>(kMinimumWindowWidth)), visible.size.width);
    frame.size.height =
      std::min(std::max(frame.size.height, static_cast<CGFloat>(kMinimumWindowHeight)), visible.size.height);
    frame.origin.x = std::clamp(frame.origin.x, NSMinX(visible), NSMaxX(visible) - frame.size.width);
    frame.origin.y = std::clamp(frame.origin.y, NSMinY(visible), NSMaxY(visible) - frame.size.height);
    return frame;
  }
} // namespace

@implementation AobusDesktopDelegate (Shell)
- (void)buildWindow
{
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kInitialWindowWidth, kInitialWindowHeight)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"Aobus";
  _window.styleMask |= NSWindowStyleMaskFullSizeContentView;
  _window.minSize = NSMakeSize(kMinimumWindowWidth, kMinimumWindowHeight);
  _window.releasedWhenClosed = NO;
  _window.restorable = NO;
  _window.delegate = self;
  _window.titlebarAppearsTransparent = YES;
  [_window center];

  if (NSString* const savedFrame = _settings[@"windowFrame"]; [savedFrame isKindOfClass:NSString.class] != NO)
  {
    if (auto const frame = ::NSRectFromString(savedFrame);
        frame.size.width >= kMinimumWindowWidth && frame.size.height >= kMinimumWindowHeight)
    {
      [_window setFrame:clampedWindowFrame(frame) display:NO];
    }
  }

  _root = [[AobusRootSurface alloc] initWithFrame:NSMakeRect(0, 0, kInitialWindowWidth, kInitialWindowHeight)];
  _root.material = NSVisualEffectMaterialWindowBackground;
  _root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  _root.libraryDropDestination = self;
  [_root registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
  _rootController = viewController(_root);
  _window.contentViewController = _rootController;
  _splitController = [[NSSplitViewController alloc] init];
  [_rootController addChildViewController:_splitController];
  _split = _splitController.splitView;
  _split.vertical = YES;
  _split.dividerStyle = NSSplitViewDividerStyleThin;
  _splitController.view.translatesAutoresizingMaskIntoConstraints = NO;
  [_root addSubview:_splitController.view];
  _splitTopConstraint = [_splitController.view.topAnchor constraintEqualToAnchor:_root.topAnchor];
  _splitBottomConstraint = [_splitController.view.bottomAnchor constraintEqualToAnchor:_root.bottomAnchor];
  [NSLayoutConstraint activateConstraints:@[
    [_splitController.view.leadingAnchor constraintEqualToAnchor:_root.leadingAnchor],
    [_splitController.view.trailingAnchor constraintEqualToAnchor:_root.trailingAnchor],
    _splitTopConstraint,
    _splitBottomConstraint
  ]];
  [NSNotificationCenter.defaultCenter addObserver:self
                                         selector:@selector(splitViewDidResizeSubviews:)
                                             name:NSSplitViewDidResizeSubviewsNotification
                                           object:_split];

  if (@available(macOS 26.0, *))
  {
    _sidebar = [[AobusFlippedView alloc] initWithFrame:NSMakeRect(0, 0, kInitialSidebarWidth, kInitialPaneHeight)];
  }
  else
  {
    auto* const sidebar =
      [[AobusSurface alloc] initWithFrame:NSMakeRect(0, 0, kInitialSidebarWidth, kInitialPaneHeight)];
    sidebar.material = NSVisualEffectMaterialSidebar;
    sidebar.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    _sidebar = sidebar;
  }

  _browser = [[AobusSurface alloc] initWithFrame:NSMakeRect(0, 0, kInitialBrowserWidth, kInitialPaneHeight)];
  _browser.material = NSVisualEffectMaterialWindowBackground;

  __weak AobusDesktopDelegate* inspectorHost = self;
  _trackInspector = [[AobusTrackInspector alloc] initWithCatalog:_sessionPtr->catalog()
    revealHandler:^{
      if (AobusDesktopDelegate* const host = inspectorHost; host != nil && host->_closing == NO && host->_sessionPtr)
      {
        [host revealSelection:nil];
      }
    }
    propertiesHandler:^{
      if (AobusDesktopDelegate* const host = inspectorHost; host != nil && host->_closing == NO && host->_sessionPtr)
      {
        [host showProperties:nil];
      }
    }
    dismissHandler:^{ [inspectorHost requestService]; }];
  _inspector = _trackInspector.view;

  _sidebarItem = [NSSplitViewItem sidebarWithViewController:viewController(_sidebar)];
  _sidebarItem.minimumThickness = kMinimumSidebarWidth;
  _sidebarItem.maximumThickness = kMaximumSidebarWidth;
  _sidebarItem.allowsFullHeightLayout = YES;
  _sidebarItem.canCollapse = YES;
  _sidebarItem.canCollapseFromWindowResize = NO;
  _sidebarItem.collapseBehavior = NSSplitViewItemCollapseBehaviorPreferResizingSiblingsWithFixedSplitView;
  _sidebarItem.holdingPriority = NSLayoutPriorityDefaultHigh + 1;
  _browserItem = [NSSplitViewItem splitViewItemWithViewController:viewController(_browser)];
  _browserItem.minimumThickness = kMinimumBrowserWidth;
  _browserItem.canCollapse = NO;
  _browserItem.holdingPriority = NSLayoutPriorityDefaultLow;
  _inspectorItem = [NSSplitViewItem inspectorWithViewController:_trackInspector];
  _inspectorItem.minimumThickness = kMinimumInspectorWidth;
  _inspectorItem.maximumThickness = kInspectorSheetWidth;
  _inspectorItem.allowsFullHeightLayout = YES;
  _inspectorItem.preferredThicknessFraction = kPreferredInspectorFraction;
  _inspectorItem.canCollapse = YES;
  _inspectorItem.canCollapseFromWindowResize = YES;
  _inspectorItem.collapseBehavior = NSSplitViewItemCollapseBehaviorPreferResizingSiblingsWithFixedSplitView;
  _inspectorItem.collapsed = static_cast<BOOL>(_inspectorVisible == NO);
  [_splitController addSplitViewItem:_sidebarItem];
  [_splitController addSplitViewItem:_browserItem];
  [_splitController addSplitViewItem:_inspectorItem];
  _libraryLabel = label([[self text:MessageId::AppKitLibrarySection] uppercaseString], kCaptionFontSize, YES);
  _libraryLabel.font = [NSFont systemFontOfSize:kCaptionFontSize weight:NSFontWeightSemibold];
  [_sidebar addSubview:_libraryLabel];
  _libraryBrowser = [[AobusLibraryBrowser alloc] initWithSession:*_sessionPtr delegate:self];
  _listMenu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitLists]];
  _listMenu.delegate = self;
  _libraryBrowser.lists.menu = _listMenu;
  _trackMenu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitTracks]];
  _trackMenu.delegate = self;
  _libraryBrowser.tracks.menu = _trackMenu;
  [_sidebar addSubview:_libraryBrowser.listScroll];
  [_browser addSubview:_libraryBrowser.trackScroll];
  _sidebarCover = [[AobusArtworkView alloc] initWithFrame:NSZeroRect];
  _sidebarCover.imageScaling = NSImageScaleProportionallyUpOrDown;
  _sidebarCover.accessibilityElement = YES;
  _sidebarCover.accessibilityLabel = [self text:MessageId::AppKitPlayingArtwork];
  _sidebarCover.accessibilityValue = [self text:MessageId::CoverArtNone];
  [_sidebar addSubview:_sidebarCover];
  _search = [[NSSearchField alloc] initWithFrame:NSZeroRect];
  _search.bezelStyle = NSTextFieldRoundedBezel;
  _search.placeholderString = [self text:MessageId::AppKitFilterPlaceholder];
  _search.target = self;
  _search.action = @selector(filterTracks:);
  _search.delegate = self;
  _search.sendsSearchStringImmediately = YES;
  [_browser addSubview:_search];
  _presentation = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  _presentation.bezelStyle = NSBezelStyleRounded;
  [_presentation addItemsWithTitles:@[
    [self text:MessageId::TrackPresentationAlbums],
    [self text:MessageId::TrackPresentationSongs],
    [self text:MessageId::TrackPresentationArtists],
    [self text:MessageId::TrackPresentationLibrary]
  ]];
  _presentation.target = self;
  _presentation.action = @selector(selectPresentation:);
  [_browser addSubview:_presentation];
  _browserTitle = label([self text:MessageId::LibraryAllTracks], kLargeTitleFontSize);
  _browserTitle.font = [NSFont systemFontOfSize:kLargeTitleFontSize weight:NSFontWeightSemibold];
  [_browser addSubview:_browserTitle];
  _count = label(@"", kSecondaryFontSize, YES);
  [_browser addSubview:_count];
  _empty = label([self text:MessageId::AppKitEmptyLibraryTitle], kLargeTitleFontSize, YES);
  _empty.alignment = NSTextAlignmentCenter;
  _empty.font = [NSFont systemFontOfSize:kLargeTitleFontSize weight:NSFontWeightSemibold];
  [_browser addSubview:_empty];
  _emptyHint = label(@"", kBodyFontSize, YES);
  _emptyHint.alignment = NSTextAlignmentCenter;
  [_browser addSubview:_emptyHint];
  _emptyAction = [NSButton buttonWithTitle:[self text:MessageId::AppKitOpenLibraryAction]
                                    target:self
                                    action:@selector(emptyAction:)];
  [_browser addSubview:_emptyAction];
  _status = label([self text:MessageId::AppKitReady], kSecondaryFontSize, YES);
  [_root addSubview:_status];
  _playbackBar = [[AobusPlaybackBar alloc] initWithCatalog:_sessionPtr->catalog()
                                                    target:self
                                           transportAction:@selector(transport:)
                                                seekAction:@selector(seek:)
                                              volumeAction:@selector(volume:)
                                          showVolumeAction:@selector(showVolume:)
                                          showOutputAction:@selector(showOutput:)
                                         showOptionsAction:@selector(showPlaybackOptions:)
                                      playingArtworkMirror:_sidebarCover];

  _modernHeader = [NSStackView stackViewWithViews:@[_presentation, _count]];
  _modernHeader.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  _modernHeader.alignment = NSLayoutAttributeCenterY;
  _modernHeader.distribution = NSStackViewDistributionFill;
  _modernHeader.spacing = 8;
  [_count setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  [_presentation setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                            forOrientation:NSLayoutConstraintOrientationHorizontal];
  [_browser addSubview:_modernHeader];
  _modernHeader.translatesAutoresizingMaskIntoConstraints = NO;
  _libraryBrowser.trackScroll.translatesAutoresizingMaskIntoConstraints = NO;
  _modernBrowserConstraints = @[
    [_modernHeader.leadingAnchor constraintEqualToAnchor:_browser.leadingAnchor constant:kContentInset],
    [_modernHeader.trailingAnchor constraintEqualToAnchor:_browser.trailingAnchor constant:-kContentInset],
    [_modernHeader.topAnchor constraintEqualToAnchor:_browser.safeAreaLayoutGuide.topAnchor constant:kContentGap],
    [_libraryBrowser.trackScroll.leadingAnchor constraintEqualToAnchor:_browser.leadingAnchor constant:kCellInset],
    [_libraryBrowser.trackScroll.trailingAnchor constraintEqualToAnchor:_browser.trailingAnchor constant:-kCellInset],
    [_libraryBrowser.trackScroll.topAnchor constraintEqualToAnchor:_modernHeader.bottomAnchor constant:kContentGap],
    [_libraryBrowser.trackScroll.bottomAnchor constraintEqualToAnchor:_browser.bottomAnchor constant:-kContentGap]
  ];

  auto* const toolbar = [[NSToolbar alloc] initWithIdentifier:@"AobusToolbar"];
  toolbar.delegate = self;
  toolbar.displayMode = NSToolbarDisplayModeIconOnly;
  toolbar.allowsUserCustomization = YES;
  toolbar.autosavesConfiguration = NO;
  // Attaching initializes the default items; restore the user's layout afterward.
  _window.toolbar = toolbar;
  _window.toolbarStyle = NSWindowToolbarStyleUnified;

  if (NSDictionary* const configuration = _settings[@"toolbarConfiguration"];
      [configuration isKindOfClass:NSDictionary.class] != NO)
  {
    if (NSArray* const savedItems = configuration[@"itemIdentifiers"]; [savedItems isKindOfClass:NSArray.class] != NO)
    {
      auto* const items = [NSMutableArray<NSToolbarItemIdentifier> array];
      auto* const allowed = [self toolbarAllowedItemIdentifiers:toolbar];

      for (NSUInteger index = 0; index < savedItems.count; ++index)
      {
        if (id const identifier = savedItems[index];
            [identifier isKindOfClass:NSString.class] != NO && [allowed containsObject:identifier] != NO)
        {
          [items addObject:identifier];
        }
      }

      toolbar.itemIdentifiers = items;
    }

    if (NSNumber* const savedMode = configuration[@"displayMode"];
        [savedMode isKindOfClass:NSNumber.class] != NO &&
        savedMode.integerValue >= static_cast<NSInteger>(NSToolbarDisplayModeDefault) &&
        savedMode.integerValue <= static_cast<NSInteger>(NSToolbarDisplayModeLabelOnly))
    {
      toolbar.displayMode = static_cast<NSToolbarDisplayMode>(savedMode.integerValue);
    }
  }

  auto containsToolbarItem = [&](NSToolbarItemIdentifier identifier)
  {
    auto* const items = toolbar.items;

    for (NSUInteger index = 0; index < items.count; ++index)
    {
      if (auto* const item = items[index]; [item.itemIdentifier isEqual:identifier] != 0)
      {
        return true;
      }
    }

    return false;
  };
  auto ensureToolbarItem = [&](NSToolbarItemIdentifier identifier, NSInteger index)
  {
    if (!containsToolbarItem(identifier))
    {
      [toolbar insertItemWithItemIdentifier:identifier
                                    atIndex:std::min(index, static_cast<NSInteger>(toolbar.items.count))];
    }
  };
  ensureToolbarItem(NSToolbarToggleSidebarItemIdentifier, 0);
  ensureToolbarItem(NSToolbarSidebarTrackingSeparatorItemIdentifier, 1);
  ensureToolbarItem(@"destination", 2);
  auto const toolbarItemCount = toolbar.items.count;
  auto inspectorIndex = static_cast<NSInteger>(toolbarItemCount);

  for (NSUInteger index = 0; index < toolbarItemCount; ++index)
  {
    if ([toolbar.items[index].itemIdentifier isEqual:NSToolbarToggleInspectorItemIdentifier] != NO)
    {
      inspectorIndex = static_cast<NSInteger>(index);
      break;
    }
  }

  ensureToolbarItem(NSToolbarInspectorTrackingSeparatorItemIdentifier, inspectorIndex);
  [self layoutContent];
}

- (void)layoutContent
{
  if (_root == nil)
  {
    return;
  }

  _window.toolbar.visible = _modern;
  _window.titleVisibility = (_modern != NO) ? NSWindowTitleHidden : NSWindowTitleVisible;
  _emptyAction.bezelStyle = NSBezelStyleRounded;
  _emptyAction.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  _search.controlSize = NSControlSizeRegular;
  _toolbarSearch.controlSize = NSControlSizeLarge;
  _presentation.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  auto const width = _root.bounds.size.width;
  auto const height = _root.bounds.size.height;
  auto const classicTop = _root.safeAreaInsets.top;
  auto const splitTop = (_modern != NO) ? 0 : classicTop + [_playbackBar heightForModern:NO];
  _splitTopConstraint.constant = splitTop;
  _splitBottomConstraint.constant = (_modern != NO) ? -[_playbackBar heightForModern:YES] : -kClassicStatusAreaHeight;
  auto const showInspector = (_inspectorVisible != NO) && width >= 1000;
  _inspectorItem.collapsed = static_cast<BOOL>(!showInspector);
  [_root layoutSubtreeIfNeeded];

  auto const sideWidth = _sidebar.bounds.size.width;
  auto const sideHeight = _sidebar.bounds.size.height;
  auto const sidebarTop = _modern != NO ? _sidebar.safeAreaInsets.top + 8 : kClassicSidebarTop;
  setFrame(_libraryLabel, kSidebarLabelLeading, kContentInset, sideWidth - 32, kSidebarLabelHeight);
  _libraryLabel.hidden = _modern;
  setFrame(_libraryBrowser.listScroll,
           8,
           sidebarTop,
           sideWidth - kContentInset,
           (_modern != NO) ? sideHeight - sidebarTop - 8 : sideHeight - kClassicSidebarVerticalInsets);
  _sidebarCover.hidden = _modern;
  setFrame(_sidebarCover,
           kContentGap,
           sideHeight - kSidebarArtworkHeight - kContentGap,
           sideWidth - (2 * kContentGap),
           kSidebarArtworkHeight);
  auto const browserWidth = _browser.bounds.size.width;
  auto const browserHeight = _browser.bounds.size.height;

  _count.alignment = NSTextAlignmentRight;
  _browserTitle.hidden = YES;
  _count.hidden = NO;
  _modernHeader.hidden = static_cast<BOOL>(_modern == NO);
  _search.hidden = _modern;
  [_playbackBar layoutInRoot:_root modern:_modern];

  if (_modern != NO)
  {
    if (_presentation.superview != _modernHeader)
    {
      detachView(_presentation);
      [_modernHeader insertArrangedSubview:_presentation atIndex:0];
      _presentation.translatesAutoresizingMaskIntoConstraints = NO;
    }

    _libraryBrowser.trackScroll.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:_modernBrowserConstraints];
  }
  else
  {
    [NSLayoutConstraint deactivateConstraints:_modernBrowserConstraints];

    if (_presentation.superview != _browser)
    {
      detachView(_presentation);
      [_browser addSubview:_presentation];
      _presentation.translatesAutoresizingMaskIntoConstraints = YES;
    }

    _libraryBrowser.trackScroll.translatesAutoresizingMaskIntoConstraints = YES;
    setFrame(_presentation,
             browserWidth - kClassicPresentationTrailing,
             kClassicBrowserControlTop,
             kPresentationWidth,
             _presentation.fittingSize.height);
    setFrame(_search,
             kClassicSearchLeading,
             kClassicBrowserControlTop,
             browserWidth - kClassicSearchTrailing,
             _search.fittingSize.height);
    setFrame(_libraryBrowser.trackScroll,
             0,
             kClassicBrowserHeaderHeight,
             browserWidth,
             browserHeight - kClassicBrowserHeaderHeight);
  }

  [_root layoutSubtreeIfNeeded];
  auto const tableFrame = _libraryBrowser.trackScroll.frame;
  auto const emptyTop = tableFrame.origin.y + std::max(0.0, tableFrame.size.height / 3);
  setFrame(_empty, kOuterInset, emptyTop, browserWidth - (2 * kOuterInset), kControlHeight);
  setFrame(_emptyHint, kOuterInset, emptyTop + kSidebarRowHeight, browserWidth - (2 * kOuterInset), kControlHeight);
  setFrame(_emptyAction,
           (browserWidth - kEmptyActionWidth) / 2,
           emptyTop + kEmptyActionOffset,
           kEmptyActionWidth,
           _emptyAction.fittingSize.height);

  [_libraryBrowser layoutForModern:_modern browserWidth:browserWidth];

  [_trackInspector layoutForModern:_modern];
  _status.hidden = _modern;
  setFrame(_status, kContentGap, height - kOuterInset, width - (2 * kContentGap), kContentInset);
}

- (void)windowDidResize:(NSNotification*) [[maybe_unused]] notification
{
  _layoutPending = YES;
  [self requestService];
}

- (NSSize)windowWillResize:(NSWindow*) [[maybe_unused]] window toSize:(NSSize)size
{
  _inspectorItem.collapsed = static_cast<BOOL>(_inspectorVisible == NO || size.width < 1000);
  return size;
}

- (void)splitViewDidResizeSubviews:(NSNotification*) [[maybe_unused]] notification
{
  _layoutPending = YES;
  [self requestService];
}

- (void)selectMode:(NSMenuItem*)sender
{
  if ((_closing != NO) || _window.attachedSheet != nil)
  {
    return;
  }

  if (auto* const editor = (_modern != NO ? _toolbarSearch : _search).currentEditor;
      ([editor isKindOfClass:NSTextView.class] != NO) && ([static_cast<NSTextView*>(editor) hasMarkedText] != NO))
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto const origin = _libraryBrowser.trackScroll.contentView.bounds.origin;
      _settings[_modern ? @"modernInspector" : @"classicInspector"] = @(_inspectorVisible);
      _modern = static_cast<BOOL>(sender.tag != 0);
      _settings[@"mode"] = _modern ? @"modern" : @"classic";
      _inspectorVisible = [_settings[_modern ? @"modernInspector" : @"classicInspector"] boolValue];
      [self layoutContent];
      [_libraryBrowser.trackScroll.contentView scrollToPoint:origin];
      [_libraryBrowser.trackScroll reflectScrolledClipView:_libraryBrowser.trackScroll.contentView];
      [self saveSettings];
      _libraryDirty = YES;
      _playbackDirty = YES;
      [self requestService];
    });
}

- (void)selectAppearance:(NSMenuItem*)sender
{
  _settings[@"appearance"] = sender.representedObject;
  NSApp.appearance = nativeAppearance(sender.representedObject);
  _layoutPending = YES;
  [self requestService];
  [self saveSettings];
}

- (void)toggleInspector:(id) [[maybe_unused]] sender
{
  if (_trackInspector.sheet != nil)
  {
    [self dismissInspector:nil];
    return;
  }

  if (_root.bounds.size.width < 1000 && (_closing == NO))
  {
    if (_window.attachedSheet != nil)
    {
      return;
    }

    [_trackInspector presentSheetForWindow:_window];
    return;
  }

  _inspectorVisible = static_cast<BOOL>(_inspectorVisible == NO);
  _settings[(_modern != NO) ? @"modernInspector" : @"classicInspector"] = @(_inspectorVisible);
  [self layoutContent];
  [self saveSettings];
}

- (void)dismissInspector:(id) [[maybe_unused]] sender
{
  [_trackInspector dismissSheet];
  [self requestService];
}

- (void)toggleSidebar:(id) [[maybe_unused]] sender
{
  auto const collapsed = static_cast<BOOL>(_sidebarItem.collapsed == NO);

  if (NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion != NO)
  {
    _sidebarItem.collapsed = collapsed;
  }
  else
  {
    [[_sidebarItem animator] setCollapsed:collapsed];
  }
}

- (void)focusSearch:(id) [[maybe_unused]] sender
{
  if (_modern != NO)
  {
    if (_toolbarSearchItem == nil)
    {
      auto* const toolbar = _window.toolbar;
      auto insertionIndex = static_cast<NSInteger>(toolbar.items.count);

      for (NSUInteger index = 0; index < toolbar.items.count; ++index)
      {
        if (auto const identifier = toolbar.items[index].itemIdentifier;
            [identifier isEqual:@"activity"] != NO ||
            [identifier isEqual:NSToolbarInspectorTrackingSeparatorItemIdentifier] != NO)
        {
          insertionIndex = static_cast<NSInteger>(index);
          break;
        }
      }

      [toolbar insertItemWithItemIdentifier:@"search" atIndex:insertionIndex];

      if (_toolbarSearch != nil)
      {
        _toolbarSearch.stringValue = nativeText(_sessionPtr->state().filter.entryText);
      }
    }

    [_toolbarSearchItem beginSearchInteraction];
  }
  else
  {
    [_window makeFirstResponder:_search];
  }
}

- (void)selectPresentation:(id) [[maybe_unused]] sender
{
  if (_closing != NO || !_sessionPtr || _presentation == nil)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto* const ids = @[@"albums", @"songs", @"artists", @"library"];
      auto const index = _presentation.indexOfSelectedItem;

      if (index < 0 || static_cast<NSUInteger>(index) >= ids.count)
      {
        return;
      }

      _sessionPtr->setPresentation(utf8(ids[static_cast<NSUInteger>(index)]));
    });
}

- (void)emptyAction:(id) [[maybe_unused]] sender
{
  if (_closing != NO || !_sessionPtr)
  {
    return;
  }

  if (_sessionPtr->state().filter.entryText.empty())
  {
    if ([self activeListId] == ao::rt::kAllTracksListId)
    {
      [self openLibrary:nil];
    }
    else
    {
      _sessionPtr->navigate(ao::rt::kAllTracksListId);
    }
  }
  else
  {
    _search.stringValue = @"";
    _toolbarSearch.stringValue = @"";
    [self filterTracks:_search];
  }
}

- (void)filterTracks:(id) [[maybe_unused]] sender
{
  auto* field = (_modern != NO) ? _toolbarSearch : _search;

  if ([sender isKindOfClass:NSSearchField.class] != NO)
  {
    field = static_cast<NSSearchField*>(sender);
  }

  if (field == nil)
  {
    return;
  }

  if (auto* const editor = field.currentEditor;
      (_closing == NO) && _sessionPtr &&
      (([editor isKindOfClass:NSTextView.class] == NO) || ([static_cast<NSTextView*>(editor) hasMarkedText] == NO)))
  {
    auto* const peer = field == _search ? _toolbarSearch : _search;

    if (peer.currentEditor == nil && [peer.stringValue isEqualToString:field.stringValue] == NO)
    {
      peer.stringValue = field.stringValue;
    }

    nativeCallback([&] { _sessionPtr->filter(utf8(field.stringValue)); });
  }
}

- (void)showVolume:(id) [[maybe_unused]] sender
{
  if (_closing != NO)
  {
    return;
  }

  [_playbackBar toggleVolumePopover];
}

@end
