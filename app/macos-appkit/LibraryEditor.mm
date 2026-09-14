// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LibraryEditor.h"

#include "AppKitText.h"
#include "ArtworkView.h"
#include <ao/Contract.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <algorithm>
#include <exception>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace
{
  constexpr auto kPropertiesHeight = 760;
  constexpr auto kListHeight = 470;
  constexpr auto kDeletionHeight = 420;
  constexpr auto kMinimumHeight = 500;
  constexpr auto kMinimumWidth = 580;
  constexpr auto kInset = 24;
  constexpr auto kCoverSize = 64;
  constexpr auto kTitleSize = 20;
  constexpr auto kBodySize = 13;
  constexpr auto kCaptionSize = 11;
  constexpr auto kContentGap = 12;
  constexpr auto kGroupGap = 16;
  constexpr auto kRowInset = 16;
  constexpr auto kSectionRowGap = 6;
  constexpr auto kSectionBottomInset = 10;
  constexpr auto kFieldColumnGap = 10;
  constexpr auto kLabelWidth = 124;
  constexpr auto kActionWidth = 28;
  constexpr auto kCornerRadius = 12;

  using ao::appkit::nativeText;
  using ao::appkit::utf8;
  using ao::i18n::MessageId;
  using ao::rt::TrackField;

  template<typename Callback>
  void nativeCallback(Callback&& callback) noexcept
  {
    try
    {
      std::forward<Callback>(callback)();
    }
    catch (...)
    {
      AO_FATAL_EXCEPTION(std::current_exception(), "AppKit library editor callback");
    }
  }

  NSTextField* textLabel(NSString* text, CGFloat size, BOOL secondary = NO)
  {
    auto* const label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:size];
    label.textColor = secondary != NO ? NSColor.secondaryLabelColor : NSColor.labelColor;
    return label;
  }

  NSUInteger propertySection(TrackField field)
  {
    switch (field)
    {
      case TrackField::Title:
      case TrackField::Artist:
      case TrackField::Album:
      case TrackField::AlbumArtist: return 0;
      case TrackField::Genre:
      case TrackField::Year: return 1;
      case TrackField::DiscNumber:
      case TrackField::DiscTotal:
      case TrackField::TrackNumber:
      case TrackField::TrackTotal: return 2;
      case TrackField::Composer:
      case TrackField::Conductor:
      case TrackField::Ensemble:
      case TrackField::Work:
      case TrackField::Movement:
      case TrackField::Soloist:
      case TrackField::MovementNumber:
      case TrackField::MovementTotal: return 3;
      default: return 4;
    }
  }
} // namespace

@interface AobusEditorSurface : NSView
@property (nonatomic) BOOL grouped;
@property (nonatomic) BOOL backdrop;
@end

@implementation AobusEditorSurface
- (BOOL)isFlipped
{
  return YES;
}
- (void)drawRect:(NSRect) [[maybe_unused]] dirtyRect
{
  if (self.grouped != NO)
  {
    [NSColor.controlBackgroundColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:self.bounds xRadius:kCornerRadius yRadius:kCornerRadius] fill];
  }
  else if (self.backdrop != NO)
  {
    [NSColor.windowBackgroundColor setFill];
    ::NSRectFill(self.bounds);
  }
}
@end

@interface AobusLibraryEditor ()
- (NSString*)text:(MessageId)message;
- (void)buildProperties;
- (void)buildList;
- (void)layoutForm;
- (void)addSection:(NSString*)title expanded:(BOOL)expanded;
- (void)addField:(NSTextField*)field label:(NSString*)label section:(NSUInteger)section editable:(BOOL)editable;
@end

@implementation AobusLibraryEditor {
  ao::appkit::LibraryEditorModel* _model;
  __weak NSWindow* _parent;
  NSPanel* _panel;
  AobusEditorSurface* _content;
  NSScrollView* _scroll;
  AobusEditorSurface* _document;
  NSStackView* _documentStack;
  NSTextField* _message;
  NSTextField* _expressionError;
  NSButton* _save;
  NSButton* _cancel;
  NSMutableArray<NSTextField*>* _fields;
  NSMutableArray<NSButton*>* _fieldButtons;
  NSMutableArray<AobusEditorSurface*>* _groups;
  NSMutableArray<NSStackView*>* _groupStacks;
  NSMutableArray<NSButton*>* _headings;
  NSMutableArray<NSMutableArray<NSView*>*>* _rows;
  NSMutableArray<NSTextField*>* _errors;
  NSTokenField* _tags;
  NSImage* _artwork;
  BOOL _modern;
  BOOL _renderedPreview;
  BOOL _confirmingDiscard;
  NSAlert* _discardAlert;
  void (^_closeCompletion)(BOOL closed);
  BOOL _finishAfterConfirmation;
  BOOL _trackingFieldMenu;
  BOOL _renderedStale;
  std::string _displayedError;
}
- (instancetype)initWithModel:(ao::appkit::LibraryEditorModel&)model
                       parent:(NSWindow*)parent
                       modern:(BOOL)modern
                      artwork:(NSImage*)artwork
{
  self = [super init];

  if (self != nil)
  {
    _model = &model;
    _parent = parent;
    _modern = modern;
    _artwork = artwork;
  }

  return self;
}

- (NSString*)text:(MessageId)message
{
  return ao::appkit::catalogText(_model->catalog(), message);
}

- (NSWindow*)window
{
  return _panel;
}

- (void)present
{
  nativeCallback(
    [&]
    {
      auto const& state = _model->state();
      CGFloat height = kListHeight;

      if (state.kind == ao::appkit::LibraryEditorKind::Properties)
      {
        height = std::clamp(_parent.frame.size.height - kInset,
                            static_cast<CGFloat>(kMinimumHeight),
                            static_cast<CGFloat>(kPropertiesHeight));
      }
      else if (state.kind == ao::appkit::LibraryEditorKind::DeleteList)
      {
        height = kDeletionHeight;
      }

      auto const width = std::clamp(_parent.contentView.bounds.size.width * 0.68, 620.0, 760.0);
      _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                          styleMask:NSWindowStyleMaskTitled
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
      _panel.minSize = NSMakeSize(kMinimumWidth, kMinimumHeight);
      // Let the application delegate negotiate dirty and busy editor shutdown.
      _panel.preventsApplicationTerminationWhenModal = NO;
      _content = [[AobusEditorSurface alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
      _content.backdrop = YES;
      _panel.contentView = _content;
      _panel.autorecalculatesKeyViewLoop = YES;
      _panel.title = nativeText(state.title);
      _fields = [NSMutableArray array];
      _fieldButtons = [NSMutableArray array];
      _groups = [NSMutableArray array];
      _groupStacks = [NSMutableArray array];
      _headings = [NSMutableArray array];
      _rows = [NSMutableArray array];
      _errors = [NSMutableArray array];

      auto* const cover = [[AobusArtworkView alloc] initWithFrame:NSZeroRect];
      cover.imageScaling = NSImageScaleProportionallyUpOrDown;
      cover.image = _artwork;
      cover.accessibilityElement = YES;
      cover.accessibilityLabel = [self text:MessageId::CoverArtTitle];
      cover.accessibilityValue =
        [self text:cover.image == nil ? MessageId::CoverArtNone : MessageId::AppKitArtworkAvailable];
      cover.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:cover];
      auto* const title = textLabel(nativeText(state.title), kTitleSize);
      title.font = [NSFont systemFontOfSize:kTitleSize weight:NSFontWeightSemibold];
      title.lineBreakMode = NSLineBreakByTruncatingTail;
      title.maximumNumberOfLines = 1;
      title.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:title];
      NSString* summary = [self text:MessageId::AppKitCollectionHint];

      if (state.kind == ao::appkit::LibraryEditorKind::Properties)
      {
        title.stringValue = state.trackIds.size() == 1
                              ? [self text:MessageId::AppKitTrackDetails]
                              : ao::appkit::catalogFormat(
                                  _model->catalog(), MessageId::AppKitEditTracks, {{"count", state.trackIds.size()}});
        auto const album =
          std::ranges::find(state.fields, TrackField::Album, [](auto const& field) { return field.spec.field; });

        if (state.trackIds.size() == 1)
        {
          auto const track =
            std::ranges::find(state.fields, TrackField::Title, [](auto const& field) { return field.spec.field; });

          if (track != state.fields.end() && !track->text.empty())
          {
            title.stringValue = nativeText(track->text);
            title.toolTip = title.stringValue;
          }
        }

        summary = album != state.fields.end() && !album->mixed && !album->text.empty()
                    ? nativeText(album->text)
                    : [self text:MessageId::AppKitSelectedMusic];
      }
      else
      {
        auto* const symbol = [NSImage imageWithSystemSymbolName:@"music.note.list"
                                       accessibilityDescription:[self text:MessageId::AppKitSavedList]];
        cover.image = symbol;
        cover.imageScaling = NSImageScaleNone;

        if (state.kind == ao::appkit::LibraryEditorKind::DeleteList)
        {
          title.stringValue = [self text:MessageId::AppKitDeleteListQuestion];
          summary = [self text:MessageId::AppKitDeleteListHint];
        }
        else if (state.kind == ao::appkit::LibraryEditorKind::Membership)
        {
          summary = ao::appkit::catalogFormat(
            _model->catalog(), MessageId::AppKitUpdatingTracks, {{"count", state.trackIds.size()}});
        }
      }

      auto* const subtitle = textLabel(summary, kBodySize, YES);
      subtitle.maximumNumberOfLines = 2;
      subtitle.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:subtitle];
      _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
      _scroll.drawsBackground = NO;
      _scroll.hasVerticalScroller = YES;
      _scroll.autohidesScrollers = YES;
      _scroll.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:_scroll];
      _document = [[AobusEditorSurface alloc] initWithFrame:NSMakeRect(0, 0, width - (2 * kInset), 0)];
      _scroll.documentView = _document;
      _documentStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
      _documentStack.orientation = NSUserInterfaceLayoutOrientationVertical;
      _documentStack.alignment = NSLayoutAttributeLeading;
      _documentStack.spacing = kGroupGap;
      _documentStack.translatesAutoresizingMaskIntoConstraints = NO;
      [_document addSubview:_documentStack];
      [NSLayoutConstraint activateConstraints:@[
        [_documentStack.leadingAnchor constraintEqualToAnchor:_document.leadingAnchor],
        [_documentStack.trailingAnchor constraintEqualToAnchor:_document.trailingAnchor],
        [_documentStack.topAnchor constraintEqualToAnchor:_document.topAnchor]
      ]];

      if (state.kind == ao::appkit::LibraryEditorKind::Properties)
      {
        [self buildProperties];
      }
      else if (state.kind == ao::appkit::LibraryEditorKind::List)
      {
        [self buildList];
      }

      auto* const separator = [[NSBox alloc] initWithFrame:NSZeroRect];
      separator.boxType = NSBoxSeparator;
      separator.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:separator];
      _message = textLabel(@"", kCaptionSize, YES);
      _message.maximumNumberOfLines = 2;
      _message.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:_message];
      _cancel = [NSButton buttonWithTitle:[self text:MessageId::AppKitCancel] target:self action:@selector(cancel:)];
      _cancel.bezelStyle = NSBezelStyleRounded;
      _cancel.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
      _cancel.keyEquivalent = @"\033";
      _save = [NSButton buttonWithTitle:state.kind == ao::appkit::LibraryEditorKind::DeleteList
                                          ? [self text:MessageId::AppKitDeleteList]
                                          : [self text:MessageId::AppKitSaveChanges]
                                 target:self
                                 action:@selector(save:)];
      _save.bezelStyle = NSBezelStyleRounded;
      _save.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
      _save.hasDestructiveAction = static_cast<BOOL>(state.kind == ao::appkit::LibraryEditorKind::DeleteList);
      _save.keyEquivalent = _save.hasDestructiveAction != 0 ? @"" : @"\r";
      _save.identifier = @"save";
      auto* const buttons = [NSStackView stackViewWithViews:@[_cancel, _save]];
      buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
      buttons.alignment = NSLayoutAttributeCenterY;
      buttons.spacing = kContentGap;
      buttons.translatesAutoresizingMaskIntoConstraints = NO;
      [_content addSubview:buttons];
      [NSLayoutConstraint activateConstraints:@[
        [cover.leadingAnchor constraintEqualToAnchor:_content.leadingAnchor constant:kInset],
        [cover.topAnchor constraintEqualToAnchor:_content.topAnchor constant:kInset],
        [cover.widthAnchor constraintEqualToConstant:kCoverSize],
        [cover.heightAnchor constraintEqualToAnchor:cover.widthAnchor],
        [title.leadingAnchor constraintEqualToAnchor:cover.trailingAnchor constant:kContentGap],
        [title.trailingAnchor constraintEqualToAnchor:_content.trailingAnchor constant:-kInset],
        [title.topAnchor constraintEqualToAnchor:cover.topAnchor constant:4],
        [subtitle.leadingAnchor constraintEqualToAnchor:title.leadingAnchor],
        [subtitle.trailingAnchor constraintEqualToAnchor:title.trailingAnchor],
        [subtitle.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:4],
        [_scroll.leadingAnchor constraintEqualToAnchor:_content.leadingAnchor constant:kInset],
        [_scroll.trailingAnchor constraintEqualToAnchor:_content.trailingAnchor constant:-kInset],
        [_scroll.topAnchor constraintEqualToAnchor:cover.bottomAnchor constant:kContentGap],
        [_scroll.bottomAnchor constraintEqualToAnchor:separator.topAnchor constant:-kContentGap],
        [separator.leadingAnchor constraintEqualToAnchor:_content.leadingAnchor constant:kInset],
        [separator.trailingAnchor constraintEqualToAnchor:_content.trailingAnchor constant:-kInset],
        [separator.bottomAnchor constraintEqualToAnchor:_message.topAnchor constant:-8],
        [_message.leadingAnchor constraintEqualToAnchor:_content.leadingAnchor constant:kInset],
        [_message.trailingAnchor constraintEqualToAnchor:_content.trailingAnchor constant:-kInset],
        [_message.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-8],
        [buttons.trailingAnchor constraintEqualToAnchor:_content.trailingAnchor constant:-kInset],
        [buttons.bottomAnchor constraintEqualToAnchor:_content.bottomAnchor constant:-kInset]
      ]];
      [self refresh];
      [self layoutForm];
      [_parent beginSheet:_panel completionHandler:nil];

      if (_fields.count > 0)
      {
        [_panel makeFirstResponder:_fields.firstObject];
      }
    });
}

- (void)addSection:(NSString*)title expanded:(BOOL)expanded
{
  auto* const group = [[AobusEditorSurface alloc] initWithFrame:NSZeroRect];
  group.grouped = _modern;
  auto* const stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
  stack.orientation = NSUserInterfaceLayoutOrientationVertical;
  stack.alignment = NSLayoutAttributeLeading;
  stack.spacing = kSectionRowGap;
  stack.edgeInsets = NSEdgeInsetsMake(8, kRowInset, kSectionBottomInset, kRowInset);
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  [group addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [stack.leadingAnchor constraintEqualToAnchor:group.leadingAnchor],
    [stack.trailingAnchor constraintEqualToAnchor:group.trailingAnchor],
    [stack.topAnchor constraintEqualToAnchor:group.topAnchor],
    [stack.bottomAnchor constraintEqualToAnchor:group.bottomAnchor]
  ]];
  [_documentStack addView:group inGravity:NSStackViewGravityLeading];
  [group.widthAnchor constraintEqualToAnchor:_documentStack.widthAnchor].active = YES;
  [_groups addObject:group];
  [_groupStacks addObject:stack];
  auto* const heading = [NSButton buttonWithTitle:title target:self action:@selector(toggleSection:)];
  [heading setButtonType:NSButtonTypePushOnPushOff];
  heading.bordered = NO;
  heading.alignment = NSTextAlignmentLeft;
  heading.font = [NSFont systemFontOfSize:kBodySize weight:NSFontWeightSemibold];
  heading.imagePosition = NSImageLeft;
  heading.tag = static_cast<NSInteger>(_headings.count);
  heading.state = expanded != NO ? NSControlStateValueOn : NSControlStateValueOff;
  heading.identifier = nativeText(std::format("section-{}", _headings.count));
  heading.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  [stack addView:heading inGravity:NSStackViewGravityLeading];
  [_headings addObject:heading];
  [_rows addObject:[NSMutableArray array]];
}

- (void)addField:(NSTextField*)field label:(NSString*)label section:(NSUInteger)section editable:(BOOL)editable
{
  auto* const row = [[NSStackView alloc] initWithFrame:NSZeroRect];
  row.orientation = NSUserInterfaceLayoutOrientationVertical;
  row.alignment = NSLayoutAttributeLeading;
  row.spacing = 4;
  auto* const line = [[NSStackView alloc] initWithFrame:NSZeroRect];
  line.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  line.alignment = NSLayoutAttributeCenterY;
  line.distribution = NSStackViewDistributionFill;
  line.spacing = kFieldColumnGap;
  auto* const name = textLabel(label, kBodySize, YES);
  name.identifier = @"label";
  name.alignment = NSTextAlignmentRight;
  [name.widthAnchor constraintEqualToConstant:kLabelWidth].active = YES;
  [line addArrangedSubview:name];
  field.font = [NSFont systemFontOfSize:kBodySize];
  field.editable = editable;
  field.selectable = YES;
  field.bezeled = editable;
  field.bezelStyle = NSTextFieldRoundedBezel;
  field.drawsBackground = field.bezeled;
  field.focusRingType = NSFocusRingTypeExterior;
  field.accessibilityLabel = label;
  field.controlSize = _modern != NO ? NSControlSizeLarge : NSControlSizeRegular;
  [field setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                  forOrientation:NSLayoutConstraintOrientationHorizontal];
  [field setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  [line addArrangedSubview:field];
  [row addView:line inGravity:NSStackViewGravityLeading];
  [_groupStacks[section] addView:row inGravity:NSStackViewGravityLeading];
  [row.widthAnchor constraintEqualToAnchor:_groupStacks[section].widthAnchor constant:-2 * kRowInset].active = YES;
  [line.widthAnchor constraintEqualToAnchor:row.widthAnchor].active = YES;
  [_rows[section] addObject:row];

  if (_model->state().kind == ao::appkit::LibraryEditorKind::Properties && field != _tags)
  {
    auto* const error = textLabel(@"", kCaptionSize);
    error.textColor = NSColor.systemRedColor;
    error.identifier = @"field-error";
    error.hidden = YES;
    error.maximumNumberOfLines = 2;
    [row addView:error inGravity:NSStackViewGravityLeading];
    [error.widthAnchor constraintEqualToAnchor:row.widthAnchor].active = YES;
    [_errors addObject:error];

    if (editable != NO)
    {
      auto* const image = [NSImage imageWithSystemSymbolName:@"ellipsis"
                                    accessibilityDescription:[self text:MessageId::AppKitFieldActions]];
      auto* const action = image != nil ? [NSButton buttonWithImage:image target:self action:@selector(fieldActions:)]
                                        : [NSButton buttonWithTitle:@"…" target:self action:@selector(fieldActions:)];
      action.bordered = NO;
      action.tag = field.tag;
      action.toolTip =
        ao::appkit::catalogFormat(_model->catalog(), MessageId::AppKitFieldActionsFor, {{"field", utf8(label)}});
      action.accessibilityLabel = action.toolTip;
      action.identifier = @"field-actions";
      [action.widthAnchor constraintEqualToConstant:kActionWidth].active = YES;
      [line addArrangedSubview:action];
      [_fieldButtons addObject:action];
    }
  }
}

- (void)buildProperties
{
  [self addSection:[self text:MessageId::AppKitBasicInformation] expanded:YES];
  [self addSection:[self text:MessageId::AppKitClassificationTags] expanded:YES];
  [self addSection:[self text:MessageId::AppKitTrackDisc] expanded:static_cast<BOOL>(_modern == NO)];
  [self addSection:[self text:MessageId::AppKitWorkPerformance] expanded:static_cast<BOOL>(_modern == NO)];
  [self addSection:[self text:MessageId::AppKitFileInformation] expanded:NO];
  auto const& state = _model->state();

  for (std::size_t index = 0; index < state.fields.size(); ++index)
  {
    auto const& value = state.fields[index];
    auto* const field = [NSTextField textFieldWithString:nativeText(value.text)];
    field.tag = static_cast<NSInteger>(index);
    field.identifier = nativeText(std::string{ao::rt::trackFieldId(value.spec.field)});
    field.delegate = self;
    [_fields addObject:field];
    [self
      addField:field
         label:nativeText(value.spec.label)
       section:propertySection(value.spec.field)
      editable:static_cast<BOOL>(value.spec.editorKind != ao::uimodel::TrackPropertiesFormEditorKind::ReadonlyText)];
  }

  _tags = [[NSTokenField alloc] initWithFrame:NSZeroRect];
  auto* const tags = [NSMutableArray array];

  for (auto const& tag : state.tags)
  {
    [tags addObject:nativeText(tag)];
  }

  _tags.objectValue = tags;
  _tags.delegate = self;
  _tags.identifier = @"shared-tags";
  _tags.placeholderString = [self text:MessageId::AppKitAddSharedTags];
  [self addField:_tags label:[self text:MessageId::AppKitSharedTags] section:1 editable:YES];
}

- (void)buildList
{
  [self addSection:[self text:MessageId::AppKitListDetails] expanded:YES];
  auto const& list = _model->state().list;
  auto* const names = @[
    [self text:MessageId::AppKitName],
    [self text:MessageId::AppKitDescription],
    [self text:MessageId::AppKitExpression]
  ];
  auto* const identifiers = @[@"Name", @"Description", @"Expression"];
  auto* const placeholders =
    @[[self text:MessageId::AppKitListName], [self text:MessageId::AppKitOptionalDescription], @"#favorites"];
  auto* const values = @[nativeText(list.name), nativeText(list.description), nativeText(list.expression)];

  for (NSUInteger index = 0; index < names.count; ++index)
  {
    auto* const field = [NSTextField textFieldWithString:values[index]];
    field.identifier = identifiers[index];
    field.delegate = self;
    field.placeholderString = placeholders[index];
    [_fields addObject:field];
    [self addField:field label:names[index] section:0 editable:YES];

    if (index == 2)
    {
      _expressionError = textLabel(@"", kCaptionSize);
      _expressionError.textColor = NSColor.systemRedColor;
      _expressionError.identifier = @"field-error";
      _expressionError.hidden = YES;
      _expressionError.maximumNumberOfLines = 2;
      [static_cast<NSStackView*>(field.superview.superview) addView:_expressionError
                                                          inGravity:NSStackViewGravityLeading];
    }
  }

  auto* const hint = textLabel([self text:MessageId::AppKitListExpressionHint], kCaptionSize, YES);
  hint.identifier = @"hint";
  [_groupStacks[0] addView:hint inGravity:NSStackViewGravityLeading];
  [_rows[0] addObject:hint];
}

- (void)layoutForm
{
  for (NSUInteger section = 0; section < _groups.count; ++section)
  {
    auto* const heading = _headings[section];
    auto const expanded = heading.state == NSControlStateValueOn;
    heading.image = [NSImage imageWithSystemSymbolName:expanded ? @"chevron.down" : @"chevron.right"
                              accessibilityDescription:expanded ? [self text:MessageId::AppKitCollapseSection]
                                                                : [self text:MessageId::AppKitExpandSection]];

    for (NSUInteger rowIndex = 0; rowIndex < _rows[section].count; ++rowIndex)
    {
      NSView* const row = _rows[section][rowIndex];
      row.hidden = static_cast<BOOL>(!expanded);

      if ([row isKindOfClass:NSStackView.class] != NO)
      {
        if (auto* const stack = static_cast<NSStackView*>(row); stack.arrangedSubviews.count > 0)
        {
          stack.arrangedSubviews.firstObject.hidden = static_cast<BOOL>(!expanded);
        }
      }
    }
  }

  [_content layoutSubtreeIfNeeded];
  auto const width = std::max(1.0, _scroll.contentSize.width);
  _document.frame = NSMakeRect(0, 0, width, std::max(1.0, _documentStack.fittingSize.height));
  [_document layoutSubtreeIfNeeded];
  auto const contentHeight = _documentStack.fittingSize.height;
  _document.frame = NSMakeRect(0, 0, width, std::max(_scroll.contentSize.height, contentHeight));
  [_scroll tile];
  [_scroll reflectScrolledClipView:_scroll.contentView];
}

- (void)refresh
{
  auto const& state = _model->state();
  auto const restoreStale = state.stale && _renderedStale == NO;

  if (_discardAlert != nil)
  {
    _discardAlert.buttons[0].title = [self text:state.stale ? MessageId::AppKitKeepOpen : MessageId::AppKitKeepEditing];
  }

  if (restoreStale)
  {
    _renderedStale = YES;
    // Keep the native token candidate when stale admission rejects its final commit.
    [_panel makeFirstResponder:_cancel];
  }

  _message.stringValue = state.optInvalidField || state.listExpressionError ? @"" : nativeText(state.error);
  _message.textColor = NSColor.systemRedColor;

  if (state.error.empty())
  {
    _message.stringValue =
      state.kind == ao::appkit::LibraryEditorKind::Properties ? [self text:MessageId::AppKitEditsStorageHint] : @"";
    _message.textColor = NSColor.secondaryLabelColor;
  }

  if (state.kind == ao::appkit::LibraryEditorKind::DeleteList && state.error.empty())
  {
    _message.stringValue = [self text:MessageId::AppKitDeletionKeepsMedia];
  }

  if (state.busy)
  {
    _message.stringValue = [self text:MessageId::AppKitSaving];
    _message.textColor = NSColor.secondaryLabelColor;
  }

  _message.toolTip = _message.stringValue;
  _save.enabled =
    static_cast<BOOL>(!state.busy && !state.stale && !state.completed &&
                      (state.dirty || state.kind == ao::appkit::LibraryEditorKind::List || state.optDeletion));
  _save.hidden = static_cast<BOOL>(state.kind == ao::appkit::LibraryEditorKind::Membership);
  _cancel.enabled = static_cast<BOOL>(!state.busy);
  _tags.enabled = static_cast<BOOL>(!state.busy);
  _tags.editable = static_cast<BOOL>(!state.busy && !state.stale);
  NSView* invalidRow = nil;
  NSTextField* invalidField = nil;

  for (NSUInteger index = 0; index < _fields.count; ++index)
  {
    auto* const field = _fields[index];
    field.enabled = static_cast<BOOL>(!state.busy);
    field.accessibilityHelp = nil;

    if (state.kind == ao::appkit::LibraryEditorKind::Properties)
    {
      auto const& value = state.fields.at(index);
      field.editable =
        static_cast<BOOL>(!state.busy && !state.stale &&
                          value.spec.editorKind != ao::uimodel::TrackPropertiesFormEditorKind::ReadonlyText);

      if (restoreStale)
      {
        field.stringValue = nativeText(value.text);
      }

      field.placeholderString = value.mixed && !value.changed ? [self text:MessageId::TrackMultipleValues] : @"—";

      if (value.changed && value.text.empty())
      {
        field.placeholderString = [self text:MessageId::AppKitWillClear];
      }

      auto const invalid = state.optInvalidField == value.spec.field;
      _errors[index].hidden = static_cast<BOOL>(!invalid);
      _errors[index].stringValue = invalid ? nativeText(state.error) : @"";
      field.textColor = invalid ? NSColor.systemRedColor : NSColor.labelColor;

      if (invalid)
      {
        invalidRow = field.superview.superview;
        invalidField = field;
        field.accessibilityHelp = nativeText(state.error);
        _headings[propertySection(value.spec.field)].state = NSControlStateValueOn;
      }
    }
  }

  if (state.kind == ao::appkit::LibraryEditorKind::List)
  {
    auto* const values =
      @[nativeText(state.list.name), nativeText(state.list.description), nativeText(state.list.expression)];

    for (NSUInteger index = 0; index < _fields.count; ++index)
    {
      _fields[index].editable = static_cast<BOOL>(!state.busy && !state.stale);

      if (restoreStale)
      {
        _fields[index].stringValue = values[index];
      }
    }

    _expressionError.hidden = static_cast<BOOL>(!state.listExpressionError);
    _expressionError.stringValue = state.listExpressionError ? nativeText(state.error) : @"";
    _expressionError.toolTip = _expressionError.stringValue;

    if (state.listExpressionError)
    {
      invalidRow = _fields[2].superview.superview;
      invalidField = _fields[2];
      invalidField.accessibilityHelp = nativeText(state.error);
      _headings[0].state = NSControlStateValueOn;
    }
  }

  for (NSUInteger buttonIndex = 0; buttonIndex < _fieldButtons.count; ++buttonIndex)
  {
    auto* const button = _fieldButtons[buttonIndex];
    button.enabled = static_cast<BOOL>(!state.busy && !state.stale);
  }

  if (state.error != _displayedError)
  {
    _displayedError = state.error;
    [self layoutForm];

    if (invalidRow != nil && invalidField != nil)
    {
      [invalidRow scrollRectToVisible:invalidRow.bounds];
      [_panel makeFirstResponder:invalidField];
      ::NSAccessibilityPostNotificationWithUserInfo(invalidField, NSAccessibilityAnnouncementRequestedNotification, @{
        NSAccessibilityAnnouncementKey: nativeText(state.error),
        NSAccessibilityPriorityKey: [NSNumber numberWithInteger:NSAccessibilityPriorityHigh]
      });
    }
  }

  if (state.optDeletion && _renderedPreview == NO)
  {
    _renderedPreview = YES;
    [self addSection:ao::appkit::catalogFormat(_model->catalog(),
                                               MessageId::AppKitDeletingLists,
                                               {{"count", state.optDeletion->deletedLists.size()}})
            expanded:YES];

    for (auto const& list : state.optDeletion->deletedLists)
    {
      auto* const field = [NSTextField labelWithString:nativeText(list.name)];
      [self addField:field label:[self text:MessageId::AppKitList] section:_groups.count - 1 editable:NO];
    }

    [self layoutForm];
  }

  if (_closeCompletion != nil && !state.busy && _confirmingDiscard == NO && _trackingFieldMenu == NO)
  {
    [self requestCloseWithCompletion:nil];
  }
}

- (void)controlTextDidChange:(NSNotification*)notification
{
  nativeCallback(
    [&]
    {
      if (notification.object == _tags)
      {
        // Record edit intent even while the uncommitted token remains native state.
        // Equal committed tags must still dirty the draft before close admission.
        _model->editTags(_model->state().tags);
      }
      else if (_model->state().kind == ao::appkit::LibraryEditorKind::Properties)
      {
        NSTextField* const field = notification.object;
        _model->editField(static_cast<std::size_t>(field.tag), utf8(field.stringValue));
      }
      else if (_model->state().kind == ao::appkit::LibraryEditorKind::List)
      {
        _model->editList(utf8(_fields[0].stringValue), utf8(_fields[1].stringValue), utf8(_fields[2].stringValue));
      }

      [self refresh];
    });
}

- (void)controlTextDidEndEditing:(NSNotification*)notification
{
  if (notification.object == _tags)
  {
    nativeCallback(
      [&]
      {
        NSArray* const values = _tags.objectValue;
        auto tags = std::vector<std::string>{};

        for (NSUInteger index = 0; index < values.count; ++index)
        {
          tags.push_back(utf8(values[index]));
        }

        _model->editTags(std::move(tags));
        [self refresh];
      });
  }
}

- (BOOL)requestClose
{
  [self requestCloseWithCompletion:nil];
  return static_cast<BOOL>(_panel == nil);
}

- (void)requestCloseWithCompletion:(void (^)(BOOL closed))completion
{
  if (_panel == nil)
  {
    if (completion != nil)
    {
      completion(YES);
    }

    return;
  }

  if (completion != nil)
  {
    AO_EXPECTS(_closeCompletion == nil, "Only one lifecycle close request may own the editor completion");
    _closeCompletion = [completion copy];
  }

  if (_model->state().busy || _confirmingDiscard != NO || _trackingFieldMenu != NO)
  {
    return;
  }

  if (_model->state().dirty)
  {
    auto* const alert = [[NSAlert alloc] init];
    alert.messageText = [self text:MessageId::AppKitDiscardQuestion];
    alert.informativeText = [self text:MessageId::AppKitDiscardHint];
    [alert
      addButtonWithTitle:[self text:_model->state().stale ? MessageId::AppKitKeepOpen : MessageId::AppKitKeepEditing]];
    [alert addButtonWithTitle:[self text:MessageId::AppKitDiscardChanges]];
    alert.buttons[1].hasDestructiveAction = YES;
    // Let the application join this close decision when Quit arrives later.
    alert.window.preventsApplicationTerminationWhenModal = NO;
    _confirmingDiscard = YES;
    _discardAlert = alert;
    // The sheet keeps its editor alive until the nested close transaction settles.
    // Its attached parent prevents the LibrarySession from being released first.
    [alert beginSheetModalForWindow:_panel
                  completionHandler:^(NSModalResponse response) {
                    nativeCallback(
                      [&]
                      {
                        auto* const owner = self;
                        auto const finishRequested = owner->_finishAfterConfirmation;
                        owner->_confirmingDiscard = NO;
                        owner->_discardAlert = nil;
                        owner->_finishAfterConfirmation = NO;

                        if (response == NSAlertSecondButtonReturn || finishRequested != NO)
                        {
                          AO_INVARIANT(owner->_panel != nil && !owner->_model->state().busy,
                                       "Discard confirmation must settle with an idle editor");
                          [owner finish];
                        }
                        else
                        {
                          auto const completed = owner->_closeCompletion;
                          owner->_closeCompletion = nil;

                          if (completed != nil)
                          {
                            completed(NO);
                          }
                        }
                      });
                  }];
    return;
  }

  [self finish];
}

- (void)finish
{
  if (_panel == nil)
  {
    return;
  }

  if (_confirmingDiscard != NO)
  {
    if (_finishAfterConfirmation == NO)
    {
      _finishAfterConfirmation = YES;
      [_panel endSheet:_discardAlert.window returnCode:NSAlertSecondButtonReturn];
    }

    return;
  }

  auto* const panel = _panel;
  _panel = nil;
  [_parent endSheet:panel];
  _model->cancel();
  auto const completed = _closeCompletion;
  _closeCompletion = nil;

  if (completed != nil)
  {
    completed(YES);
  }
}

- (void)clearField:(NSMenuItem*)sender
{
  nativeCallback(
    [&]
    {
      if (_model->state().busy || _model->state().stale || _panel == nil)
      {
        [self refresh];
        return;
      }

      auto* const field = _fields[static_cast<NSUInteger>(sender.tag)];
      field.stringValue = @"";
      _model->editField(static_cast<std::size_t>(sender.tag), "");
      [self refresh];
    });
}

- (void)fieldActions:(NSButton*)sender
{
  nativeCallback(
    [&]
    {
      if (_model->state().busy || _model->state().stale || _panel == nil)
      {
        [self refresh];
        return;
      }

      [_panel makeFirstResponder:sender];
      auto* const menu = [[NSMenu alloc] initWithTitle:[self text:MessageId::AppKitFieldActions]];
      auto* const item = [menu addItemWithTitle:[self text:MessageId::AppKitClearSelectedField]
                                         action:@selector(clearField:)
                                  keyEquivalent:@""];
      item.target = self;
      item.tag = sender.tag;
      _trackingFieldMenu = YES;
      [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, sender.bounds.size.height) inView:sender];
      _trackingFieldMenu = NO;
      [self refresh];
    });
}

- (void)toggleSection:(NSButton*)sender
{
  [_panel makeFirstResponder:sender];
  [self layoutForm];
}

- (void)save:(id) [[maybe_unused]] sender
{
  if (_panel == nil || _confirmingDiscard != NO)
  {
    return;
  }

  nativeCallback(
    [&]
    {
      [_panel makeFirstResponder:_save];
      _model->save();
      [self refresh];
    });
}

- (void)cancel:(id) [[maybe_unused]] sender
{
  std::ignore = [self requestClose];
}
@end
