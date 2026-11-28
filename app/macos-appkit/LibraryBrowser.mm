// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryBrowser.h"

#include "AppKitText.h"
#include "LibraryBrowserModel.h"
#include "NativeCallback.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/field/TrackRowField.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/track/TrackDisplayIndex.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace
{
  constexpr CGFloat kCellInset = 6;
  constexpr CGFloat kContentGap = 12;
  constexpr CGFloat kContentInset = 16;
  constexpr CGFloat kCaptionFontSize = 10;
  constexpr CGFloat kSecondaryFontSize = 11;
  constexpr CGFloat kBodyFontSize = 13;
  constexpr CGFloat kHeadingFontSize = 14;
  constexpr CGFloat kClassicGroupFontSize = 12;
  constexpr CGFloat kSidebarGroupRowHeight = 26;
  constexpr CGFloat kSidebarRowHeight = 34;
  constexpr CGFloat kTrackTextLineHeight = 18;
  constexpr CGFloat kTrackHeadingPrimaryTop = 18;
  constexpr CGFloat kTitleLineHeight = 19;
  constexpr CGFloat kSecondaryLineHeight = 17;
  constexpr CGFloat kClassicRowHeight = 22;
  constexpr CGFloat kClassicGroupRowHeight = 38;
  constexpr CGFloat kModernGroupHeight = 44;
  constexpr CGFloat kTrackNumberColumnWidth = 44;
  constexpr CGFloat kMinimumNumberColumnWidth = 36;
  constexpr CGFloat kMinimumTextColumnWidth = 65;
  constexpr CGFloat kInitialDurationColumnWidth = 70;
  constexpr CGFloat kDurationColumnWidth = 72;
  constexpr CGFloat kMinimumTitleColumnWidth = 160.0;
  constexpr CGFloat kInitialTitleColumnWidth = 260;
  constexpr CGFloat kMinimumArtistColumnWidth = 130.0;
  constexpr CGFloat kAlbumColumnWidth = 170;
  constexpr CGFloat kTitleColumnShare = 0.58;
  constexpr CGFloat kArtistColumnShare = 0.42;
  constexpr double kTypeSelectionContinuationSeconds = 1.0;
  NSPasteboardType const kTrackPasteboardType = @"org.aobus.track-id";
  NSUserInterfaceItemIdentifier const kClassicGroupCellIdentifier = @"track-group-classic";
  NSUserInterfaceItemIdentifier const kModernGroupCellIdentifier = @"track-group-modern";

  using ao::appkit::nativeCallback;
  using ao::appkit::nativeText;
  using ao::i18n::MessageId;

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
- (BOOL)canAcceptTrackDrop:(id<NSDraggingInfo>)info outlineView:(NSOutlineView*)outline item:(id)item;
- (NSString*)text:(MessageId)message;
- (void)selectTracks:(std::vector<ao::TrackId> const&)selection;
- (void)refreshSortDescriptors:(std::optional<ao::rt::TrackColumnSort> const&)optColumnSort;
- (void)playTrack:(id)sender;
- (void)playSelection:(id)sender;
- (void)togglePlayback:(id)sender;
@end

@implementation AobusLibraryBrowser {
  // The composition root revokes this borrow with detach before closing the session.
  ao::appkit::LibraryBrowserModel* _model;
  __weak id<AobusLibraryBrowserDelegate> _delegate;
  NSScrollView* _trackScroll;
  AobusTrackTable* _tracks;
  NSScrollView* _listScroll;
  NSOutlineView* _lists;
  NSMutableDictionary<NSNumber*, NSNumber*>* _listItems;
  ao::appkit::LibraryBrowserModel::Sidebar _sidebar;
  std::vector<ao::TrackId> _displayedSelection;
  ao::rt::ViewId _renderedViewId;
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
}

- (instancetype)initWithModel:(ao::appkit::LibraryBrowserModel&)model delegate:(id<AobusLibraryBrowserDelegate>)delegate
{
  self = [super init];

  if (self != nil)
  {
    _model = &model;
    _delegate = delegate;
    _renderedViewId = ao::rt::kInvalidViewId;
    _tableRevision = UINT64_MAX;
    _listRevision = UINT64_MAX;
    _activeListTitle = ao::appkit::catalogText(model.catalog(), MessageId::LibraryAllTracks);
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
        column.title = ao::appkit::catalogText(model.catalog(), MessageId::TrackFieldTitle);
      }
      else if ([identifier isEqual:@"Artist"] != NO)
      {
        column.title = ao::appkit::catalogText(model.catalog(), MessageId::TrackFieldArtist);
      }
      else if ([identifier isEqual:@"Album"] != NO)
      {
        column.title = ao::appkit::catalogText(model.catalog(), MessageId::TrackFieldAlbum);
      }
      else if ([identifier isEqual:@"Duration"] != NO)
      {
        column.title = ao::appkit::catalogText(model.catalog(), MessageId::TrackFieldDuration);
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
  if (_model == nullptr)
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
  _model = nullptr;
}

- (void)invalidateProjection
{
  _tableRevision = UINT64_MAX;
}

- (BOOL)isClosing
{
  auto const delegate = _delegate;
  return static_cast<BOOL>(_model == nullptr || delegate == nil || [delegate isLibraryBrowserClosing:self] != NO);
}

- (BOOL)isSheetBlocked
{
  auto const delegate = _delegate;
  return static_cast<BOOL>(delegate == nil || [delegate isLibraryBrowserSheetBlocked:self] != NO);
}

- (NSString*)text:(MessageId)message
{
  return _model != nullptr ? ao::appkit::catalogText(_model->catalog(), message) : @"";
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
  if (_model == nullptr)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto const activeListId = _model->activeListId();
      auto const table = _model->boundTable();
      auto const selection = _model->boundSelection();

      if (_tableRevision != table.revision)
      {
        auto const origin = _trackScroll.contentView.bounds.origin;
        _refreshing = YES;
        _renderedViewId = table.viewId;
        _tableRevision = table.revision;
        [_tracks reloadData];
        [self selectTracks:selection];
        [_trackScroll.contentView scrollToPoint:origin];
        [_trackScroll reflectScrolledClipView:_trackScroll.contentView];
        _refreshing = NO;
        _displayedSelection = selection;
      }

      if (auto const revision = _model->listRevision(); _listRevision != revision)
      {
        _sidebar = _model->projectSidebar();
        _listRevision = revision;
        auto* const currentItems = [NSMutableDictionary<NSNumber*, NSNumber*> dictionary];

        for (auto const& [id, row] : _sidebar.tree.rowsById)
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

      [self refreshSortDescriptors:_model->renderedSort(_renderedViewId)];
      _activeListTitle = [self text:MessageId::LibraryAllTracks];

      if (activeListId == ao::kInvalidListId)
      {
        return;
      }

      NSNumber* selectedItem = _listItems[@(ao::rt::kAllTracksListId.raw())];

      if (auto const row = _sidebar.tree.rowsById.find(activeListId); row != _sidebar.tree.rowsById.end())
      {
        selectedItem = _listItems[@(row->first.raw())];
        _activeListTitle = nativeText(row->second.name);
      }

      _refreshing = YES;
      auto* const ancestors = [NSMutableArray<NSNumber*> array];

      for (auto const parentId : ao::uimodel::listTreeAncestorIds(_sidebar.tree, activeListId))
      {
        auto* const parent = _listItems[@(parentId.raw())];

        if (parent == nil)
        {
          break;
        }

        [ancestors addObject:parent];
      }

      if (selectedItem != nil &&
          (selectedItem != _revealedListItem || [ancestors isEqualToArray:_revealedListAncestors] == 0))
      {
        [_lists expandItem:activeListId == ao::rt::kAllTracksListId ? @"Library" : @"Saved Lists"];

        // Objective-C fast enumeration assigns parent before each iteration.
        // NOLINTNEXTLINE(aobus-modernize-local-initialization-style,cppcoreguidelines-init-variables)
        for (NSNumber* parent in ancestors.reverseObjectEnumerator)
        {
          [_lists expandItem:parent];
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
    });
}

- (void)selectTracks:(std::vector<ao::TrackId> const&)selection
{
  auto* const indexes = [NSMutableIndexSet indexSet];

  for (auto id : selection)
  {
    if (auto const optIndex = _model->displayIndexOf(id); optIndex)
    {
      [indexes addIndex:*optIndex];
    }
  }

  [_tracks selectRowIndexes:indexes byExtendingSelection:NO];
}

- (void)refreshSortDescriptors:(std::optional<ao::rt::TrackColumnSort> const&)optColumnSort
{
  auto* sortDescriptors = [NSMutableArray<NSSortDescriptor*> array];

  if (optColumnSort)
  {
    NSString* key = nil;

    switch (optColumnSort->column)
    {
      case ao::rt::TrackField::DisplayTrackNumber: key = @"#"; break;
      case ao::rt::TrackField::Title: key = @"Title"; break;
      case ao::rt::TrackField::Artist: key = @"Artist"; break;
      case ao::rt::TrackField::Album: key = @"Album"; break;
      case ao::rt::TrackField::Duration: key = @"Duration"; break;
      default: break;
    }

    if (key != nil)
    {
      auto* const descriptor = [[NSSortDescriptor alloc] initWithKey:key
                                                           ascending:static_cast<BOOL>(optColumnSort->ascending)];
      [sortDescriptors addObject:descriptor];
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
  return _model != nullptr ? _model->activeListId() : ao::kInvalidListId;
}

- (ao::ListId)clickedListId
{
  if ([self isClosing] != NO || _listRevision != _model->listRevision())
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

- (std::vector<ao::TrackId>)selectedTrackIds
{
  return _model != nullptr ? _model->boundSelection() : std::vector<ao::TrackId>{};
}

- (BOOL)prepareContextMenu:(NSMenu*)menu
{
  if ([self isClosing] != NO || [self isSheetBlocked] != NO)
  {
    return NO;
  }

  if (menu == _lists.menu)
  {
    if (_listRevision != _model->listRevision())
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

  if (menu != _tracks.menu || _tableRevision != _model->boundTable().revision)
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
  if (_model == nullptr)
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
    return _sidebar.tree.rootIds.empty() ? 0 : static_cast<NSInteger>(_sidebar.tree.rootIds.size() - 1);
  }

  if ([item isKindOfClass:NSNumber.class] != NO)
  {
    auto const id = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
    auto const row = _sidebar.tree.rowsById.find(id);
    return row != _sidebar.tree.rowsById.end() ? static_cast<NSInteger>(row->second.childIds.size()) : 0;
  }

  return 0;
}

- (id)outlineView:(NSOutlineView*) [[maybe_unused]] outline child:(NSInteger)index ofItem:(id)item
{
  if (_model == nullptr)
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
    return _listItems[@(_sidebar.tree.rootIds.at(static_cast<std::size_t>(index) + 1).raw())];
  }

  auto const id = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
  auto const childId = _sidebar.tree.rowsById.at(id).childIds.at(static_cast<std::size_t>(index));
  return _listItems[@(childId.raw())];
}

- (BOOL)outlineView:(NSOutlineView*)outline isItemExpandable:(id)item
{
  if (_model == nullptr)
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
  return static_cast<BOOL>(_model != nullptr && [item isKindOfClass:NSString.class] != NO);
}

- (CGFloat)outlineView:(NSOutlineView*) [[maybe_unused]] outline heightOfRowByItem:(id)item
{
  return [item isKindOfClass:NSString.class] != NO ? kSidebarGroupRowHeight : kSidebarRowHeight;
}

- (BOOL)outlineView:(NSOutlineView*) [[maybe_unused]] outline shouldSelectItem:(id)item
{
  return static_cast<BOOL>(_model != nullptr && [item isKindOfClass:NSNumber.class] != NO);
}

- (NSView*)outlineView:(NSOutlineView*)outline viewForTableColumn:(NSTableColumn*) [[maybe_unused]] column item:(id)item
{
  if (_model == nullptr)
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
  auto const row = _sidebar.tree.rowsById.find(listId);
  cell.textField.stringValue = row != _sidebar.tree.rowsById.end() ? nativeText(row->second.name) : @"";
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
      nativeCallback([&] { _model->navigateToList(ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]}); });
    }
  }
}

- (BOOL)canAcceptTrackDrop:(id<NSDraggingInfo>)info outlineView:(NSOutlineView*)outline item:(id)item
{
  if (outline != _lists || [self isClosing] != NO || [self isSheetBlocked] != NO ||
      [item isKindOfClass:NSNumber.class] == NO ||
      [info.draggingPasteboard availableTypeFromArray:@[kTrackPasteboardType]] == nil)
  {
    return NO;
  }

  if (_listRevision != _model->listRevision())
  {
    return NO;
  }

  auto const listId = ao::ListId{[static_cast<NSNumber*>(item) unsignedIntValue]};
  return static_cast<BOOL>(_sidebar.writableListIds.contains(listId));
}

- (NSDragOperation)outlineView:(NSOutlineView*)outline
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item
            proposedChildIndex:(NSInteger)index
{
  if ([self canAcceptTrackDrop:info outlineView:outline item:item] == NO)
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
  if ([self canAcceptTrackDrop:info outlineView:outline item:item] == NO)
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

  return static_cast<NSInteger>(_model->boundTable().index.displayCount());
}

- (CGFloat)tableView:(NSTableView*)table heightOfRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO)
  {
    return _modern != NO ? 32 : kClassicRowHeight;
  }

  auto const optItem = _model->boundTable().index.itemAt(static_cast<std::size_t>(row));

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

  auto const optItem = _model->boundTable().index.itemAt(static_cast<std::size_t>(row));
  return static_cast<BOOL>(optItem && optItem->kind == ao::uimodel::TrackDisplayItemKind::GroupHeader);
}

- (BOOL)tableView:(NSTableView*)table shouldSelectRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO || _tableRevision != _model->boundTable().revision)
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
      auto const optItem = _model->boundTable().index.itemAt(static_cast<std::size_t>(row));

      if (!optItem)
      {
        return;
      }

      if (optItem->kind == ao::uimodel::TrackDisplayItemKind::GroupHeader)
      {
        auto const heading = _model->groupHeading(optItem->groupIndex);
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

      auto const* track = _model->rowAt(static_cast<std::size_t>(row));

      if (track == nullptr)
      {
        return;
      }

      auto* const identifier = column.identifier;
      auto columnField = ao::rt::TrackField::DisplayTrackNumber;

      if ([identifier isEqual:@"Title"] != NO)
      {
        columnField = ao::rt::TrackField::Title;
      }
      else if ([identifier isEqual:@"Artist"] != NO)
      {
        columnField = ao::rt::TrackField::Artist;
      }
      else if ([identifier isEqual:@"Album"] != NO)
      {
        columnField = ao::rt::TrackField::Album;
      }
      else if ([identifier isEqual:@"Duration"] != NO)
      {
        columnField = ao::rt::TrackField::Duration;
      }

      auto const text = ao::uimodel::formatTrackRowField(
        _model->catalog(), *track, columnField, ao::uimodel::TrackRowPathFormat::Native);
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
      _tableRevision != _model->boundTable().revision)
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
        if (auto const* row = _model->rowAt(index); row != nullptr)
        {
          trackIds.push_back(row->id);
        }
      }

      _model->selectTracks(trackIds);
      _displayedSelection = std::move(trackIds);

      if (auto const delegate = _delegate; delegate != nil && [delegate isLibraryBrowserClosing:self] == 0)
      {
        [delegate libraryBrowserSelectionDidChange:self];
      }
    });
}

- (id<NSPasteboardWriting>)tableView:(NSTableView*)table pasteboardWriterForRow:(NSInteger)row
{
  if (table != _tracks || [self isClosing] != NO || _tableRevision != _model->boundTable().revision || row < 0)
  {
    return nil;
  }

  auto const* track = _model->rowAt(static_cast<std::size_t>(row));

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
  if (table != _tracks || _refreshing != NO || [self isClosing] != NO ||
      _tableRevision != _model->boundTable().revision || table.sortDescriptors.count == 0)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      auto const key = table.sortDescriptors.firstObject.key;
      auto optColumn = std::optional<ao::rt::TrackField>{};

      if ([key isEqual:@"#"] != 0)
      {
        optColumn = ao::rt::TrackField::DisplayTrackNumber;
      }
      else if ([key isEqual:@"Title"] != 0)
      {
        optColumn = ao::rt::TrackField::Title;
      }
      else if ([key isEqual:@"Artist"] != 0)
      {
        optColumn = ao::rt::TrackField::Artist;
      }
      else if ([key isEqual:@"Album"] != 0)
      {
        optColumn = ao::rt::TrackField::Album;
      }
      else if ([key isEqual:@"Duration"] != 0)
      {
        optColumn = ao::rt::TrackField::Duration;
      }

      if (optColumn)
      {
        if (auto const res = _model->toggleColumnSort(_renderedViewId, *optColumn); !res)
        {
          showError(_tracks.window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
        }
      }
    });
}

- (void)playTrack:(id) [[maybe_unused]] sender
{
  if ([self isClosing] != NO || _tracks.clickedRow < 0 || _tableRevision != _model->boundTable().revision)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      if (auto const* row = _model->rowAt(static_cast<std::size_t>(_tracks.clickedRow)); row != nullptr)
      {
        _model->playTrack(row->id);
      }
    });
}

- (void)playSelectedTrack
{
  if ([self isClosing] != NO || _tableRevision != _model->boundTable().revision)
  {
    return;
  }

  nativeCallback([&] { _model->playSelection(); });
}

- (void)playSelection:(id) [[maybe_unused]] sender
{
  [self playSelectedTrack];
}

- (void)togglePlayback:(id) [[maybe_unused]] sender
{
  if ([self isClosing] == NO)
  {
    nativeCallback([&] { _model->togglePlayback(); });
  }
}
@end
