// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitPlaybackScenario.h"
#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/ActivityPopover.h"
#include "app/macos-appkit/AppKitText.h"
#include "app/macos-appkit/DesktopControls.h"
#include "app/macos-appkit/LibraryBrowser.h"
#include "app/macos-appkit/LibrarySession.h"
#include "app/macos-appkit/TrackInspector.h"
#include <ao/Contract.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

@interface AobusPresentationDraggingInfo : NSObject
@property (nonatomic, strong) NSPasteboard* draggingPasteboard;
@end

@implementation AobusPresentationDraggingInfo
@end

@interface AobusPresentationBrowserDelegate : NSObject<AobusLibraryBrowserDelegate>
@property (nonatomic) BOOL closing;
@property (nonatomic) BOOL sheetBlocked;
- (NSUInteger)selectionChangeCount;
- (NSUInteger)membershipRequestCount;
- (std::vector<ao::TrackId>)membershipTrackIds;
- (ao::ListId)membershipListId;
@end

@implementation AobusPresentationBrowserDelegate {
  NSUInteger _selectionChangeCount;
  NSUInteger _membershipRequestCount;
  std::vector<ao::TrackId> _membershipTrackIds;
  ao::ListId _membershipListId;
}

- (BOOL)isLibraryBrowserClosing:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  return _closing;
}

- (BOOL)isLibraryBrowserSheetBlocked:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  return _sheetBlocked;
}

- (void)libraryBrowserSelectionDidChange:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  ++_selectionChangeCount;
}

- (BOOL)libraryBrowser:(AobusLibraryBrowser*) [[maybe_unused]] browser
  requestMembershipForTracks:(std::vector<ao::TrackId> const&)trackIds
                      listId:(ao::ListId)listId
{
  AO_INVARIANT(!trackIds.empty(), "library membership request must contain tracks");
  AO_INVARIANT(listId != ao::kInvalidListId, "library membership request must target a list");
  ++_membershipRequestCount;
  _membershipTrackIds = trackIds;
  _membershipListId = listId;
  return YES;
}

- (NSUInteger)selectionChangeCount
{
  return _selectionChangeCount;
}

- (NSUInteger)membershipRequestCount
{
  return _membershipRequestCount;
}

- (std::vector<ao::TrackId>)membershipTrackIds
{
  return _membershipTrackIds;
}

- (ao::ListId)membershipListId
{
  return _membershipListId;
}
@end

namespace
{
  constexpr auto kActivityMaximumHeight = 360.0;
  constexpr auto kNotificationCount = std::size_t{40};
  constexpr auto kFixturePrefix = "Activity scenario ";
  constexpr auto kInitialUpdate = "Activity scenario live update: initial";
  constexpr auto kFinalUpdate = "Activity scenario live update: refreshed";
  constexpr auto kTopGrowth = "Activity scenario top growth";
  constexpr auto kHistoryGrowth = "Activity scenario history growth";

  template<typename Control>
  Control* requireControl(NSView* root, NSString* identifier)
  {
    auto* const control = ao::appkit::test::findControl(root, identifier);
    AO_INVARIANT(control != nil, "required AppKit control is missing");
    return static_cast<Control*>(control);
  }

  NSEvent* keyEvent(NSWindow* window, NSString* characters, std::uint16_t keyCode)
  {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:window.windowNumber
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:characters
                           isARepeat:NO
                             keyCode:keyCode];
  }

  NSString* notificationIdentifier(ao::rt::NotificationId const id)
  {
    return ao::appkit::nativeText(std::format("activity-notification-{}", id.raw()));
  }

  std::size_t activityFixtureCount(ao::appkit::LibrarySession const& session)
  {
    return static_cast<std::size_t>(std::ranges::count_if(session.state().activity.detail.items,
                                                          [](auto const& item)
                                                          { return item.message.starts_with(kFixturePrefix); }));
  }

  void exerciseActivity(ao::appkit::LibrarySession& session,
                        NSWindow* window,
                        NSButton* anchor,
                        std::filesystem::path const& stateRoot)
  {
    auto* const sessionBorrow = &session;
    auto* const popover = [[AobusActivityPopover alloc] initWithCatalog:session.catalog()
      dismissHandler:^{ sessionBorrow->dismissActivity(); }
      hideNotificationHandler:^(ao::rt::NotificationId const id) { sessionBorrow->hideActivityNotification(id); }];
    auto& notifications = session.runtime().notifications();

    for (std::size_t index = 0; index < kNotificationCount; ++index)
    {
      notifications.post(ao::rt::NotificationSeverity::Warning,
                         std::format("{}row {:02}", kFixturePrefix, index),
                         ao::rt::NotificationLifetime::history());
    }

    auto const updateKey = ao::rt::NotificationReportKey{"appkit.presentation.scenario.live"};
    notifications.createOrUpdate(updateKey,
                                 ao::rt::NotificationRequest{.severity = ao::rt::NotificationSeverity::Warning,
                                                             .message = kInitialUpdate,
                                                             .lifetime = ao::rt::NotificationLifetime::history()});
    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == kNotificationCount + 1; },
                                       "activity projection did not publish the scenario notifications");

    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    [popover showRelativeToRect:anchor.bounds ofView:anchor preferredEdge:NSRectEdgeMinY];
    ao::appkit::test::requireWaitUntil(
      [&] { return popover.shown != 0 && popover.contentViewController.view.window != nil; },
      "activity popover did not open");
    auto* const scroll = static_cast<NSScrollView*>(popover.contentViewController.view);
    AO_INVARIANT([scroll isKindOfClass:NSScrollView.class] != 0, "activity content must be a scroll view");
    [scroll layoutSubtreeIfNeeded];
    auto* const stack = scroll.documentView;
    AO_INVARIANT([stack isKindOfClass:NSStackView.class] != 0, "activity document must be a stack view");
    AO_INVARIANT(scroll.hasVerticalScroller != 0, "activity history must expose vertical scrolling");
    AO_INVARIANT(
      popover.contentSize.height <= kActivityMaximumHeight, "activity history must respect the supplied height bound");
    AO_INVARIANT(
      stack.frame.size.height > scroll.contentSize.height, "the populated history must exceed its bounded viewport");
    auto* const compact = requireControl<NSTextField>(stack, @"activity-compact");
    AO_INVARIANT(NSContainsRect(scroll.documentVisibleRect, [compact convertRect:compact.bounds toView:stack]),
                 "activity compact status must be visible when the popover opens");

    notifications.post(ao::rt::NotificationSeverity::Warning, kTopGrowth, ao::rt::NotificationLifetime::history());
    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == kNotificationCount + 2; },
                                       "activity projection did not publish a notification added while open");
    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    auto* const grownCompact = requireControl<NSTextField>(stack, @"activity-compact");
    AO_INVARIANT(
      NSContainsRect(scroll.documentVisibleRect, [grownCompact convertRect:grownCompact.bounds toView:stack]),
      "activity growth must keep the compact status visible when the user was at the top");

    auto const& items = session.state().activity.detail.items;
    auto const initial =
      std::ranges::find(items, std::string{kInitialUpdate}, &ao::uimodel::ActivityDetailItem::message);
    auto const newest =
      std::ranges::find(items, std::string{"Activity scenario row 39"}, &ao::uimodel::ActivityDetailItem::message);
    auto const oldest =
      std::ranges::find(items, std::string{"Activity scenario row 00"}, &ao::uimodel::ActivityDetailItem::message);
    AO_INVARIANT(initial != items.end() && newest != items.end() && oldest != items.end(),
                 "activity projection omitted a requested history item");
    auto const updatedId = initial->id;
    auto const oldestId = oldest->id;
    auto* const newestControl = requireControl<NSTextField>(stack, notificationIdentifier(newest->id));
    auto* const oldestControl = requireControl<NSTextField>(stack, notificationIdentifier(oldest->id));
    [newestControl scrollRectToVisible:newestControl.bounds];
    AO_INVARIANT(
      NSContainsRect(scroll.documentVisibleRect, [newestControl convertRect:newestControl.bounds toView:stack]),
      "newest activity history item must be reachable by scrolling");
    [oldestControl scrollRectToVisible:oldestControl.bounds];
    AO_INVARIANT(
      NSContainsRect(scroll.documentVisibleRect, [oldestControl convertRect:oldestControl.bounds toView:stack]),
      "oldest activity history item must be reachable by scrolling");

    notifications.post(ao::rt::NotificationSeverity::Warning, kHistoryGrowth, ao::rt::NotificationLifetime::history());
    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == kNotificationCount + 3; },
                                       "activity projection did not publish history growth");
    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    auto* const preservedOldest = requireControl<NSTextField>(stack, notificationIdentifier(oldestId));
    AO_INVARIANT(
      NSIntersectsRect(scroll.documentVisibleRect, [preservedOldest convertRect:preservedOldest.bounds toView:stack]),
      "activity growth must preserve the visible history item when the user is away from the top");
    ao::appkit::test::captureView(scroll, stateRoot / "appkit-activity-scroll.png");

    notifications.createOrUpdate(updateKey,
                                 ao::rt::NotificationRequest{.severity = ao::rt::NotificationSeverity::Warning,
                                                             .message = kFinalUpdate,
                                                             .lifetime = ao::rt::NotificationLifetime::history()});
    ao::appkit::test::requireWaitUntil(
      [&]
      {
        return std::ranges::contains(
          session.state().activity.detail.items, std::string{kFinalUpdate}, &ao::uimodel::ActivityDetailItem::message);
      },
      "activity keyed update did not reach the projection");
    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    AO_INVARIANT(popover.shown != 0, "a live activity update must preserve the open popover");
    auto* const updated = requireControl<NSTextField>(stack, notificationIdentifier(updatedId));
    AO_INVARIANT([updated.stringValue isEqualToString:@"Activity scenario live update: refreshed"] != 0,
                 "open activity content must render keyed notification updates");

    auto* const compactDismiss = requireControl<NSButton>(stack, @"activity-dismiss");
    AO_INVARIANT(compactDismiss.enabled != 0, "compact activity dismissal must be enabled");
    [compactDismiss performClick:nil];
    auto const fixtures = session.state().activity.detail.items;

    for (auto const& item : fixtures)
    {
      if (!item.message.starts_with(kFixturePrefix))
      {
        continue;
      }

      AO_INVARIANT(item.dismissible, "scenario activity notifications must be dismissible");
      auto* const dismiss =
        requireControl<NSButton>(stack, [notificationIdentifier(item.id) stringByAppendingString:@"-dismiss"]);
      AO_INVARIANT(dismiss.enabled != 0, "notification dismissal must be enabled");
      [dismiss performClick:nil];
    }

    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == 0; },
                                       "notification dismissal controls did not remove the scenario history");
    [popover close];
    ao::appkit::test::requireWaitUntil([&] { return popover.shown == 0; }, "activity popover did not close");
    AO_INVARIANT(window.attachedSheet == nil, "activity presentation must not attach a sheet");
  }

  void exerciseInspector(ao::appkit::LibrarySession& session,
                         NSWindow* window,
                         ao::TrackId const trackId,
                         std::filesystem::path const& stateRoot)
  {
    __block NSUInteger revealCount = 0;
    __block NSUInteger propertiesCount = 0;
    __block NSUInteger dismissCount = 0;
    auto* const inspector = [[AobusTrackInspector alloc] initWithCatalog:session.catalog()
      revealHandler:^{ ++revealCount; }
      propertiesHandler:^{ ++propertiesCount; }
      dismissHandler:^{ ++dismissCount; }];
    auto optRow = session.runtime().library().snapshot().trackRow(trackId);
    AO_INVARIANT(optRow, "selected track must remain available to the inspector");

    for (std::int32_t line = 0; line < 80; ++line)
    {
      optRow->genre += "Long metadata scroll probe\n";
    }

    inspector.view.frame = NSMakeRect(0, 0, 270, 340);
    [inspector renderSelectionCount:1 row:optRow canReveal:YES];
    [inspector renderArtwork:session.state().selectedCover];
    [inspector layoutForModern:YES];
    [inspector presentSheetForWindow:window];
    ao::appkit::test::requireWaitUntil([&]
                                       { return window.attachedSheet == inspector.sheet && inspector.sheet != nil; },
                                       "compact inspector sheet did not attach to its host window");
    auto* const scroll =
      static_cast<NSScrollView*>(ao::appkit::test::findView(inspector.sheet.contentView, @"compact-inspector-scroll"));
    AO_INVARIANT(
      [scroll isKindOfClass:NSScrollView.class] != 0, "compact inspector must expose its metadata scroll view");
    AO_INVARIANT(scroll.hasVerticalScroller != 0, "compact inspector metadata must be scrollable");
    auto* const text = static_cast<NSTextView*>(scroll.documentView);
    AO_INVARIANT([text isKindOfClass:NSTextView.class] != 0, "inspector scroll view must own metadata text");
    [text.layoutManager ensureLayoutForTextContainer:text.textContainer];
    [text scrollRangeToVisible:NSMakeRange(text.string.length - 1, 1)];
    AO_INVARIANT(text.bounds.size.height > scroll.contentView.bounds.size.height,
                 "long inspector metadata must exceed the compact viewport");
    AO_INVARIANT(
      scroll.contentView.bounds.origin.y > 0, "the end of long inspector metadata must be reachable by scrolling");
    ao::appkit::test::captureView(inspector.sheet.contentView, stateRoot / "appkit-inspector-sheet.png");

    auto* const done = static_cast<NSButton*>(inspector.sheet.defaultButtonCell.controlView);
    AO_INVARIANT([done isKindOfClass:NSButton.class] != 0, "The compact inspector must expose its default Done button");
    auto const returnHandled = [done performKeyEquivalent:keyEvent(inspector.sheet, @"\r", 36)];
    AO_INVARIANT(returnHandled != 0, "The Done button must accept its native Return key equivalent");
    ao::appkit::test::requireWaitUntil(
      [&] { return window.attachedSheet == nil; }, "Return did not detach the compact inspector sheet");
    AO_INVARIANT(dismissCount == 1, "Return dismissal must invoke its handler once");

    [inspector presentSheetForWindow:window];
    AO_INVARIANT(window.attachedSheet == inspector.sheet, "inspector sheet must support an Escape presentation");
    [inspector.sheet cancelOperation:nil];
    ao::appkit::test::requireWaitUntil(
      [&] { return window.attachedSheet == nil; }, "Escape did not detach the compact inspector sheet");
    AO_INVARIANT(dismissCount == 2, "Escape dismissal must invoke its handler once");

    [inspector presentSheetForWindow:window];
    AO_INVARIANT(
      window.attachedSheet == inspector.sheet, "inspector sheet must support a VoiceOver Escape presentation");
    auto const accessibilityEscaped = [inspector.sheet accessibilityPerformCancel];
    AO_INVARIANT(accessibilityEscaped != 0, "VoiceOver Escape must report handled");
    ao::appkit::test::requireWaitUntil(
      [&] { return window.attachedSheet == nil; }, "VoiceOver Escape did not detach the compact inspector sheet");
    AO_INVARIANT(dismissCount == 3, "VoiceOver Escape dismissal must invoke its handler once");
    AO_INVARIANT(
      revealCount == 0 && propertiesCount == 0, "keyboard inspector dismissal must not invoke unrelated actions");

    [inspector presentSheetForWindow:window];
    AO_INVARIANT(window.attachedSheet == inspector.sheet, "inspector sheet must support a final presentation");
    [inspector detach];
    ao::appkit::test::requireWaitUntil([&] { return window.attachedSheet == nil && inspector.sheet == nil; },
                                       "inspector detach must end an attached sheet");
    AO_INVARIANT(dismissCount == 3, "inspector detach must clear handlers before ending its sheet");
    [inspector renderSelectionCount:1 row:optRow canReveal:YES];
    [inspector layoutForModern:NO];
  }

  void exerciseBrowserCells(ao::appkit::LibrarySession& session, AobusLibraryBrowser* browser)
  {
    auto selectPresentation = [&](std::string const& identifier)
    {
      auto const viewId = session.runtime().workspace().snapshot().activeViewId;
      auto viewRes = session.runtime().views().findTrackListState(viewId);
      AO_INVARIANT(viewRes, "browser cell scenario requires an active track-list view");

      if (viewRes->presentation.id == identifier)
      {
        return;
      }

      auto const revision = session.state().tableRevision;
      session.setPresentation(identifier);
      ao::appkit::test::requireWaitUntil(
        [&]
        {
          auto const stateRes = session.runtime().views().findTrackListState(viewId);
          return stateRes && stateRes->presentation.id == identifier && session.state().tableRevision != revision;
        },
        "browser presentation did not publish");
    };
    auto groupRow = [&]
    {
      for (NSInteger row = 0; row < browser.tracks.numberOfRows; ++row)
      {
        if ([browser tableView:browser.tracks isGroupRow:row] != 0)
        {
          return row;
        }
      }

      return NSInteger{-1};
    };
    auto directTextFieldCount = [](NSView* view)
    {
      NSUInteger count = 0;
      auto* const subviews = view.subviews;

      for (NSUInteger index = 0; index < subviews.count; ++index)
      {
        auto* const subview = subviews[index];
        count += [subview isKindOfClass:NSTextField.class] != 0 ? 1 : 0;
      }

      return count;
    };
    auto visibleTitleCells = [&]
    {
      auto* const cells = [NSMutableArray<NSView*> array];
      auto const titleColumn = [browser.tracks columnWithIdentifier:@"Title"];
      auto const rows = [browser.tracks rowsInRect:browser.tracks.visibleRect];

      if (titleColumn < 0 || rows.location == NSNotFound)
      {
        return cells;
      }

      for (NSUInteger row = rows.location; row < NSMaxRange(rows); ++row)
      {
        auto const* track = session.rowAt(row);

        if (track == nullptr)
        {
          continue;
        }

        if (auto* const view = [browser.tracks viewAtColumn:titleColumn
                                                        row:static_cast<NSInteger>(row)
                                            makeIfNecessary:NO];
            view != nil)
        {
          auto* const trackCell = static_cast<NSTableCellView*>(view);
          AO_INVARIANT([trackCell.identifier isEqualToString:@"Title"] != 0 &&
                         [trackCell.textField.stringValue isEqualToString:ao::appkit::nativeText(track->title)] != 0,
                       "identified title cells must refresh from their current projected row");
          [cells addObject:view];
        }
      }

      return cells;
    };

    selectPresentation("albums");
    [browser layoutForModern:YES browserWidth:760];
    [browser refresh];
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto const modernGroupRow = groupRow();
    AO_INVARIANT(modernGroupRow >= 0, "album presentation must expose a group row");
    auto* const modernGroup = [browser tableView:browser.tracks viewForTableColumn:nil row:modernGroupRow];
    AO_INVARIANT([modernGroup.identifier isEqualToString:@"track-group-modern"] != 0 &&
                   [browser tableView:browser.tracks heightOfRow:modernGroupRow] == 44 &&
                   [browser.tracks rectOfRow:modernGroupRow].size.height == 44 &&
                   directTextFieldCount(modernGroup) == 2,
                 "modern group cells must have a reusable two-line presentation");

    auto* const originalCells = visibleTitleCells();
    AO_INVARIANT(originalCells.count >= 2, "browser reuse scenario requires visible track title cells");
    [browser.tracks reloadData];
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto* const reloadedCells = visibleTitleCells();
    AO_INVARIANT(reloadedCells.count == originalCells.count,
                 "reloading reusable cells must preserve the visible title-cell population");

    auto const originalScrollFrame = browser.trackScroll.frame;
    auto compactScrollFrame = originalScrollFrame;
    compactScrollFrame.size.height = 96;
    browser.trackScroll.frame = compactScrollFrame;
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks scrollRowToVisible:0];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto const firstVisibleRows = [browser.tracks rowsInRect:browser.tracks.visibleRect];
    AO_INVARIANT(visibleTitleCells().count > 0, "the compact browser viewport must show track cells");
    [browser.tracks scrollRowToVisible:browser.tracks.numberOfRows - 1];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto const lastVisibleRows = [browser.tracks rowsInRect:browser.tracks.visibleRect];
    AO_INVARIANT(lastVisibleRows.location > firstVisibleRows.location && visibleTitleCells().count > 0,
                 "scrolling must populate cells with a different set of projected tracks");
    browser.trackScroll.frame = originalScrollFrame;
    [browser.tracks scrollRowToVisible:0];

    [browser layoutForModern:NO browserWidth:620];
    [browser refresh];
    auto const classicGroupRow = groupRow();
    AO_INVARIANT(classicGroupRow >= 0, "classic album presentation must retain a group row");
    auto* const classicGroup = static_cast<NSTableCellView*>([browser tableView:browser.tracks
                                                             viewForTableColumn:nil
                                                                            row:classicGroupRow]);
    AO_INVARIANT([classicGroup.identifier isEqualToString:@"track-group-classic"] != 0 &&
                   [browser tableView:browser.tracks heightOfRow:classicGroupRow] == 38 &&
                   [browser.tracks rectOfRow:classicGroupRow].size.height == 38 &&
                   directTextFieldCount(classicGroup) == 1 &&
                   [classicGroup.textField.stringValue containsString:@"·"] != 0,
                 "classic mode must refresh group height and one-line cell state");

    auto const projectionRevision = session.state().tableRevision;
    selectPresentation("songs");
    AO_INVARIANT(session.state().tableRevision == projectionRevision + 1,
                 "A replacement projection's synchronous subscription reset must rebuild rows exactly once");
    [browser refresh];

    for (NSInteger row = 0; row < browser.tracks.numberOfRows; ++row)
    {
      AO_INVARIANT([browser tableView:browser.tracks isGroupRow:row] == 0,
                   "songs projection must replace grouped rows with tracks");
    }

    AO_INVARIANT(browser.tracks.numberOfRows == static_cast<NSInteger>(session.displayIndex().displayCount()),
                 "projection refresh must preserve the native row/data-index contract");
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    AO_INVARIANT(visibleTitleCells().count >= 2, "projection refresh must repopulate identified title cells");
    selectPresentation("albums");
    [browser refresh];
    [browser layoutForModern:YES browserWidth:760];
  }

  ao::TrackId exerciseBrowser(ao::appkit::LibrarySession& session,
                              AobusLibraryBrowser* browser,
                              AobusPresentationBrowserDelegate* delegate)
  {
    exerciseBrowserCells(session, browser);
    [browser layoutForModern:YES browserWidth:760];
    [browser refresh];
    AO_INVARIANT(browser.tracks.numberOfRows == static_cast<NSInteger>(session.displayIndex().displayCount()),
                 "library table rows must reflect the current display index");
    std::size_t selectedIndex = 0;
    auto selectedId = ao::kInvalidTrackId;

    for (std::size_t index = 0; index < session.displayIndex().displayCount(); ++index)
    {
      if (auto const* row = session.rowAt(index); row != nullptr)
      {
        selectedIndex = index;
        selectedId = row->id;
        break;
      }
    }

    AO_INVARIANT(selectedId != ao::kInvalidTrackId, "library scenario requires a selectable track row");
    [browser.tracks selectRowIndexes:[NSIndexSet indexSetWithIndex:selectedIndex] byExtendingSelection:NO];
    ao::appkit::test::requireWaitUntil([&] { return session.selection() == std::vector{selectedId}; },
                                       "native table selection did not reach the library session");
    AO_INVARIANT(browser.selectedTrackIds == std::vector<ao::TrackId>{selectedId},
                 "browser selected-track state must reflect the session selection");
    AO_INVARIANT(delegate.selectionChangeCount > 0, "native library selection must notify its delegate");

    auto const initialRevision = session.state().tableRevision;
    auto* const titleSort = [[NSSortDescriptor alloc] initWithKey:@"Title" ascending:NO];
    browser.tracks.sortDescriptors = @[titleSort];
    ao::appkit::test::requireWaitUntil(
      [&]
      {
        auto const viewId = session.runtime().workspace().snapshot().activeViewId;
        auto const viewRes = session.runtime().views().findTrackListState(viewId);
        return viewRes && session.state().tableRevision != initialRevision &&
               viewRes->presentation.id == "appkit-column-sort" && viewRes->presentation.sortBy.size() == 1 &&
               viewRes->presentation.sortBy.front().field == ao::rt::TrackSortField::Title &&
               !viewRes->presentation.sortBy.front().ascending;
      },
      "title column sort did not update the shared presentation state");
    [browser refresh];
    AO_INVARIANT([browser.presentationIdentifier isEqualToString:@"appkit-column-sort"] != 0,
                 "browser presentation identifier must reflect the shared sort state");
    AO_INVARIANT(browser.tracks.sortDescriptors.count == 1 &&
                   [browser.tracks.sortDescriptors.firstObject.key isEqualToString:@"Title"] != 0 &&
                   browser.tracks.sortDescriptors.firstObject.ascending == 0,
                 "browser sort descriptor must reflect the shared presentation state");
    AO_INVARIANT(
      session.selection() == std::vector<ao::TrackId>{selectedId}, "sorting must preserve the selected track identity");
    auto const titleRevision = session.state().tableRevision;
    browser.tracks.sortDescriptors = @[[[NSSortDescriptor alloc] initWithKey:@"#" ascending:YES]];
    ao::appkit::test::requireWaitUntil(
      [&]
      {
        auto const viewId = session.runtime().workspace().snapshot().activeViewId;
        auto const viewRes = session.runtime().views().findTrackListState(viewId);
        return viewRes && session.state().tableRevision != titleRevision && viewRes->presentation.sortBy.size() == 2 &&
               viewRes->presentation.sortBy[0].field == ao::rt::TrackSortField::DiscNumber &&
               viewRes->presentation.sortBy[0].ascending &&
               viewRes->presentation.sortBy[1].field == ao::rt::TrackSortField::TrackNumber &&
               viewRes->presentation.sortBy[1].ascending;
      },
      "The number column must sort disc before track within a multi-disc album");
    [browser refresh];
    AO_INVARIANT(browser.tracks.sortDescriptors.count == 1 &&
                   [browser.tracks.sortDescriptors.firstObject.key isEqualToString:@"#"] != 0 &&
                   browser.tracks.sortDescriptors.firstObject.ascending != 0,
                 "The compound disc/track sort must reflect as one native number-column descriptor");
    AO_INVARIANT(session.selection() == std::vector<ao::TrackId>{selectedId},
                 "The compound number-column sort must preserve the selected track identity");
    return selectedId;
  }

  ao::ListId exerciseBrowserDrops(ao::appkit::LibrarySession& session,
                                  AobusLibraryBrowser* browser,
                                  AobusPresentationBrowserDelegate* delegate,
                                  id<NSDraggingInfo> info,
                                  ao::TrackId const trackId)
  {
    auto& model = session.editor();
    auto const admissionRes = model.beginList();
    AO_INVARIANT(admissionRes, "The drop scenario must begin a writable List draft");
    model.editList("Drop target", "", "#appkit_drop_membership");
    auto const initialRevision = session.state().listRevision;
    model.save();
    ao::appkit::test::requireWaitUntil(
      [&] { return model.state().completed && session.state().listRevision != initialRevision; },
      "The drop target List must be saved and published");
    auto const listId = model.state().savedListId;
    AO_INVARIANT(listId != ao::kInvalidListId, "The drop target must have a durable List identity");
    model.cancel();
    [browser refresh];

    auto const writer = [browser tableView:browser.tracks pasteboardWriterForRow:browser.tracks.selectedRow];
    AO_INVARIANT(writer != nil, "The selected track must expose its native drag payload");
    auto const written = [info.draggingPasteboard writeObjects:@[writer]];
    AO_INVARIANT(written != NO, "The isolated drag pasteboard must accept the native track payload");
    auto* const item = @(listId.raw());
    auto const operation = [browser outlineView:browser.lists
                                   validateDrop:info
                                   proposedItem:item
                             proposedChildIndex:NSOutlineViewDropOnItemIndex];
    auto const accepted = [browser outlineView:browser.lists
                                    acceptDrop:info
                                          item:item
                                    childIndex:NSOutlineViewDropOnItemIndex];
    AO_INVARIANT(operation == NSDragOperationCopy && accepted != NO && delegate.membershipRequestCount == 1 &&
                   delegate.membershipTrackIds == std::vector{trackId} && delegate.membershipListId == listId,
                 "A writable List drop must request membership exactly once with the captured track and List IDs");

    auto const requireRejected = [&](char const* obligation)
    {
      auto const rejectedOperation = [browser outlineView:browser.lists
                                             validateDrop:info
                                             proposedItem:item
                                       proposedChildIndex:NSOutlineViewDropOnItemIndex];
      auto const rejected = [browser outlineView:browser.lists
                                      acceptDrop:info
                                            item:item
                                      childIndex:NSOutlineViewDropOnItemIndex];
      AO_INVARIANT(rejectedOperation == NSDragOperationNone && rejected == NO && delegate.membershipRequestCount == 1,
                   "{}",
                   obligation);
    };
    delegate.sheetBlocked = YES;
    requireRejected("A sheet-blocked browser must reject validation and acceptance without a membership request");
    delegate.sheetBlocked = NO;
    delegate.closing = YES;
    requireRejected("A closing browser must reject validation and acceptance without a membership request");
    delegate.closing = NO;

    auto const renameAdmissionRes = model.beginList(listId);
    AO_INVARIANT(renameAdmissionRes, "The drop target List must reopen for a revision change");
    model.editList("Renamed drop target", "", "#appkit_drop_membership");
    auto const listRevision = session.state().listRevision;
    model.save();
    ao::appkit::test::requireWaitUntil(
      [&] { return model.state().completed && session.state().listRevision != listRevision; },
      "The changed drop target must publish a new List revision");
    model.cancel();
    requireRejected("A stale browser must reject validation and acceptance until its List projection is refreshed");
    [browser refresh];
    auto const refreshedOperation = [browser outlineView:browser.lists
                                            validateDrop:info
                                            proposedItem:item
                                      proposedChildIndex:NSOutlineViewDropOnItemIndex];
    AO_INVARIANT(refreshedOperation == NSDragOperationCopy,
                 "Refreshing the List projection must restore admission for the same writable target");
    return listId;
  }

  void exerciseDetachedBrowser(AobusLibraryBrowser* browser,
                               AobusPresentationBrowserDelegate* delegate,
                               NSUInteger const selectionChangeCount,
                               id<NSDraggingInfo> info,
                               ao::ListId const dropListId)
  {
    auto const membershipRequestCount = delegate.membershipRequestCount;
    AO_INVARIANT(browser.tracks.delegate == nil && browser.tracks.dataSource == nil && browser.tracks.target == nil &&
                   browser.lists.delegate == nil && browser.lists.dataSource == nil,
                 "library detach must revoke native callbacks and targets");
    AO_INVARIANT(
      [browser numberOfRowsInTableView:browser.tracks] == 0, "detached library data source must report no track rows");
    AO_INVARIANT([browser outlineView:browser.lists numberOfChildrenOfItem:nil] == 0,
                 "detached library outline must report no root children");
    AO_INVARIANT([browser tableView:browser.tracks heightOfRow:0] > 0 &&
                   [browser tableView:browser.tracks shouldSelectRow:0] == 0 &&
                   [browser tableView:browser.tracks isGroupRow:0] == 0,
                 "Late table layout and selection queries must remain safe after session release");
    id const inertChild = [browser outlineView:browser.lists child:0 ofItem:nil];
    AO_INVARIANT(inertChild != nil && inertChild == NSNull.null,
                 "detached outline child callback must return an inert nonnull object");
    AO_INVARIANT([browser outlineView:browser.lists isItemExpandable:inertChild] == 0 &&
                   [browser outlineView:browser.lists isGroupItem:inertChild] == 0 &&
                   [browser outlineView:browser.lists shouldSelectItem:inertChild] == 0,
                 "detached outline callbacks must return neutral selection and expansion state");
    AO_INVARIANT([browser outlineView:browser.lists
                   viewForTableColumn:browser.lists.outlineTableColumn
                                 item:inertChild] == nil,
                 "detached outline callback must not create a cell");
    AO_INVARIANT([browser tableView:browser.tracks viewForTableColumn:browser.tracks.tableColumns.firstObject
                                  row:0] == nil,
                 "detached table callback must not create a cell");
    auto* const tableNotification = [NSNotification notificationWithName:NSTableViewSelectionDidChangeNotification
                                                                  object:browser.tracks];
    auto* const outlineNotification = [NSNotification notificationWithName:NSOutlineViewSelectionDidChangeNotification
                                                                    object:browser.lists];
    [browser tableViewSelectionDidChange:tableNotification];
    [browser outlineViewSelectionDidChange:outlineNotification];
    [browser tableView:browser.tracks sortDescriptorsDidChange:@[]];
    [browser refresh];
    [browser invalidateProjection];
    [browser playSelectedTrack];
    AO_INVARIANT(browser.activeListId == ao::kInvalidListId && browser.clickedListId == ao::kInvalidListId &&
                   browser.selectedTrackIds.empty(),
                 "detached library public state must be neutral");
    AO_INVARIANT(
      [browser prepareContextMenu:[[NSMenu alloc] init]] == 0, "detached library must reject context menu preparation");
    auto const operation = [browser outlineView:browser.lists
                                   validateDrop:info
                                   proposedItem:@(dropListId.raw())
                             proposedChildIndex:NSOutlineViewDropOnItemIndex];
    auto const accepted = [browser outlineView:browser.lists
                                    acceptDrop:info
                                          item:@(dropListId.raw())
                                    childIndex:NSOutlineViewDropOnItemIndex];
    AO_INVARIANT(operation == NSDragOperationNone && accepted == NO,
                 "A detached browser must reject a formerly valid track drop after session release");
    AO_INVARIANT(delegate.selectionChangeCount == selectionChangeCount &&
                   delegate.membershipRequestCount == membershipRequestCount,
                 "late native callbacks after session release must not reach the delegate");
  }
} // namespace

namespace ao::appkit::test
{
  std::int32_t runPresentationScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    AO_INVARIANT([NSThread isMainThread] != 0, "AppKit presentation scenario must run on the main thread");
    AO_INVARIANT(NSApp != nil, "AppKit presentation scenario requires an initialized NSApplication");
    auto* const window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 980, 720)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
    window.title = @"Aobus AppKit presentation scenario";
    window.releasedWhenClosed = NO;
    window.preventsApplicationTerminationWhenModal = NO;
    auto* const root = [[AobusFlippedView alloc] initWithFrame:window.contentView.bounds];
    root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = root;
    auto* const activityAnchor = [NSButton buttonWithTitle:@"Activity" target:nil action:nullptr];
    activityAnchor.frame = NSMakeRect(20, 660, 120, 30);
    [root addSubview:activityAnchor];
    auto* const browserDelegate = [[AobusPresentationBrowserDelegate alloc] init];
    auto* const draggingInfo = [[AobusPresentationDraggingInfo alloc] init];
    draggingInfo.draggingPasteboard = [NSPasteboard pasteboardWithUniqueName];
    id<NSDraggingInfo> const info = static_cast<id>(draggingInfo);
    auto dropListId = kInvalidListId;
    AobusLibraryBrowser* browser = nil;

    [window center];
    [window makeKeyAndOrderFront:nil];
    settleNativeCallbacks();

    exerciseActivityExpiration();

    {
      auto fixture = SessionFixture{musicRoot, stateRoot};
      auto& session = fixture.session();
      browser = [[AobusLibraryBrowser alloc] initWithSession:session delegate:browserDelegate];
      browser.listScroll.frame = NSMakeRect(20, 100, 190, 540);
      browser.trackScroll.frame = NSMakeRect(220, 100, 740, 540);
      [root addSubview:browser.listScroll];
      [root addSubview:browser.trackScroll];
      auto const selectedId = exerciseBrowser(session, browser, browserDelegate);
      dropListId = exerciseBrowserDrops(session, browser, browserDelegate, info, selectedId);
      exerciseActivity(session, window, activityAnchor, stateRoot);
      exerciseInspector(session, window, selectedId, stateRoot);
      exercisePlaybackPresentation(session, window, root, stateRoot);
      exerciseSelectedArtwork(session, window, selectedId);
      [browser detach];
    }

    auto const selectionChangeCount = browserDelegate.selectionChangeCount;
    exerciseDetachedBrowser(browser, browserDelegate, selectionChangeCount, info, dropListId);
    [draggingInfo.draggingPasteboard releaseGlobally];
    [window orderOut:nil];
    [window close];
    settleNativeCallbacks();
    return 0;
  }
} // namespace ao::appkit::test
