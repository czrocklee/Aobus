// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitDesktopScenarioHelpers.h"

#include "AppKitScenarioSupport.h"
#include <ao/Contract.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ao::appkit::test::desktop
{
  namespace
  {
    bool isVisible(NSView* view)
    {
      return view != nil && view.window != nil && view.hiddenOrHasHiddenAncestor == NO;
    }

    void appendText(NSView* view, NSMutableString* result)
    {
      if ([view isKindOfClass:NSTextField.class] != NO)
      {
        if (auto* const field = static_cast<NSTextField*>(view); field.stringValue.length > 0)
        {
          [result appendFormat:@"\x1f%@", field.stringValue];
        }
      }

      auto* const children = view.subviews;

      for (NSUInteger index = 0; index < children.count; ++index)
      {
        appendText(children[index], result);
      }
    }

    bool isGroupRow(NSTableView* table, NSInteger row)
    {
      auto const delegate = table.delegate;
      return [delegate respondsToSelector:@selector(tableView:isGroupRow:)] != NO && [delegate tableView:table
                                                                                              isGroupRow:row] != NO;
    }
  } // namespace

  NSString* nativePath(std::filesystem::path const& path)
  {
    auto const value = path.string();
    auto* const result = [NSString stringWithUTF8String:value.c_str()];
    AO_INVARIANT(result != nil, "Scenario paths must be valid UTF-8");
    return result;
  }

  NSWindow* findDesktopWindow()
  {
    auto* const windows = NSApp.windows;

    for (NSUInteger index = 0; index < windows.count; ++index)
    {
      if (auto* const window = windows[index]; [window isKindOfClass:NSPanel.class] == NO &&
                                               window.contentView != nil && window.delegate != nil &&
                                               (window.styleMask & NSWindowStyleMaskTitled) != 0)
      {
        return window;
      }
    }

    return nil;
  }

  NSTableView* findTrackTable(NSView* view)
  {
    if ([view isKindOfClass:NSTableView.class] != NO && [view isKindOfClass:NSOutlineView.class] == NO)
    {
      auto* const table = static_cast<NSTableView*>(view);
      bool hasTitle = false;
      bool hasDuration = false;

      auto* const columns = table.tableColumns;

      for (NSUInteger index = 0; index < columns.count; ++index)
      {
        auto* const column = columns[index];
        hasTitle = hasTitle || [column.identifier isEqual:@"Title"] != NO;
        hasDuration = hasDuration || [column.identifier isEqual:@"Duration"] != NO;
      }

      if (hasTitle && hasDuration && table.dataSource != nil && table.delegate != nil)
      {
        return table;
      }
    }

    auto* const children = view.subviews;

    for (NSUInteger index = 0; index < children.count; ++index)
    {
      if (auto* const table = findTrackTable(children[index]); table != nil)
      {
        return table;
      }
    }

    return nil;
  }

  std::int32_t selectableRowCount(NSTableView* table)
  {
    std::int32_t count = 0;

    for (NSInteger row = 0; row < table.numberOfRows; ++row)
    {
      if (!isGroupRow(table, row))
      {
        ++count;
      }
    }

    return count;
  }

  NSInteger firstSelectableRow(NSTableView* table)
  {
    for (NSInteger row = 0; row < table.numberOfRows; ++row)
    {
      if (!isGroupRow(table, row))
      {
        return row;
      }
    }

    return -1;
  }

  NSString* rowIdentity(NSTableView* table, NSInteger row)
  {
    if (row < 0 || row >= table.numberOfRows || isGroupRow(table, row))
    {
      return @"";
    }

    auto* const result = [NSMutableString string];
    // Album visibility differs by presentation mode; compare the common columns.
    auto* const identifiers = @[@"#", @"Title", @"Artist", @"Duration"];

    for (NSUInteger index = 0; index < identifiers.count; ++index)
    {
      auto const column = [table columnWithIdentifier:identifiers[index]];
      AO_INVARIANT(column >= 0, "The native track table must expose its identity columns");

      if (auto* const cell = [table viewAtColumn:column row:row makeIfNecessary:YES]; cell != nil)
      {
        appendText(cell, result);
      }
    }

    return [result copy];
  }

  NSMenuItem* findMenuItem(NSMenu* menu, SEL action, bool matchTag, NSInteger tag, NSString* representedObject)
  {
    if (menu == nil)
    {
      return nil;
    }

    if (auto const delegate = menu.delegate; [delegate respondsToSelector:@selector(menuNeedsUpdate:)] != NO)
    {
      [delegate menuNeedsUpdate:menu];
    }

    auto* const items = menu.itemArray;

    for (NSUInteger index = 0; index < items.count; ++index)
    {
      auto* const item = items[index];

      if (item.action == action && (!matchTag || item.tag == tag) &&
          (representedObject == nil || [item.representedObject isEqual:representedObject] != NO))
      {
        return item;
      }

      if (auto* const found = findMenuItem(item.submenu, action, matchTag, tag, representedObject); found != nil)
      {
        return found;
      }
    }

    return nil;
  }

  void activateMenuItem(NSMenuItem* item, NSString* obligation)
  {
    AO_INVARIANT(item != nil, "Missing native menu item for {}", obligation.UTF8String);
    [item.menu update];
    AO_INVARIANT(item.enabled != 0, "Native menu item is disabled for {}", obligation.UTF8String);
    auto const sent = [NSApp sendAction:item.action to:item.target from:item];
    AO_INVARIANT(sent != 0, "Native menu action failed for {}", obligation.UTF8String);
  }

  NSControl* findActionControl(NSView* view, SEL action)
  {
    if ([view isKindOfClass:NSControl.class] != NO)
    {
      if (auto* const control = static_cast<NSControl*>(view); control.action == action)
      {
        return control;
      }
    }

    auto* const children = view.subviews;

    for (NSUInteger index = 0; index < children.count; ++index)
    {
      if (auto* const control = findActionControl(children[index], action); control != nil)
      {
        return control;
      }
    }

    return nil;
  }

  NSView* findAccessibilityView(NSView* view, NSString* identifier)
  {
    if ([view.accessibilityIdentifier isEqual:identifier] != NO)
    {
      return view;
    }

    auto* const children = view.subviews;

    for (NSUInteger index = 0; index < children.count; ++index)
    {
      if (auto* const found = findAccessibilityView(children[index], identifier); found != nil)
      {
        return found;
      }
    }

    return nil;
  }

  NSSearchField* findSearchField(NSWindow* window)
  {
    auto findInView = [&](this auto const& self, NSView* view) -> NSSearchField*
    {
      if ([view isKindOfClass:NSSearchField.class] != 0 && isVisible(view))
      {
        return static_cast<NSSearchField*>(view);
      }

      auto* const children = view.subviews;

      for (NSUInteger index = 0; index < children.count; ++index)
      {
        if (auto* const field = self(children[index]); field != nil)
        {
          return field;
        }
      }

      return nil;
    };

    if (auto* const field = findInView(window.contentView); field != nil)
    {
      return field;
    }

    auto* const items = window.toolbar.items;

    for (NSUInteger index = 0; index < items.count; ++index)
    {
      if (auto* const item = items[index]; [item isKindOfClass:NSSearchToolbarItem.class] != NO)
      {
        auto* const field = static_cast<NSSearchToolbarItem*>(item).searchField;

        if (isVisible(field))
        {
          return field;
        }
      }
    }

    return nil;
  }

  bool hasSavedLibrary(std::filesystem::path const& stateRoot, std::filesystem::path const& expected)
  {
    auto* const settings = [NSDictionary dictionaryWithContentsOfFile:nativePath(stateRoot / "desktop.plist")];
    id const root = settings[@"libraryRoot"];
    return [root isKindOfClass:NSString.class] != NO && [root isEqual:nativePath(expected)] != NO;
  }

  void writeMarker(std::filesystem::path const& path, NSString* contents)
  {
    NSError* error = nil;
    auto const written = [contents writeToFile:nativePath(path)
                                    atomically:YES
                                      encoding:NSUTF8StringEncoding
                                         error:&error];
    auto* const diagnostic = error != nil ? error.localizedDescription : @"unknown write failure";
    AO_INVARIANT(written != 0, "Could not write scenario marker: {}", diagnostic.UTF8String);
  }

  void preseedSettings(DesktopLaunch const& launch)
  {
    AO_INVARIANT(launch.optRequest, "The desktop smoke requires an initial library request");
    auto const successorRoot = launch.stateRoot / "successor-library";
    auto successorFixtureRes = prepareAppKitMediaLibrary(successorRoot);
    auto const successorFixtureError = successorFixtureRes ? std::string{} : successorFixtureRes.error().message;
    AO_INVARIANT(
      successorFixtureRes, "The desktop successor must have its own playable media fixture: {}", successorFixtureError);
    auto const settingsPath = launch.stateRoot / "desktop.plist";
    NSMutableDictionary* settings = [[NSDictionary dictionaryWithContentsOfFile:nativePath(settingsPath)] mutableCopy];

    if (settings == nil)
    {
      settings = [NSMutableDictionary dictionary];
    }

    settings[@"libraryRoot"] = nativePath(launch.optRequest->libraryRoot);
    settings[@"recentLibraries"] = @[nativePath(successorRoot)];
    auto const written = [settings writeToFile:nativePath(settingsPath) atomically:YES];
    AO_INVARIANT(written != 0, "The isolated desktop settings must be writable");
  }
} // namespace ao::appkit::test::desktop
