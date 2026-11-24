// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitDesktopScenarioHelpers.h"
#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/DesktopApplicationInternal.h"
#include "app/macos-appkit/DesktopWindowGeometry.h"
#include "app/macos-appkit/LibraryBrowser.h"
#include <ao/Contract.h>

#include <filesystem>
#include <functional>
#include <print>
#include <utility>

namespace ao::appkit::test
{
  namespace
  {
    using namespace desktop;

    NSString* const kComputed = @"All except blocked";
    NSString* const kCurrent = @"Concerts";
    NSString* const kOther = @"夜 tapes";
    NSString* const kCurrentExpression = @"#\"Live \\\"朝\\\"\"";
    NSString* const kOtherExpression = @"#\"Set \\\"A\\\"\\\\夜\"";
    NSString* const kFirst = @"AppKit Fixture Track 01";
    NSString* const kSecond = @"AppKit Fixture Track 02";
    NSString* const kSentinel = @"AppKit Fixture Track 03";

    NSArray<NSNumber*>* selectTracks(AobusLibraryBrowser* browser, NSArray<NSString*>* titles)
    {
      auto* const table = browser.tracks;
      auto* const indexes = [NSMutableIndexSet indexSet];

      for (NSUInteger index = 0; index < titles.count; ++index)
      {
        auto* const title = titles[index];
        NSInteger row = -1;
        requireWaitUntil(
          [&]
          {
            row = trackRow(table, title);
            return row >= 0 && [table.delegate tableView:table shouldSelectRow:row] != NO;
          },
          "The named fixture track must become selectable");
        [indexes addIndex:static_cast<NSUInteger>(row)];
      }

      [table selectRowIndexes:indexes byExtendingSelection:NO];
      auto* const ids = [NSMutableArray<NSNumber*> array];

      for (auto const id : [browser selectedTrackIds])
      {
        [ids addObject:@(id.raw())];
      }

      AO_INVARIANT([table.selectedRowIndexes isEqual:indexes] != NO && ids.count == titles.count,
                   "The real desktop must select every requested track before building its menu");
      return ids;
    }

    NSNumber* createList(AobusLibraryBrowser* browser, NSString* name, NSString* expression)
    {
      auto* const window = browser.tracks.window;
      activateMenuItem(findMenuItem(NSApp.mainMenu, ::NSSelectorFromString(@"newList:")), @"create membership fixture");
      settleNativeCallbacks();
      auto* const sheet = window.attachedSheet;
      editText(sheet, @"Name", name);
      editText(sheet, @"Expression", expression);
      auto* const save = static_cast<NSButton*>(findControl(sheet.contentView, @"save"));
      AO_INVARIANT(save.enabled != NO, "The fixture's native list editor must enable Save");
      [save performClick:nil];
      requireWaitUntil([&] { return window.attachedSheet == nil && [browser.activeListTitle isEqual:name] != NO; },
                       "Saving the fixture list must navigate the desktop to it");
      return @(browser.activeListId.raw());
    }

    void navigate(AobusLibraryBrowser* browser, NSNumber* listId, NSString* name)
    {
      auto* const outline = browser.lists;
      auto const row = [outline rowForItem:listId];
      AO_INVARIANT(row >= 0, "The desktop sidebar must contain {}", name.UTF8String);
      [outline selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
      requireWaitUntil([&] { return [browser.activeListTitle isEqual:name] != NO; }, "Sidebar navigation must finish");
    }

    struct MembershipItems final
    {
      NSMenuItem* add;
      NSMenuItem* remove;
    };

    MembershipItems membershipItems(NSTableView* table, NSString* addLabel, NSString* removeLabel)
    {
      auto* const menu = table.menu;
      AO_INVARIANT([menu.delegate respondsToSelector:@selector(menuNeedsUpdate:)] != NO,
                   "The production track table must own an updatable context menu");
      [menu.delegate menuNeedsUpdate:menu];
      [menu update];
      auto* const submenu = [menu itemWithTitle:@"Add to Playlist"].submenu;
      AO_INVARIANT(submenu != nil && submenu.numberOfItems == 1,
                   "Add to Playlist must contain exactly the other writable target or one disabled placeholder");
      [submenu update];
      auto* const add = [submenu itemAtIndex:0];
      auto* const change = ::NSSelectorFromString(@"changeMembership:");

      if (addLabel == nil)
      {
        AO_INVARIANT([add.title isEqual:@"No Playlists to add to"] != NO && add.enabled == NO &&
                       add.action == nullptr && add.representedObject == nil,
                     "Without another writable target, Add must contain only a disabled, actionless placeholder");
      }
      else
      {
        AO_INVARIANT([add.title isEqual:addLabel] != NO && add.enabled != NO && add.action == change && add.tag == 0,
                     "Expected enabled Add target '{}', saw '{}'",
                     addLabel.UTF8String,
                     add.title.UTF8String);
      }

      NSMenuItem* remove = nil;

      for (NSInteger index = 0; index < menu.numberOfItems; ++index)
      {
        if (auto* const item = [menu itemAtIndex:index];
            item.action == change || [item.title hasPrefix:@"Remove from"] != NO)
        {
          AO_INVARIANT(remove == nil, "The menu must not duplicate its Remove action");
          remove = item;
        }
      }

      if (removeLabel == nil)
      {
        AO_INVARIANT(remove == nil, "A computed list must not be a Remove target");
      }
      else
      {
        AO_INVARIANT([remove.title isEqual:removeLabel] != NO && remove.enabled != NO && remove.action == change &&
                       remove.tag != 0,
                     "The active writable list must have an enabled named Remove action: {}",
                     removeLabel.UTF8String);
      }

      return {.add = add, .remove = remove};
    }

    void requirePayload(NSMenuItem* item, NSNumber* list, NSArray<NSNumber*>* tracks)
    {
      NSDictionary* const captured = item.representedObject;
      AO_INVARIANT([captured[@"list"] isEqual:list] != NO && [captured[@"tracks"] isEqual:tracks] != NO,
                   "Membership actions must carry the list ID and the track IDs captured when the menu was built");
    }

    void applyMembership(NSMenuItem* item, NSWindow* window)
    {
      // Do not rebuild the menu after changing live selection: dispatch the retained item as sender.
      auto const sent = [NSApp sendAction:item.action to:item.target from:item];
      AO_INVARIANT(sent != NO && window.attachedSheet != nil, "The native action must start a membership edit");

      if (item.tag == 0)
      {
        AO_INVARIANT([window.attachedSheet.title isEqual:@"Add to Playlist"] != NO,
                     "The Add membership sheet must use the same Playlist title as the menu");
      }

      requireWaitUntil([&] { return window.attachedSheet == nil; }, "The membership edit must finish without error");
    }

    void requireMembers(NSTableView* table, NSArray<NSString*>* members)
    {
      requireWaitUntil([&] { return static_cast<NSUInteger>(selectableRowCount(table)) == members.count; },
                       "The active list must show exactly the captured mutation's result count");

      for (NSUInteger index = 0; index < members.count; ++index)
      {
        auto* const member = members[index];
        AO_INVARIANT(trackRow(table, member) >= 0, "The active list must contain '{}'", member.UTF8String);
      }
    }

    void exerciseSelectedFile(NSWindow* window, NSTableView* table, std::filesystem::path const& musicRoot)
    {
      auto* const browser = static_cast<AobusLibraryBrowser*>(table.delegate);
      AO_INVARIANT([browser isKindOfClass:AobusLibraryBrowser.class] != NO, "The desktop must own the native browser");
      selectTracks(browser, @[kFirst]);
      auto* const expectedURL = [NSURL fileURLWithPath:nativePath(musicRoot / "appkit-fixture-01.wav")];

      auto* const menu = table.menu;
      [menu.delegate menuNeedsUpdate:menu];
      [menu update];
      auto* const reveal = findMenuItem(menu, ::NSSelectorFromString(@"revealSelection:"));
      AO_INVARIANT(reveal != nil && reveal.enabled && [reveal.target isKindOfClass:AobusDesktopDelegate.class] != NO,
                   "Reveal in Finder must be available for the selected track");
      auto* const target = static_cast<AobusDesktopDelegate*>(reveal.target);
      // Ask the real action target for the URL without dispatching the Finder action.
      auto* const url = [target revealURL:reveal];
      auto* const expectedResolvedURL = expectedURL.URLByResolvingSymlinksInPath;
      AO_INVARIANT(url.isFileURL && [url.URLByResolvingSymlinksInPath isEqual:expectedResolvedURL] != NO,
                   "Reveal in Finder must resolve the selected fixture file");

      [window setContentSize:NSMakeSize(780, 520)];
      requireWaitUntil([&] { return window.contentView.bounds.size.width < kDockedInspectorMinimumWidth; },
                       "The test window must be narrow enough to open the compact inspector");
      activateMenuItem(findMenuItem(NSApp.mainMenu, ::NSSelectorFromString(@"toggleInspector:")),
                       @"show the selected file in the compact inspector");
      requireWaitUntil([&] { return window.attachedSheet != nil; }, "The compact inspector sheet must open");
      auto* const sheet = window.attachedSheet;
      auto* const scroll =
        static_cast<NSScrollView*>(findAccessibilityView(sheet.contentView, @"compact-inspector-scroll"));
      AO_INVARIANT(
        [scroll isKindOfClass:NSScrollView.class] != NO && [scroll.documentView isKindOfClass:NSTextView.class] != NO,
        "The compact inspector must provide native detail text");
      auto* const text = static_cast<NSTextView*>(scroll.documentView);
      AO_INVARIANT([text.string containsString:expectedURL.lastPathComponent] != NO,
                   "The compact inspector must display the selected track's filename");
      [sheet cancelOperation:nil];
      requireWaitUntil([&] { return window.attachedSheet == nil; }, "The compact inspector sheet must close");
    }

    void exerciseInitialIndex(std::filesystem::path const& musicRoot)
    {
      requireWaitUntil([] { return findDesktopWindow() != nil; }, "The production desktop window must open");
      auto* const window = findDesktopWindow();
      auto* const table = findTrackTable(window.contentView);
      auto* const rescan = findMenuItem(NSApp.mainMenu, ::NSSelectorFromString(@"rescan:"));
      AO_INVARIANT(table != nil && rescan != nil, "The fresh desktop must expose the track table and Rescan action");
      requireWaitUntil(
        [&]
        {
          [rescan.menu update];
          return rescan.enabled != NO && trackRow(table, kPendingFixtureTrack) >= 0;
        },
        "Opening a fresh library without explicit scan intent must index the fixture tracks");
      AO_INVARIANT(selectableRowCount(table) == kIndexedFixtureTrackCount + 1,
                   "The initial scan must show exactly twenty selectable fixture tracks");
      exerciseSelectedFile(window, table, musicRoot);
    }

    void exerciseMembership()
    {
      requireWaitUntil([] { return findDesktopWindow() != nil; }, "The production desktop window must open");
      auto* const window = findDesktopWindow();
      auto* const table = findTrackTable(window.contentView);
      auto* const browser = static_cast<AobusLibraryBrowser*>(table.delegate);
      AO_INVARIANT([browser isKindOfClass:AobusLibraryBrowser.class] != NO, "The desktop must own the native browser");
      requireWaitUntil([&] { return selectableRowCount(table) >= kIndexedFixtureTrackCount; },
                       "The restored index must be visible before list authoring");
      requireIndexedOpenWithoutScan(table, "Restoring the indexed library from desktop settings");
      auto* const computed = createList(browser, kComputed, @"not #blocked");
      selectTracks(browser, @[kFirst]);
      membershipItems(table, nil, nil);
      std::println("AppKit membership: computed list with zero writable targets passed");

      auto* const current = createList(browser, kCurrent, kCurrentExpression);
      navigate(browser, computed, kComputed);
      auto* const three = selectTracks(browser, @[kFirst, kSecond, kSentinel]);
      auto* const currentLabel = [NSString stringWithFormat:@"%@ (%@)", kCurrent, kCurrentExpression];
      auto* const seed = membershipItems(table, currentLabel, nil).add;
      requirePayload(seed, current, three);
      applyMembership(seed, window);
      navigate(browser, current, kCurrent);
      requireMembers(table, @[kFirst, kSecond, kSentinel]);
      std::println("AppKit membership: computed list offers only Add to a writable target passed");

      auto* const single = selectTracks(browser, @[kSentinel]);
      auto* const removeLabel = [@"Remove from " stringByAppendingString:currentLabel];
      requirePayload(membershipItems(table, nil, removeLabel).remove, current, single);
      std::println("AppKit membership: current-only Remove and disabled Add placeholder passed");

      auto* const other = createList(browser, kOther, kOtherExpression);
      navigate(browser, current, kCurrent);
      auto* const pair = selectTracks(browser, @[kFirst, kSecond]);
      auto* const otherLabel = [NSString stringWithFormat:@"%@ (%@)", kOther, kOtherExpression];
      auto const items = membershipItems(table, otherLabel, removeLabel);
      selectTracks(browser, @[kSentinel]);
      requirePayload(items.add, other, pair);
      requirePayload(items.remove, current, pair);
      applyMembership(items.add, window);
      navigate(browser, other, kOther);
      requireMembers(table, @[kFirst, kSecond]);
      selectTracks(browser, @[kFirst]);
      // Both the active list and the selected tracks now differ from the captured Remove payload.
      applyMembership(items.remove, window);
      navigate(browser, current, kCurrent);
      requireMembers(table, @[kSentinel]);
      navigate(browser, other, kOther);
      requireMembers(table, @[kFirst, kSecond]);
      std::println(
        "AppKit membership: current plus other routes captured Add and Remove after selection changes passed");
    }
    std::int32_t runDesktopCheck(DesktopLaunch launch, i18n::MessageCatalog catalog, std::function<void()> exercise)
    {
      __block bool completed = false;
      auto const observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSApplicationWillTerminateNotification
                    object:NSApp
                     queue:nil
                usingBlock:^(NSNotification*) {
                  AO_INVARIANT(completed, "The desktop scenario must complete before Quit");
                }];
      [NSTimer scheduledTimerWithTimeInterval:0
                                      repeats:NO
                                        block:^(NSTimer*) {
                                          exercise();
                                          completed = true;
                                          [NSApp terminate:nil];
                                        }];
      auto const status = runDesktopApplication(std::move(launch), std::move(catalog));
      [NSNotificationCenter.defaultCenter removeObserver:observer];
      return completed ? status : 1;
    }
  } // namespace

  std::int32_t runInitialIndexScenario(DesktopLaunch launch, i18n::MessageCatalog catalog)
  {
    AO_INVARIANT(launch.optRequest && !launch.optRequest->scanAfterOpen,
                 "The first-open control must use the public library option without explicit scan intent");
    auto const musicRoot = launch.optRequest->libraryRoot;
    return runDesktopCheck(std::move(launch), std::move(catalog), [musicRoot] { exerciseInitialIndex(musicRoot); });
  }

  std::int32_t runMembershipScenario(DesktopLaunch launch, i18n::MessageCatalog catalog)
  {
    AO_INVARIANT(launch.optRequest, "The membership scenario requires an isolated library fixture");
    auto const& musicRoot = launch.optRequest->libraryRoot;
    preindexLibraryWithPendingTrack(musicRoot, launch.stateRoot);
    auto* const settings = @{@"libraryRoot": nativePath(musicRoot)};
    auto const written = [settings writeToFile:nativePath(launch.stateRoot / "desktop.plist") atomically:YES];
    AO_INVARIANT(written != NO, "The scenario must save the root that ordinary startup will restore");
    launch.optRequest.reset();
    return runDesktopCheck(std::move(launch), std::move(catalog), exerciseMembership);
  }
} // namespace ao::appkit::test
