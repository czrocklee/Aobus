// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryBrowser.h"

#include "AppKitText.h"
#include "LibrarySession.h"
#include <ao/Contract.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
  constexpr auto kCellInset = 6;
  constexpr auto kContentGap = 12;
  constexpr auto kContentInset = 16;
  constexpr auto kCaptionFontSize = 10;
  constexpr auto kSecondaryFontSize = 11;
  constexpr auto kBodyFontSize = 13;
  constexpr auto kHeadingFontSize = 14;
  constexpr auto kClassicGroupFontSize = 12;
  constexpr auto kSidebarGroupRowHeight = 26;
  constexpr auto kSidebarRowHeight = 34;
  constexpr auto kTrackTextLineHeight = 18;
  constexpr auto kTrackHeadingPrimaryTop = 18;
  constexpr auto kTitleLineHeight = 19;
  constexpr auto kSecondaryLineHeight = 17;
  constexpr auto kClassicRowHeight = 22;
  constexpr auto kClassicGroupRowHeight = 38;
  constexpr auto kModernGroupHeight = 44;
  constexpr auto kTrackNumberColumnWidth = 44;
  constexpr auto kMinimumNumberColumnWidth = 36;
  constexpr auto kMinimumTextColumnWidth = 65;
  constexpr auto kInitialDurationColumnWidth = 70;
  constexpr auto kDurationColumnWidth = 72;
  constexpr auto kMinimumTitleColumnWidth = 160.0;
  constexpr auto kInitialTitleColumnWidth = 260;
  constexpr auto kMinimumArtistColumnWidth = 130.0;
  constexpr auto kAlbumColumnWidth = 170;
  constexpr auto kTitleColumnShare = 0.58;
  constexpr auto kArtistColumnShare = 0.42;
  constexpr auto kTypeSelectionContinuationSeconds = 1.0;
  NSPasteboardType const kTrackPasteboardType = @"org.aobus.track-id";
  NSUserInterfaceItemIdentifier const kClassicGroupCellIdentifier = @"track-group-classic";
  NSUserInterfaceItemIdentifier const kModernGroupCellIdentifier = @"track-group-modern";

  using ao::appkit::nativeText;
  using ao::i18n::MessageId;

  template<typename Callback>
  void nativeCallback(Callback&& callback) noexcept
  {
    try
    {
      std::forward<Callback>(callback)();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "AppKit library browser callback");
    }
  }

  void setFrame(NSView* view, CGFloat left, CGFloat top, CGFloat width, CGFloat height)
  {
    view.frame = NSMakeRect(left, top, std::max(0.0, width), std::max(0.0, height));
  }

  NSTextField* label(NSString* text, CGFloat size, BOOL secondary = NO)
  {
    auto* const field = [NSTextField labelWithString:text];
    field.font = [NSFont systemFontOfSize:size];
    field.textColor = secondary != NO ? NSColor.secondaryLabelColor : NSColor.labelColor;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
  }

  void showError(NSWindow* window, NSString* title, NSString* message)
  {
    auto* const alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = message;

    if (window != nil)
    {
      [alert beginSheetModalForWindow:window completionHandler:nil];
    }
    else
    {
      [alert runModal];
    }
  }
} // namespace

@interface AobusSourceLabel : NSTextField
@end
@implementation AobusSourceLabel
- (BOOL)allowsVibrancy
{
  return NO;
}
@end

@interface AobusSourceCell : NSTableCellView
@end
@implementation AobusSourceCell
- (void)setBackgroundStyle:(NSBackgroundStyle)style
{
  [super setBackgroundStyle:style];
  auto const emphasized = style == NSBackgroundStyleEmphasized && self.window.isKeyWindow != NO;
  self.textField.cell.backgroundStyle = emphasized ? NSBackgroundStyleEmphasized : NSBackgroundStyleNormal;
  [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
    NSColor* const textColor = emphasized ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor;
    self.textField.textColor = [textColor colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
    self.imageView.contentTintColor =
      emphasized ? NSColor.alternateSelectedControlTextColor : NSColor.controlAccentColor;
  }];
}
- (void)viewDidChangeEffectiveAppearance
{
  [super viewDidChangeEffectiveAppearance];
  [self setBackgroundStyle:self.backgroundStyle];
}
@end

@interface AobusTrackCell : NSTableCellView
@property (nonatomic, strong) NSTextField* secondaryTextField;
@end
@implementation AobusTrackCell
- (void)layout
{
  [super layout];

  if (auto const width = self.bounds.size.width; self.secondaryTextField != nil)
  {
    setFrame(self.textField, kContentGap, kTrackHeadingPrimaryTop, width - (2 * kContentGap), kTitleLineHeight);
    setFrame(self.secondaryTextField, kContentGap, 2, width - (2 * kContentGap), kSecondaryLineHeight);
  }
  else
  {
    auto const height = self.bounds.size.height;
    setFrame(
      self.textField, kCellInset, (height - kTrackTextLineHeight) / 2, width - kContentGap, kTrackTextLineHeight);
  }
}
@end

@interface AobusTrackTable : NSTableView
@end
@implementation AobusTrackTable {
  NSTimeInterval _typeSelectionDeadline;
}
- (void)keyDown:(NSEvent*)event
{
  auto const modifiers =
    event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
  auto* const characters = event.charactersIgnoringModifiers;
  auto const continuingTypeSelection = event.timestamp <= _typeSelectionDeadline;

  if (modifiers == 0 && [characters isEqual:@" "] != NO && !continuingTypeSelection)
  {
    [NSApp sendAction:(::NSSelectorFromString(@"togglePlayback:")) to:self.target from:self];
  }
  else if (modifiers == 0 && [characters isEqual:@"\r"] != NO)
  {
    [NSApp sendAction:(::NSSelectorFromString(@"playSelection:")) to:self.target from:self];
  }
  else
  {
    if (modifiers == 0 &&
        ((characters.length == 1 &&
          [characters rangeOfCharacterFromSet:NSCharacterSet.alphanumericCharacterSet].location != NSNotFound) ||
         ([characters isEqual:@" "] != NO && continuingTypeSelection)))
    {
      _typeSelectionDeadline = event.timestamp + kTypeSelectionContinuationSeconds;
    }

    [super keyDown:event];
  }
}
@end

@interface AobusLibraryBrowser ()
- (BOOL)isClosing;
- (BOOL)isSheetBlocked;
- (NSString*)text:(MessageId)message;
- (void)selectTracks:(std::vector<ao::TrackId> const&)selection;
- (void)refreshSortDescriptors:(std::vector<ao::rt::TrackSortTerm> const&)sortBy;
- (void)playTrack:(id)sender;
- (void)playSelection:(id)sender;
- (void)togglePlayback:(id)sender;
@end

@implementation AobusLibraryBrowser {
  // The composition root revokes this borrow with detach before destroying the session.
  ao::appkit::LibrarySession* _session;
  __weak id<AobusLibraryBrowserDelegate> _delegate;
  NSScrollView* _trackScroll;
  AobusTrackTable* _tracks;
  NSScrollView* _listScroll;
  NSOutlineView* _lists;
  NSMutableDictionary<NSNumber*, NSNumber*>* _listItems;
  ao::uimodel::ListTreeProjection _listTree;
  std::unordered_set<ao::ListId> _writableListIds;
  std::vector<ao::TrackId> _displayedSelection;
  std::uint64_t _tableRevision;
  std::uint64_t _listRevision;
  BOOL _outlineInitialized;
  NSNumber* _revealedListItem;
  NSArray<NSNumber*>* _revealedListAncestors;
  BOOL _refreshing;
  BOOL _modern;
  BOOL _rowHeightsInitialized;
  CGFloat _columnLayoutWidth;
  BOOL _columnLayoutModern;
  NSString* _activeListTitle;
  NSString* _presentationIdentifier;
}

- (instancetype)initWithSession:(ao::appkit::LibrarySession&)session delegate:(id<AobusLibraryBrowserDelegate>)delegate
{
  self = [super init];

  if (self != nil)
  {
    _session = &session;
    _delegate = delegate;
    _tableRevision = UINT64_MAX;
    _listRevision = UINT64_MAX;
    _activeListTitle = ao::appkit::catalogText(session.catalog(), MessageId::LibraryAllTracks);
    _presentationIdentifier = @"";
    _listItems = [NSMutableDictionary dictionary];
    _listItems[@(ao::rt::kAllTracksListId.raw())] = @(ao::rt::kAllTracksListId.raw());
    _lists = [[NSOutlineView alloc] initWithFrame:NSZeroRect];
    auto* const listColumn = [[NSTableColumn alloc] initWithIdentifier:@"list"];
    [_lists addTableColumn:listColumn];
    _lists.outlineTableColumn = listColumn;
    _lists.headerView = nil;
    _lists.rowHeight = kSidebarRowHeight;
    _lists.style = NSTableViewStyleSourceList;
    _lists.indentationPerLevel = kContentGap;
    _lists.backgroundColor = NSColor.clearColor;
    _lists.delegate = self;
    _lists.dataSource = self;
    [_lists registerForDraggedTypes:@[kTrackPasteboardType]];
    _listScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _listScroll.documentView = _lists;
    _listScroll.drawsBackground = NO;
    _listScroll.hasVerticalScroller = YES;

    _tracks = [[AobusTrackTable alloc] initWithFrame:NSZeroRect];
    _tracks.delegate = self;
    _tracks.dataSource = self;
    _tracks.target = self;
    _tracks.doubleAction = @selector(playTrack:);
    _tracks.allowsMultipleSelection = YES;
    _tracks.usesAutomaticRowHeights = NO;
    _tracks.allowsColumnReordering = YES;
    _tracks.allowsColumnResizing = YES;
    _tracks.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    _tracks.style = NSTableViewStyleInset;
    _tracks.backgroundColor = NSColor.clearColor;

    for (auto const& identifier : std::array<NSString*, 5>{@"#", @"Title", @"Artist", @"Album", @"Duration"})
    {
      auto* const column = [[NSTableColumn alloc] initWithIdentifier:identifier];
      column.title = identifier;

      if ([identifier isEqual:@"Title"] != NO)
      {
        column.title = ao::appkit::catalogText(session.catalog(), MessageId::TrackFieldTitle);
      }
      else if ([identifier isEqual:@"Artist"] != NO)
      {
        column.title = ao::appkit::catalogText(session.catalog(), MessageId::TrackFieldArtist);
      }
      else if ([identifier isEqual:@"Album"] != NO)
      {
        column.title = ao::appkit::catalogText(session.catalog(), MessageId::TrackFieldAlbum);
      }
      else if ([identifier isEqual:@"Duration"] != NO)
      {
        column.title = ao::appkit::catalogText(session.catalog(), MessageId::TrackFieldDuration);
      }

      column.width = kAlbumColumnWidth;

      if ([identifier isEqual:@"Title"] != NO)
      {
        column.width = kInitialTitleColumnWidth;
      }
      else if ([identifier isEqual:@"#"] != NO)
      {
        column.width = kTrackNumberColumnWidth;
      }

      column.resizingMask = NSTableColumnUserResizingMask;
      column.minWidth = [identifier isEqual:@"#"] != NO ? kMinimumNumberColumnWidth : kMinimumTextColumnWidth;

      if ([identifier isEqual:@"Duration"] != NO)
      {
        column.width = kInitialDurationColumnWidth;
      }

      column.sortDescriptorPrototype = [[NSSortDescriptor alloc] initWithKey:identifier ascending:YES];
      [_tracks addTableColumn:column];
    }

    [_tracks setDraggingSourceOperationMask:NSDragOperationCopy forLocal:YES];
    _trackScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _trackScroll.documentView = _tracks;
    _trackScroll.wantsLayer = YES;
    _trackScroll.layer.masksToBounds = YES;
    _trackScroll.hasVerticalScroller = YES;
    _trackScroll.hasHorizontalScroller = YES;
    _trackScroll.autohidesScrollers = YES;
    _trackScroll.drawsBackground = NO;
  }

  return self;
}

- (void)dealloc
{
  [self detach];
}

- (NSTableView*)tracks
{
  return _tracks;
}

- (NSOutlineView*)lists
{
  return _lists;
}

- (NSScrollView*)trackScroll
{
  return _trackScroll;
}

- (NSScrollView*)listScroll
{
  return _listScroll;
}

- (void)detach
{
  if (_session == nullptr)
  {
    return;
  }

  _tracks.delegate = nil;
  _tracks.dataSource = nil;
  _tracks.target = nil;
  _tracks.menu = nil;
  _lists.delegate = nil;
  _lists.dataSource = nil;
  _lists.menu = nil;
  [_tracks unregisterDraggedTypes];
  [_lists unregisterDraggedTypes];
  _delegate = nil;
  _session = nullptr;
}

- (void)invalidateProjection
{
  _tableRevision = UINT64_MAX;
}

- (BOOL)isClosing
{
  auto const delegate = _delegate;
  return static_cast<BOOL>(_session == nullptr || delegate == nil || [delegate isLibraryBrowserClosing:self] != NO);
}

- (BOOL)isSheetBlocked
{
  auto const delegate = _delegate;
  return static_cast<BOOL>(delegate == nil || [delegate isLibraryBrowserSheetBlocked:self] != NO);
}

- (NSString*)text:(MessageId)message
{
  return _session != nullptr ? ao::appkit::catalogText(_session->catalog(), message) : @"";
}

- (void)layoutForModern:(BOOL)modern browserWidth:(CGFloat)browserWidth
{
  auto const modeChanged = _modern != modern;

  if (modeChanged)
  {
    _modern = modern;
    _tableRevision = UINT64_MAX;
  }

  _trackScroll.layer.cornerRadius = modern != NO ? kContentGap : 0;
  _tracks.style = modern != NO ? NSTableViewStyleInset : NSTableViewStylePlain;
  _tracks.usesAlternatingRowBackgroundColors = static_cast<BOOL>(modern == NO);
  _tracks.rowHeight = modern != NO ? 32 : kClassicRowHeight;
  [_tracks tableColumnWithIdentifier:@"Album"].hidden = modern;
  _tracks.intercellSpacing = NSMakeSize(0, 0);
  _tracks.rowSizeStyle = NSTableViewRowSizeStyleCustom;

  if (_columnLayoutWidth != browserWidth || _columnLayoutModern != modern)
  {
    _columnLayoutWidth = browserWidth;
    _columnLayoutModern = modern;
    auto const contentWidth = std::max(440.0, browserWidth - (modern != NO ? 64 : kMinimumNumberColumnWidth));
    [_tracks tableColumnWithIdentifier:@"#"].width = kTrackNumberColumnWidth;
    [_tracks tableColumnWithIdentifier:@"Duration"].width = kDurationColumnWidth;
    [_tracks tableColumnWithIdentifier:@"Album"].width = kAlbumColumnWidth;
    [_tracks tableColumnWithIdentifier:@"Title"].width = std::max(
      kMinimumTitleColumnWidth,
      (contentWidth - kTrackNumberColumnWidth - kDurationColumnWidth - (modern != NO ? 0 : kAlbumColumnWidth)) *
        kTitleColumnShare);
    [_tracks tableColumnWithIdentifier:@"Artist"].width = std::max(
      kMinimumArtistColumnWidth,
      (contentWidth - kTrackNumberColumnWidth - kDurationColumnWidth - (modern != NO ? 0 : kAlbumColumnWidth)) *
        kArtistColumnShare);
  }

  if (_rowHeightsInitialized == NO || modeChanged)
  {
    [_tracks
      noteHeightOfRowsWithIndexesChanged:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, _tracks.numberOfRows)]];
    _rowHeightsInitialized = YES;
  }
}

- (void)refresh
{
  if (_session == nullptr)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto const workspace = _session->runtime().workspace().snapshot();
      auto viewRes = _session->runtime().views().findTrackListState(workspace.activeViewId);
      auto const emptySelection = std::vector<ao::TrackId>{};
      auto const& selection = viewRes ? viewRes->selection : emptySelection;
      auto const& state = _session->state();

      if (_tableRevision != state.tableRevision)
      {
        auto const origin = _trackScroll.contentView.bounds.origin;
        _refreshing = YES;
        _tableRevision = state.tableRevision;
        [_tracks reloadData];
        [self selectTracks:selection];
        [_trackScroll.contentView scrollToPoint:origin];
        [_trackScroll reflectScrolledClipView:_trackScroll.contentView];
        _refreshing = NO;
        _displayedSelection = selection;
      }

      if (_listRevision != state.listRevision)
      {
        auto const lists = _session->runtime().library().snapshot().lists();
        _listRevision = state.listRevision;
        _listTree = ao::uimodel::buildListTreeProjection(_session->catalog(), lists);
        _writableListIds.clear();

        for (auto const& target : ao::uimodel::writableTagListTargets(lists, _session->runtime().textOrderingPolicy()))
        {
          _writableListIds.insert(target.listId);
        }

        auto* const currentItems = [NSMutableDictionary<NSNumber*, NSNumber*> dictionary];

        for (auto const& [id, row] : _listTree.rowsById)
        {
          auto* const key = @(id.raw());
          currentItems[key] = _listItems[key] != nil ? _listItems[key] : key;
        }

        _listItems = currentItems;
        _refreshing = YES;
        [_lists reloadData];

        if (_outlineInitialized == 0)
        {
          [_lists expandItem:@"Library"];
          [_lists expandItem:@"Saved Lists"];
          _outlineInitialized = YES;
        }

        _refreshing = NO;
      }

      if (selection != _displayedSelection)
      {
        _refreshing = YES;
        [self selectTracks:selection];
        _refreshing = NO;
        _displayedSelection = selection;
      }

      if (!viewRes)
      {
        _activeListTitle = [self text:MessageId::LibraryAllTracks];
        _presentationIdentifier = @"";
        return;
      }

      _presentationIdentifier = nativeText(viewRes->presentation.id);
      _activeListTitle = [self text:MessageId::LibraryAllTracks];
      NSNumber* selectedItem = _listItems[@(ao::rt::kAllTracksListId.raw())];

      if (auto const row = _listTree.rowsById.find(viewRes->listId); row != _listTree.rowsById.end())
      {
        selectedItem = _listItems[@(row->first.raw())];
        _activeListTitle = nativeText(row->second.name);
      }

      _refreshing = YES;
      auto* const ancestors = [NSMutableArray<NSNumber*> array];

      for (auto row = _listTree.rowsById.find(viewRes->listId); row != _listTree.rowsById.end();
           row = _listTree.rowsById.find(row->second.parentId))
      {
        auto* const parent = _listItems[@(row->second.parentId.raw())];

        if (parent == nil)
        {
          break;
        }

        [ancestors insertObject:parent atIndex:0];
      }

      if (selectedItem != nil &&
          (selectedItem != _revealedListItem || [ancestors isEqualToArray:_revealedListAncestors] == 0))
      {
        [_lists expandItem:viewRes->listId == ao::rt::kAllTracksListId ? @"Library" : @"Saved Lists"];

        for (NSUInteger index = 0; index < ancestors.count; ++index)
        {
          [_lists expandItem:ancestors[index]];
        }

        _revealedListItem = selectedItem;
        _revealedListAncestors = [ancestors copy];
      }

      if (auto const selectedRow = [_lists rowForItem:selectedItem]; selectedRow >= 0)
      {
        [_lists selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(selectedRow)]
            byExtendingSelection:NO];
      }

      _refreshing = NO;
      [self refreshSortDescriptors:viewRes->presentation.sortBy];
    });
}

- (void)selectTracks:(std::vector<ao::TrackId> const&)selection
{
  auto* const indexes = [NSMutableIndexSet indexSet];

  for (auto id : selection)
  {
    if (auto const optIndex = _session->displayIndexOf(id); optIndex)
    {
      [indexes addIndex:*optIndex];
    }
  }

  [_tracks selectRowIndexes:indexes byExtendingSelection:NO];
}

- (void)refreshSortDescriptors:(std::vector<ao::rt::TrackSortTerm> const&)sortBy
{
  auto* sortDescriptors = [NSMutableArray<NSSortDescriptor*> array];

  if (!sortBy.empty())
  {
    auto const term = sortBy.front();
    NSString* key = nil;

    if (sortBy.size() >= 2 && term.field == ao::rt::TrackSortField::DiscNumber &&
        sortBy[1].field == ao::rt::TrackSortField::TrackNumber && sortBy[1].ascending == term.ascending)
    {
      key = @"#";
    }
    else
    {
      switch (term.field)
      {
        case ao::rt::TrackSortField::TrackNumber: key = @"#"; break;
        case ao::rt::TrackSortField::Title: key = @"Title"; break;
        case ao::rt::TrackSortField::Artist: key = @"Artist"; break;
        case ao::rt::TrackSortField::Album: key = @"Album"; break;
        case ao::rt::TrackSortField::Duration: key = @"Duration"; break;
        default: break;
      }
    }

    if (key != nil)
    {
      [sortDescriptors addObject:[[NSSortDescriptor alloc] initWithKey:key
                                                             ascending:static_cast<BOOL>(term.ascending)]];
    }
  }

  if ([_tracks.sortDescriptors isEqualToArray:sortDescriptors] == NO)
  {
    _refreshing = YES;
    _tracks.sortDescriptors = sortDescriptors;
    _refreshing = NO;
  }
}

- (ao::ListId)activeListId
{
  if (_session == nullptr)
  {
    return ao::kInvalidListId;
  }

  auto const workspace = _session->runtime().workspace().snapshot();
  auto const viewRes = _session->runtime().views().findTrackListState(workspace.activeViewId);
  return viewRes ? viewRes->listId : ao::kInvalidListId;
}

- (ao::ListId)clickedListId
{
  if ([self isClosing] != NO || _listRevision != _session->state().listRevision)
  {
    return ao::kInvalidListId;
  }

  auto const row = _lists.clickedRow;
  id const item = row >= 0 ? [_lists itemAtRow:row] : nil;
  return [item isKindOfClass:NSNumber.class] != NO ? ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]}
                                                   : ao::kInvalidListId;
}

- (NSString*)activeListTitle
{
  return _activeListTitle;
}

- (NSString*)presentationIdentifier
{
  return _presentationIdentifier;
}

- (std::vector<ao::TrackId>)selectedTrackIds
{
  return _session != nullptr ? _session->selection() : std::vector<ao::TrackId>{};
}

- (BOOL)prepareContextMenu:(NSMenu*)menu
{
  if ([self isClosing] != NO || [self isSheetBlocked] != NO)
  {
    return NO;
  }

  if (menu == _lists.menu)
  {
    if (_listRevision != _session->state().listRevision)
    {
      return NO;
    }

    if (_lists.clickedRow >= 0)
    {
      [_lists selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(_lists.clickedRow)]
          byExtendingSelection:NO];
    }

    return YES;
  }

  if (menu != _tracks.menu || _tableRevision != _session->state().tableRevision)
  {
    return NO;
  }

  if (_tracks.clickedRow >= 0)
  {
    if ([self tableView:_tracks isGroupRow:_tracks.clickedRow] != NO)
    {
      return NO;
    }

    if ([_tracks isRowSelected:_tracks.clickedRow] == NO)
    {
      [_tracks selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(_tracks.clickedRow)]
           byExtendingSelection:NO];
    }
  }

  return YES;
}

- (NSInteger)outlineView:(NSOutlineView*) [[maybe_unused]] outline numberOfChildrenOfItem:(id)item
{
  if (_session == nullptr)
  {
    return 0;
  }

  if (item == nil)
  {
    return 2;
  }

  if ([item isEqual:@"Library"] != NO)
  {
    return 1;
  }

  if ([item isEqual:@"Saved Lists"] != NO)
  {
    return _listTree.rootIds.empty() ? 0 : static_cast<NSInteger>(_listTree.rootIds.size() - 1);
  }

  if ([item isKindOfClass:NSNumber.class] != NO)
  {
    auto const id = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
    auto const row = _listTree.rowsById.find(id);
    return row != _listTree.rowsById.end() ? static_cast<NSInteger>(row->second.childIds.size()) : 0;
  }

  return 0;
}

- (id)outlineView:(NSOutlineView*) [[maybe_unused]] outline child:(NSInteger)index ofItem:(id)item
{
  if (_session == nullptr)
  {
    // AppKit requires a nonnull child; detached data sources advertise no rows.
    return NSNull.null;
  }

  if (item == nil)
  {
    return index == 0 ? @"Library" : @"Saved Lists";
  }

  if ([item isEqual:@"Library"] != NO)
  {
    return _listItems[@(ao::rt::kAllTracksListId.raw())];
  }

  if ([item isEqual:@"Saved Lists"] != NO)
  {
    return _listItems[@(_listTree.rootIds.at(static_cast<std::size_t>(index) + 1).raw())];
  }

  auto const id = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
  auto const childId = _listTree.rowsById.at(id).childIds.at(static_cast<std::size_t>(index));
  return _listItems[@(childId.raw())];
}

- (BOOL)outlineView:(NSOutlineView*)outline isItemExpandable:(id)item
{
  if (_session == nullptr)
  {
    return NO;
  }

  if ([item isKindOfClass:NSString.class] != NO)
  {
    return YES;
  }

  return static_cast<BOOL>([self outlineView:outline numberOfChildrenOfItem:item] > 0);
}

- (BOOL)outlineView:(NSOutlineView*) [[maybe_unused]] outline isGroupItem:(id)item
{
  return static_cast<BOOL>(_session != nullptr && [item isKindOfClass:NSString.class] != NO);
}

- (CGFloat)outlineView:(NSOutlineView*) [[maybe_unused]] outline heightOfRowByItem:(id)item
{
  return [item isKindOfClass:NSString.class] != NO ? kSidebarGroupRowHeight : kSidebarRowHeight;
}

- (BOOL)outlineView:(NSOutlineView*) [[maybe_unused]] outline shouldSelectItem:(id)item
{
  return static_cast<BOOL>(_session != nullptr && [item isKindOfClass:NSNumber.class] != NO);
}

- (NSView*)outlineView:(NSOutlineView*)outline viewForTableColumn:(NSTableColumn*) [[maybe_unused]] column item:(id)item
{
  if (_session == nullptr)
  {
    return nil;
  }

  if ([item isKindOfClass:NSString.class] != NO)
  {
    NSTableCellView* heading = [outline makeViewWithIdentifier:@"source-heading" owner:self];

    if (heading == nil)
    {
      heading = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
      heading.identifier = @"source-heading";
      auto* const text = label(@"", kCaptionFontSize, YES);
      text.font = [NSFont systemFontOfSize:kCaptionFontSize weight:NSFontWeightSemibold];
      text.translatesAutoresizingMaskIntoConstraints = NO;
      heading.textField = text;
      [heading addSubview:text];
      [NSLayoutConstraint activateConstraints:@[
        [text.leadingAnchor constraintEqualToAnchor:heading.leadingAnchor constant:4],
        [text.trailingAnchor constraintEqualToAnchor:heading.trailingAnchor constant:-4],
        [text.centerYAnchor constraintEqualToAnchor:heading.centerYAnchor]
      ]];
    }

    heading.textField.stringValue =
      [[self text:[item isEqual:@"Library"] != NO ? MessageId::AppKitLibrarySection : MessageId::AppKitSavedLists]
        uppercaseString];
    return heading;
  }

  AobusSourceCell* cell = static_cast<AobusSourceCell*>([outline makeViewWithIdentifier:@"source-item" owner:self]);

  if (cell == nil)
  {
    cell = [[AobusSourceCell alloc] initWithFrame:NSZeroRect];
    cell.identifier = @"source-item";
    auto* const icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    icon.contentTintColor = NSColor.controlAccentColor;
    auto* const text = [AobusSourceLabel labelWithString:@""];
    text.font = [NSFont systemFontOfSize:kBodyFontSize];
    text.lineBreakMode = NSLineBreakByTruncatingTail;
    text.translatesAutoresizingMaskIntoConstraints = NO;
    cell.imageView = icon;
    cell.textField = text;
    [cell addSubview:icon];
    [cell addSubview:text];
    [NSLayoutConstraint activateConstraints:@[
      [icon.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4],
      [icon.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
      [icon.widthAnchor constraintEqualToConstant:kContentInset],
      [icon.heightAnchor constraintEqualToAnchor:icon.widthAnchor],
      [text.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:8],
      [text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-kCellInset],
      [text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor]
    ]];
  }

  auto const listId = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
  auto const row = _listTree.rowsById.find(listId);
  cell.textField.stringValue = row != _listTree.rowsById.end() ? nativeText(row->second.name) : @"";
  cell.imageView.image =
    [NSImage imageWithSystemSymbolName:listId == ao::rt::kAllTracksListId ? @"music.note.house" : @"music.note.list"
              accessibilityDescription:nil];
  return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification
{
  if (notification.object != _lists || _refreshing != NO || [self isClosing] != NO)
  {
    return;
  }

  if (auto const row = _lists.selectedRow; row >= 0)
  {
    if (id const item = [_lists itemAtRow:row]; [item isKindOfClass:NSNumber.class] != NO)
    {
      nativeCallback([&] { _session->navigate(ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]}); });
    }
  }
}

- (NSDragOperation)outlineView:(NSOutlineView*)outline
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item
            proposedChildIndex:(NSInteger)index
{
  if (outline != _lists || [self isClosing] != NO || [self isSheetBlocked] != NO ||
      [item isKindOfClass:NSNumber.class] == NO ||
      [info.draggingPasteboard availableTypeFromArray:@[kTrackPasteboardType]] == nil)
  {
    return NSDragOperationNone;
  }

  if (_listRevision != _session->state().listRevision)
  {
    return NSDragOperationNone;
  }

  auto const listId = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};

  if (!_writableListIds.contains(listId))
  {
    return NSDragOperationNone;
  }

  if (index != NSOutlineViewDropOnItemIndex)
  {
    [outline setDropItem:item dropChildIndex:NSOutlineViewDropOnItemIndex];
  }

  return NSDragOperationCopy;
}

- (BOOL)outlineView:(NSOutlineView*)outline
         acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item
         childIndex:(NSInteger) [[maybe_unused]] index
{
  if ([self outlineView:outline validateDrop:info proposedItem:item proposedChildIndex:index] == NSDragOperationNone)
  {
    return NO;
  }

  auto trackIds = std::vector<ao::TrackId>{};
  auto* const pasteboardItems = info.draggingPasteboard.pasteboardItems;

  for (NSUInteger pasteboardIndex = 0; pasteboardIndex < pasteboardItems.count; ++pasteboardIndex)
  {
    auto* const pasteboardItem = pasteboardItems[pasteboardIndex];

    if (NSString* const value = [pasteboardItem stringForType:kTrackPasteboardType]; value != nil)
    {
      if (auto const raw = value.longLongValue;
          raw > 0 && std::cmp_less_equal(raw, std::numeric_limits<std::uint32_t>::max()))
      {
        trackIds.emplace_back(static_cast<std::uint32_t>(raw));
      }
    }
  }

  std::ranges::sort(trackIds, {}, &ao::TrackId::raw);
  auto const [firstDuplicate, last] = std::ranges::unique(trackIds);
  trackIds.erase(firstDuplicate, last);

  if (trackIds.empty())
  {
    return NO;
  }

  auto const delegate = _delegate;
  auto const listId = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
  BOOL accepted = NO;
  nativeCallback(
    [&]
    {
      if ([self isClosing] == NO && [self isSheetBlocked] == 0 && delegate != nil)
      {
        accepted = [delegate libraryBrowser:self requestMembershipForTracks:trackIds listId:listId];
      }
    });
  return accepted;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)table
{
  if (table != _tracks || [self isClosing] != NO)
  {
    return 0;
  }

  return static_cast<NSInteger>(_session->displayIndex().displayCount());
}

- (CGFloat)tableView:(NSTableView*)table heightOfRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO)
  {
    return _modern != NO ? 32 : kClassicRowHeight;
  }

  auto const optItem = _session->displayIndex().itemAt(static_cast<std::size_t>(row));

  if (optItem && optItem->kind == ao::uimodel::TrackDisplayItemKind::GroupHeader)
  {
    return _modern != NO ? kModernGroupHeight : kClassicGroupRowHeight;
  }

  return _modern != NO ? 32 : kClassicRowHeight;
}

- (BOOL)tableView:(NSTableView*)table isGroupRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO)
  {
    return NO;
  }

  auto const optItem = _session->displayIndex().itemAt(static_cast<std::size_t>(row));
  return static_cast<BOOL>(optItem && optItem->kind == ao::uimodel::TrackDisplayItemKind::GroupHeader);
}

- (BOOL)tableView:(NSTableView*)table shouldSelectRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO || _tableRevision != _session->state().tableRevision)
  {
    return NO;
  }

  return [self tableView:table isGroupRow:row] == NO ? YES : NO;
}

- (NSView*)tableView:(NSTableView*)table viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO)
  {
    return nil;
  }

  NSView* result = nil;
  auto cell = [&](NSUserInterfaceItemIdentifier identifier, CGFloat height, CGFloat width)
  {
    auto* reusable = static_cast<AobusTrackCell*>([table makeViewWithIdentifier:identifier owner:self]);

    if (reusable == nil)
    {
      reusable = [[AobusTrackCell alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
      reusable.identifier = identifier;
      auto* const primary = label(@"", kBodyFontSize);
      primary.translatesAutoresizingMaskIntoConstraints = NO;
      reusable.textField = primary;
      [reusable addSubview:primary];
    }

    reusable.frame = NSMakeRect(0, 0, width, height);
    reusable.needsLayout = YES;
    return reusable;
  };
  nativeCallback(
    [&]
    {
      auto const optItem = _session->displayIndex().itemAt(static_cast<std::size_t>(row));

      if (!optItem)
      {
        return;
      }

      if (optItem->kind == ao::uimodel::TrackDisplayItemKind::GroupHeader)
      {
        auto const heading = _session->groupHeading(optItem->groupIndex);
        auto const height = [self tableView:table heightOfRow:row];
        auto const width = table.bounds.size.width;
        auto* const group =
          cell(_modern != NO ? kModernGroupCellIdentifier : kClassicGroupCellIdentifier, height, width);
        auto* const text = group.textField;
        text.stringValue = nativeText(heading.primaryText);
        text.alignment = NSTextAlignmentLeft;
        text.textColor = NSColor.labelColor;
        text.font = [NSFont systemFontOfSize:_modern != NO ? kHeadingFontSize : kClassicGroupFontSize
                                      weight:NSFontWeightSemibold];

        if (_modern != NO)
        {
          auto* secondary = group.secondaryTextField;

          if (secondary == nil)
          {
            secondary = label(@"", kSecondaryFontSize, YES);
            secondary.translatesAutoresizingMaskIntoConstraints = NO;
            group.secondaryTextField = secondary;
            [group addSubview:secondary];
          }

          secondary.stringValue = nativeText(heading.secondaryText);
          secondary.toolTip = secondary.stringValue;
          secondary.alignment = NSTextAlignmentLeft;
          secondary.textColor = NSColor.secondaryLabelColor;
          secondary.font = [NSFont systemFontOfSize:kSecondaryFontSize];
        }
        else
        {
          text.stringValue = nativeText(heading.primaryText + "   ·   " + heading.secondaryText);
        }

        text.toolTip = text.stringValue;
        result = group;
        return;
      }

      auto const* track = _session->rowAt(static_cast<std::size_t>(row));

      if (track == nullptr)
      {
        return;
      }

      auto text = std::string{};
      auto* const identifier = column.identifier;

      if ([identifier isEqual:@"Title"] != NO)
      {
        text = track->title;
      }
      else if ([identifier isEqual:@"Artist"] != NO)
      {
        text = track->artist;
      }
      else if ([identifier isEqual:@"Album"] != NO)
      {
        text = track->album;
      }
      else if ([identifier isEqual:@"Duration"] != NO)
      {
        text = ao::uimodel::formatDuration(track->duration);
      }
      else
      {
        text = ao::uimodel::formatDisplayTrackNumber(track->discNumber, track->discTotal, track->trackNumber);
      }

      auto const height = [self tableView:table heightOfRow:row];
      auto* const trackCell = cell(identifier, height, column.width);
      auto* const field = trackCell.textField;
      field.stringValue = nativeText(text);
      field.toolTip = field.stringValue;
      field.alignment = NSTextAlignmentLeft;
      field.textColor = [identifier isEqual:@"Title"] != NO ? NSColor.labelColor : NSColor.secondaryLabelColor;
      field.font = [NSFont systemFontOfSize:_modern != NO ? kBodyFontSize : kSecondaryFontSize];

      if ([identifier isEqual:@"Duration"] != NO || [identifier isEqual:@"#"] != NO)
      {
        field.alignment = NSTextAlignmentRight;
        field.font = [NSFont monospacedDigitSystemFontOfSize:_modern != NO ? kSecondaryFontSize : kCaptionFontSize
                                                      weight:NSFontWeightRegular];
      }

      result = trackCell;
    });
  return result;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification
{
  if (notification.object != _tracks || _refreshing != NO || [self isClosing] != NO ||
      _tableRevision != _session->state().tableRevision)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto trackIds = std::vector<ao::TrackId>{};
      auto* const indexes = _tracks.selectedRowIndexes;

      for (auto index = indexes.firstIndex; index != NSNotFound; index = [indexes indexGreaterThanIndex:index])
      {
        if (auto const* row = _session->rowAt(index); row != nullptr)
        {
          trackIds.push_back(row->id);
        }
      }

      _session->select(trackIds);
      _displayedSelection = trackIds;

      if (auto const delegate = _delegate; delegate != nil && [delegate isLibraryBrowserClosing:self] == 0)
      {
        [delegate libraryBrowserSelectionDidChange:self];
      }
    });
}

- (id<NSPasteboardWriting>)tableView:(NSTableView*)table pasteboardWriterForRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO || _tableRevision != _session->state().tableRevision || row < 0)
  {
    return nil;
  }

  auto const* track = _session->rowAt(static_cast<std::size_t>(row));

  if (track == nullptr)
  {
    return nil;
  }

  auto* const item = [[NSPasteboardItem alloc] init];
  [item setString:nativeText(std::format("{}", track->id.raw())) forType:kTrackPasteboardType];
  return item;
}

- (void)tableView:(NSTableView*)table
  sortDescriptorsDidChange:(NSArray<NSSortDescriptor*>*) [[maybe_unused]] oldDescriptors
{
  if (table != _tracks || _refreshing != NO || [self isClosing] != NO || table.sortDescriptors.count == 0)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto* const descriptor = table.sortDescriptors.firstObject;
      auto const key = descriptor.key;
      auto optField = std::optional<ao::rt::TrackSortField>{};

      if ([key isEqual:@"#"] != 0)
      {
        optField = ao::rt::TrackSortField::TrackNumber;
      }
      else if ([key isEqual:@"Title"] != 0)
      {
        optField = ao::rt::TrackSortField::Title;
      }
      else if ([key isEqual:@"Artist"] != 0)
      {
        optField = ao::rt::TrackSortField::Artist;
      }
      else if ([key isEqual:@"Album"] != 0)
      {
        optField = ao::rt::TrackSortField::Album;
      }
      else if ([key isEqual:@"Duration"] != 0)
      {
        optField = ao::rt::TrackSortField::Duration;
      }

      if (optField)
      {
        if (auto res = _session->sort(*optField, descriptor.ascending != 0); !res)
        {
          showError(_tracks.window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
        }
      }
    });
}

- (void)playTrack:(id) [[maybe_unused]] sender
{
  if ([self isClosing] != NO || _tracks.clickedRow < 0 || _tableRevision != _session->state().tableRevision)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      if (auto const* row = _session->rowAt(static_cast<std::size_t>(_tracks.clickedRow)); row != nullptr)
      {
        _session->play(row->id);
      }
    });
}

- (void)playSelectedTrack
{
  if ([self isClosing] != NO || _tableRevision != _session->state().tableRevision)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      if (auto const trackIds = _session->selection(); !trackIds.empty())
      {
        _session->play(trackIds.front());
      }
    });
}

- (void)playSelection:(id) [[maybe_unused]] sender
{
  [self playSelectedTrack];
}

- (void)togglePlayback:(id) [[maybe_unused]] sender
{
  if ([self isClosing] == NO)
  {
    nativeCallback([&] { _session->execute(ao::uimodel::PlaybackCommand::PlayPause); });
  }
}
@end
