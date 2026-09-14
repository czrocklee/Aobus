// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackInspector.h"

#include "AppKitText.h"
#include "ArtworkView.h"
#include "DesktopControls.h"
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/utility/Path.h>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace
{
  using ao::appkit::nativeText;
  using ao::i18n::MessageId;
  constexpr auto kContentGap = 12;
  constexpr auto kContentInset = 16;
  constexpr auto kOuterInset = 20;
  constexpr auto kSecondaryFontSize = 11;
  constexpr auto kBodyFontSize = 13;
  constexpr auto kLargeTitleFontSize = 20;
  constexpr auto kControlHeight = 28;
  constexpr auto kInitialPaneHeight = 600;
  constexpr auto kInitialInspectorWidth = 270;
  constexpr auto kInspectorTextVerticalInsets = 86;
  constexpr auto kMinimumInspectorTextHeight = 500.0;
  constexpr auto kInspectorSheetWidth = 420;
  constexpr auto kInspectorSheetHeight = 540;
  constexpr auto kInspectorSheetFooterHeight = 90;
  constexpr auto kInspectorSheetButtonHeight = 30;
  constexpr auto kInspectorDoneButtonWidth = 90;
  constexpr auto kSheetDoneLeading = 310;
  constexpr auto kInspectorEditButtonWidth = 146;
  void setFrame(NSView* view, CGFloat left, CGFloat top, CGFloat width, CGFloat height)
  {
    view.frame = NSMakeRect(left, top, std::max(0.0, width), std::max(0.0, height));
  }
} // namespace

@interface AobusInspectorPanel : NSPanel
@property (nonatomic, copy) void (^dismissHandler)(void);
@end

@implementation AobusInspectorPanel
- (void)cancelOperation:(id) [[maybe_unused]] sender
{
  if (self.dismissHandler != nil)
  {
    self.dismissHandler();
  }
}

- (BOOL)accessibilityPerformCancel
{
  if (self.dismissHandler == nil)
  {
    return NO;
  }

  self.dismissHandler();
  return YES;
}
@end

@implementation AobusTrackInspector {
  std::optional<ao::i18n::MessageCatalog> _optCatalog;
  AobusInspectorAction _revealHandler;
  AobusInspectorAction _propertiesHandler;
  AobusInspectorAction _dismissHandler;
  NSImageView* _detailCover;
  NSScrollView* _detailScroll;
  NSTextView* _detailText;
  NSButton* _reveal;
  NSButton* _properties;
  NSPanel* _sheet;
  ao::rt::ResourceBytes _selectedBytes;
  std::size_t _selectionCount;
  BOOL _modern;
  BOOL _detached;
}

- (instancetype)initWithCatalog:(ao::i18n::MessageCatalog)catalog
                  revealHandler:(AobusInspectorAction)revealHandler
              propertiesHandler:(AobusInspectorAction)propertiesHandler
                 dismissHandler:(AobusInspectorAction)dismissHandler
{
  self = [super initWithNibName:nil bundle:nil];

  if (self == nil)
  {
    return nil;
  }

  _optCatalog.emplace(std::move(catalog));
  _revealHandler = [revealHandler copy];
  _propertiesHandler = [propertiesHandler copy];
  _dismissHandler = [dismissHandler copy];

  if (@available(macOS 26.0, *))
  {
    self.view = [[AobusFlippedView alloc] initWithFrame:NSMakeRect(0, 0, kInitialInspectorWidth, kInitialPaneHeight)];
  }
  else
  {
    auto* const surface =
      [[AobusSurface alloc] initWithFrame:NSMakeRect(0, 0, kInitialInspectorWidth, kInitialPaneHeight)];
    surface.material = NSVisualEffectMaterialSidebar;
    self.view = surface;
  }

  _detailCover = [[AobusArtworkView alloc] initWithFrame:NSZeroRect];
  _detailCover.imageScaling = NSImageScaleProportionallyUpOrDown;
  _detailCover.accessibilityElement = YES;
  _detailCover.accessibilityLabel = [self text:MessageId::AppKitSelectedArtwork];
  _detailCover.accessibilityValue = [self text:MessageId::CoverArtNone];
  _detailCover.image = nil;
  _detailCover.contentTintColor = NSColor.tertiaryLabelColor;
  [self.view addSubview:_detailCover];
  _detailText = [[NSTextView alloc] initWithFrame:NSZeroRect];
  _detailText.editable = NO;
  _detailText.selectable = YES;
  _detailText.drawsBackground = NO;
  _detailText.textContainerInset = NSMakeSize(4, 8);
  _detailScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  _detailScroll.documentView = _detailText;
  _detailScroll.hasVerticalScroller = YES;
  _detailScroll.drawsBackground = NO;
  [self.view addSubview:_detailScroll];
  _reveal = [NSButton buttonWithTitle:[self text:MessageId::AppKitRevealInFinder]
                               target:self
                               action:@selector(reveal:)];
  [self.view addSubview:_reveal];
  _properties = [NSButton buttonWithTitle:[self text:MessageId::AppKitEditPropertiesAction]
                                   target:self
                                   action:@selector(editProperties:)];
  [self.view addSubview:_properties];
  return self;
}

- (NSString*)text:(MessageId)message
{
  return ao::appkit::catalogText(*_optCatalog, message);
}

- (NSPanel*)sheet
{
  return _sheet;
}

- (void)renderSelectionCount:(std::size_t)count
                         row:(std::optional<ao::rt::TrackRow> const&)optRow
                   canReveal:(BOOL)canReveal
{
  if (_detached != NO)
  {
    return;
  }

  _selectionCount = count;
  _reveal.toolTip = nil;
  auto* const text = [[NSMutableAttributedString alloc] init];
  auto append = [&](NSString* value, CGFloat size, NSColor* color, BOOL emphasis, CGFloat spacing)
  {
    auto* const paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.paragraphSpacing = spacing;
    paragraph.lineSpacing = 3;
    auto* const part = [[NSAttributedString alloc]
      initWithString:[value stringByAppendingString:@"\n"]
          attributes:@{
            NSFontAttributeName: [NSFont systemFontOfSize:size
                                                   weight:emphasis != 0 ? NSFontWeightSemibold : NSFontWeightRegular],
            NSForegroundColorAttributeName: color,
            NSParagraphStyleAttributeName: paragraph
          }];
    [text appendAttributedString:part];
  };
  auto detail = [&](NSString* name, std::string const& value)
  {
    if (!value.empty())
    {
      append(name, kSecondaryFontSize, NSColor.secondaryLabelColor, NO, 2);
      append(nativeText(value), kBodyFontSize, NSColor.labelColor, NO, kContentGap);
    }
  };

  if (count == 1)
  {
    if (optRow)
    {
      auto const& row = *optRow;
      append(nativeText(row.title), kLargeTitleFontSize, NSColor.labelColor, YES, 4);
      append(nativeText(row.artist), kBodyFontSize, NSColor.secondaryLabelColor, NO, kOuterInset);
      detail([self text:MessageId::TrackFieldAlbum], row.album);
      detail([self text:MessageId::TrackFieldGenre], row.genre);

      if (row.year != 0)
      {
        detail([self text:MessageId::TrackFieldYear], std::format("{}", row.year));
      }

      if (row.trackNumber != 0)
      {
        detail([self text:MessageId::TrackFieldTrackNumber],
               row.trackTotal != 0 ? ao::i18n::requiredFormat(*_optCatalog,
                                                              MessageId::AppKitTrackOfTotal,
                                                              {{"track", row.trackNumber}, {"total", row.trackTotal}})
                                   : std::format("{}", row.trackNumber));
      }

      detail([self text:MessageId::AppKitAudio],
             ao::uimodel::formatTechnicalSummary(row.codec, row.sampleRate, row.bitDepth, row.bitrate));

      if (row.optUriPath)
      {
        detail([self text:MessageId::AppKitFileLabel], ao::utility::pathToUtf8(row.optUriPath->filename()));
        _reveal.toolTip = nativeText(ao::utility::pathToUtf8(*row.optUriPath));
      }
    }
  }
  else
  {
    append((count == 0) ? [self text:MessageId::AppKitTrackDetails]
                        : ao::appkit::catalogFormat(*_optCatalog, MessageId::AppKitTracksSelected, {{"count", count}}),
           kLargeTitleFontSize,
           NSColor.labelColor,
           YES,
           kContentGap);
    append(
      (count == 0) ? [self text:MessageId::AppKitSelectionDetailsHint] : [self text:MessageId::AppKitSharedDetailsHint],
      kBodyFontSize,
      NSColor.secondaryLabelColor,
      NO,
      kContentGap);
  }

  [_detailText.textStorage setAttributedString:text];
  _reveal.enabled = canReveal;
  _properties.enabled = static_cast<BOOL>(!(count == 0));
}

- (void)renderArtwork:(ao::rt::ResourceBytes const&)bytes
{
  if (_detached != NO)
  {
    return;
  }

  if (auto const span = bytes.view(); _selectedBytes.view().data() != span.data())
  {
    _selectedBytes = bytes;
    _detailCover.image =
      span.empty() ? nil : [[NSImage alloc] initWithData:[NSData dataWithBytes:span.data() length:span.size()]];
    _detailCover.accessibilityValue =
      _detailCover.image == nil ? [self text:MessageId::CoverArtNone] : [self text:MessageId::AppKitArtworkAvailable];
  }
}

- (void)layoutForModern:(BOOL)modern
{
  if (_detached != NO)
  {
    return;
  }

  _modern = modern;

  for (auto const& button : std::array<NSButton*, 2>{_properties, _reveal})
  {
    button.bezelStyle = NSBezelStyleRounded;
    button.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  }

  auto const inspectorWidth = self.view.bounds.size.width;
  auto const inspectorHeight = self.view.bounds.size.height;
  auto const inspectorTop = (_modern != NO ? self.view.safeAreaInsets.top : 0) + kContentInset;
  auto const artSize = (_selectionCount == 0) ? 0.0 : std::min(inspectorWidth - 32, 176.0);
  setFrame(_detailCover, (inspectorWidth - artSize) / 2, inspectorTop, artSize, artSize);
  setFrame(_detailScroll,
           kContentGap,
           inspectorTop + artSize + kContentInset,
           inspectorWidth - (2 * kContentGap),
           inspectorHeight - inspectorTop - artSize - kInspectorTextVerticalInsets - _properties.fittingSize.height -
             _reveal.fittingSize.height);
  _detailText.frame = NSMakeRect(
    0,
    0,
    inspectorWidth - (2 * kContentGap),
    std::max(kMinimumInspectorTextHeight, inspectorHeight - artSize - kInspectorTextVerticalInsets - kControlHeight));
  _detailText.textContainer.containerSize = NSMakeSize(inspectorWidth - 32, CGFLOAT_MAX);
  _detailText.textContainer.widthTracksTextView = YES;
  auto const revealHeight = _reveal.fittingSize.height;
  auto const propertiesHeight = _properties.fittingSize.height;
  auto const revealTop = inspectorHeight - kContentInset - revealHeight;
  setFrame(_reveal, kContentInset, revealTop, inspectorWidth - 32, revealHeight);
  setFrame(
    _properties, kContentInset, revealTop - kContentGap - propertiesHeight, inspectorWidth - 32, propertiesHeight);
}

- (void)presentSheetForWindow:(NSWindow*)window
{
  if (_detached != NO || _sheet != nil || window.attachedSheet != nil)
  {
    return;
  }

  auto* const sheet =
    [[AobusInspectorPanel alloc] initWithContentRect:NSMakeRect(0, 0, kInspectorSheetWidth, kInspectorSheetHeight)
                                           styleMask:NSWindowStyleMaskTitled
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
  __weak AobusTrackInspector* weakSelf = self;
  sheet.dismissHandler = ^{
    if (auto* const owner = weakSelf; owner != nil)
    {
      [owner dismissSheet];
    }
  };
  _sheet = sheet;
  _sheet.title = [self text:MessageId::AppKitTrackInformation];
  _sheet.preventsApplicationTerminationWhenModal = NO;
  _sheet.contentView =
    [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, kInspectorSheetWidth, kInspectorSheetHeight)];
  auto* const scroll =
    [[NSScrollView alloc] initWithFrame:NSMakeRect(kOuterInset,
                                                   60,
                                                   kInspectorSheetWidth - (2 * kOuterInset),
                                                   kInspectorSheetHeight - kInspectorSheetFooterHeight)];
  scroll.hasVerticalScroller = YES;
  scroll.drawsBackground = NO;
  scroll.accessibilityIdentifier = @"compact-inspector-scroll";
  auto* const text = [[NSTextView alloc] initWithFrame:scroll.contentView.bounds];
  text.editable = NO;
  text.verticallyResizable = YES;
  text.horizontallyResizable = NO;
  text.autoresizingMask = NSViewWidthSizable;
  text.textContainer.widthTracksTextView = YES;
  [text.textStorage setAttributedString:_detailText.attributedString];
  text.drawsBackground = NO;
  scroll.documentView = text;
  [_sheet.contentView addSubview:scroll];
  auto* const done = [NSButton buttonWithTitle:[self text:MessageId::AppKitDone]
                                        target:self
                                        action:@selector(dismiss:)];
  done.frame = NSMakeRect(kSheetDoneLeading, kContentInset, kInspectorDoneButtonWidth, kInspectorSheetButtonHeight);
  done.bezelStyle = NSBezelStyleRounded;
  done.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  done.keyEquivalent = @"\r";
  done.keyEquivalentModifierMask = 0;
  [_sheet.contentView addSubview:done];
  _sheet.defaultButtonCell = static_cast<NSButtonCell*>(done.cell);
  auto* const edit = [NSButton buttonWithTitle:[self text:MessageId::AppKitEditPropertiesAction]
                                        target:self
                                        action:@selector(editProperties:)];
  edit.frame = NSMakeRect(kOuterInset, kContentInset, kInspectorEditButtonWidth, kInspectorSheetButtonHeight);
  edit.bezelStyle = NSBezelStyleRounded;
  edit.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  edit.enabled = _properties.enabled;
  [_sheet.contentView addSubview:edit];
  [window beginSheet:_sheet completionHandler:nil];
}

- (void)dismissSheet
{
  if (_sheet == nil)
  {
    return;
  }

  auto* const sheet = _sheet;
  auto* const panel = static_cast<AobusInspectorPanel*>(sheet);
  _sheet = nil;
  panel.dismissHandler = nil;
  [sheet.sheetParent endSheet:sheet];

  if (_dismissHandler != nil)
  {
    _dismissHandler();
  }
}

- (void)reveal:(id) [[maybe_unused]] sender
{
  if (_detached == NO && _revealHandler != nil)
  {
    _revealHandler();
  }
}

- (void)editProperties:(id) [[maybe_unused]] sender
{
  if (_detached != NO)
  {
    return;
  }

  [self dismissSheet];

  if (_propertiesHandler != nil)
  {
    _propertiesHandler();
  }
}

- (void)dismiss:(id) [[maybe_unused]] sender
{
  [self dismissSheet];
}

- (void)detach
{
  _detached = YES;
  _revealHandler = nil;
  _propertiesHandler = nil;
  _dismissHandler = nil;
  _reveal.target = nil;
  _properties.target = nil;
  [self dismissSheet];
  _selectedBytes = {};
}
@end
