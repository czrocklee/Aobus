// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ActivityPopover.h"

#include "AppKitText.h"
#include "DesktopControls.h"
#include <ao/Contract.h>
#include <ao/rt/NotificationState.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace
{
  constexpr auto kContentInset = 16;
  constexpr auto kBodyFontSize = 13;
  constexpr auto kSmallButtonWidth = 26;
  constexpr auto kActivityTextWidth = 320;
  constexpr auto kActivityMinimumHeight = 72.0;
  constexpr auto kScrollAnchorTolerance = 1.0;

  using ao::appkit::nativeText;
  using ao::appkit::symbolButton;
  using ao::appkit::utf8;
  using ao::appkit::viewController;
  using ao::i18n::MessageId;

  bool hasSameCompactStructure(ao::uimodel::ActivityCompactState const& lhs,
                               ao::uimodel::ActivityCompactState const& rhs)
  {
    return lhs.kind == rhs.kind && lhs.dismissible == rhs.dismissible && lhs.hasDetails == rhs.hasDetails &&
           lhs.optAutoDismissTimeout == rhs.optAutoDismissTimeout;
  }

  bool hasSameItems(std::vector<ao::uimodel::ActivityDetailItem> const& lhs,
                    std::vector<ao::uimodel::ActivityDetailItem> const& rhs)
  {
    return lhs.size() == rhs.size() &&
           std::ranges::equal(lhs,
                              rhs,
                              [](auto const& left, auto const& right)
                              {
                                return left.id == right.id && left.severity == right.severity &&
                                       left.message == right.message && left.dismissible == right.dismissible;
                              });
  }

  bool hasSameTask(std::optional<ao::uimodel::ActivityTaskDetail> const& lhs,
                   std::optional<ao::uimodel::ActivityTaskDetail> const& rhs)
  {
    return lhs.has_value() == rhs.has_value() &&
           (!lhs || (lhs->message == rhs->message && lhs->progressFraction == rhs->progressFraction));
  }
} // namespace

@interface AobusActivityPopover () {
  std::optional<ao::i18n::MessageCatalog> _optCatalog;
  AobusActivityDismissHandler _dismissHandler;
  AobusActivityHideNotificationHandler _hideNotificationHandler;
  NSScrollView* _scroll;
  NSStackView* _stack;
  NSTextField* _compactText;
  NSTextField* _taskText;
  NSProgressIndicator* _taskProgress;
  NSMutableDictionary<NSString*, NSNumber*>* _notificationIds;
  ao::uimodel::ActivityStatusViewState _renderedState;
  BOOL _hasRenderedState;
}
- (void)dismissActivity:(id)sender;
- (void)hideNotification:(NSButton*)sender;
@end

@implementation AobusActivityPopover
- (instancetype)initWithCatalog:(ao::i18n::MessageCatalog const&)catalog
                 dismissHandler:(AobusActivityDismissHandler)dismissHandler
        hideNotificationHandler:(AobusActivityHideNotificationHandler)hideNotificationHandler
{
  self = [super init];

  if (self != nil)
  {
    _optCatalog.emplace(catalog);
    _dismissHandler = [dismissHandler copy];
    _hideNotificationHandler = [hideNotificationHandler copy];
    _stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = 8;
    _stack.edgeInsets = NSEdgeInsetsMake(kContentInset, kContentInset, kContentInset, kContentInset);
    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.drawsBackground = NO;
    _scroll.borderType = NSNoBorder;
    _scroll.hasHorizontalScroller = NO;
    _scroll.hasVerticalScroller = YES;
    _scroll.autohidesScrollers = YES;
    _scroll.scrollerStyle = NSScrollerStyleOverlay;
    _scroll.documentView = _stack;
    _notificationIds = [[NSMutableDictionary alloc] init];
    self.behavior = NSPopoverBehaviorTransient;
    self.contentViewController = viewController(_scroll);
  }

  return self;
}

- (void)render:(ao::uimodel::ActivityStatusViewState const&)state maximumHeight:(CGFloat)maximumHeight
{
  auto layout = [&]
  {
    auto const previousOrigin = _scroll.contentView.bounds.origin;
    auto const previousContentHeight = _stack.frame.size.height;
    auto const previousViewportHeight = _scroll.contentView.bounds.size.height;
    auto const previousMaximumScroll = std::max(0.0, previousContentHeight - previousViewportHeight);
    auto const wasAtTop =
      _hasRenderedState != NO && std::abs(previousOrigin.y - previousMaximumScroll) <= kScrollAnchorTolerance;
    auto const width = kActivityTextWidth + (2 * kContentInset);
    auto const contentHeight = std::max(kActivityMinimumHeight, _stack.fittingSize.height);
    auto const viewportHeight = std::min(contentHeight, std::max(kActivityMinimumHeight, maximumHeight));
    _stack.frame = NSMakeRect(0, 0, width, contentHeight);
    self.contentSize = NSMakeSize(width, viewportHeight);
    [_stack layoutSubtreeIfNeeded];
    auto const maximumScroll = std::max(0.0, contentHeight - viewportHeight);
    // NSStackView has a non-flipped document origin; keep a top viewport at the
    // new top while an intentional history position remains content-anchored.
    auto const scrollY =
      _hasRenderedState == 0 || wasAtTop ? maximumScroll : std::clamp(previousOrigin.y, 0.0, maximumScroll);
    [_scroll.contentView scrollToPoint:NSMakePoint(0, scrollY)];
    [_scroll reflectScrolledClipView:_scroll.contentView];
  };

  if (_hasRenderedState != NO && hasSameCompactStructure(_renderedState.compact, state.compact) &&
      hasSameItems(_renderedState.detail.items, state.detail.items) &&
      _renderedState.detail.optLibraryTask.has_value() == state.detail.optLibraryTask.has_value())
  {
    if (_renderedState.compact.text == state.compact.text &&
        _renderedState.compact.optProgressFraction == state.compact.optProgressFraction &&
        hasSameTask(_renderedState.detail.optLibraryTask, state.detail.optLibraryTask))
    {
      return;
    }

    AO_INVARIANT(_compactText != nil);
    _compactText.stringValue = state.compact.text.empty()
                                 ? ao::appkit::catalogText(*_optCatalog, MessageId::AppKitNoActivity)
                                 : nativeText(state.compact.text);

    if (state.detail.optLibraryTask)
    {
      AO_INVARIANT(_taskText != nil && _taskProgress != nil);
      _taskText.stringValue = nativeText(state.detail.optLibraryTask->message);
      _taskProgress.doubleValue = state.detail.optLibraryTask->progressFraction;
    }

    layout();
    _renderedState = state;
    return;
  }

  NSArray<NSView*>* const arrangedSubviews = _stack.arrangedSubviews.copy;

  for (NSUInteger index = 0; index < arrangedSubviews.count; ++index)
  {
    [_stack removeArrangedSubview:arrangedSubviews[index]];
    [arrangedSubviews[index] removeFromSuperview];
  }

  _taskText = nil;
  _taskProgress = nil;
  [_notificationIds removeAllObjects];
  auto addMessage = [&](NSString* message, NSColor* color, NSFontWeight weight, NSString* identifier)
  {
    auto* const text = [NSTextField wrappingLabelWithString:message];
    text.identifier = identifier;
    text.font = [NSFont systemFontOfSize:kBodyFontSize weight:weight];
    text.textColor = color;
    [text.widthAnchor constraintEqualToConstant:kActivityTextWidth].active = YES;
    [_stack addView:text inGravity:NSStackViewGravityLeading];
    return text;
  };
  auto* const compactRow = [NSStackView stackViewWithViews:@[]];
  compactRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  compactRow.alignment = NSLayoutAttributeCenterY;
  compactRow.spacing = 8;
  _compactText =
    [NSTextField wrappingLabelWithString:state.compact.text.empty()
                                           ? ao::appkit::catalogText(*_optCatalog, MessageId::AppKitNoActivity)
                                           : nativeText(state.compact.text)];
  _compactText.identifier = @"activity-compact";
  _compactText.font = [NSFont systemFontOfSize:kBodyFontSize weight:NSFontWeightSemibold];
  [_compactText.widthAnchor
    constraintEqualToConstant:kActivityTextWidth - (state.compact.dismissible ? kSmallButtonWidth + 8 : 0)]
    .active = YES;
  [compactRow addView:_compactText inGravity:NSStackViewGravityLeading];

  if (state.compact.dismissible)
  {
    auto* const dismiss = symbolButton(@"xmark.circle",
                                       ao::appkit::catalogText(*_optCatalog, MessageId::AppKitDismissActivity),
                                       self,
                                       @selector(dismissActivity:));
    dismiss.identifier = @"activity-dismiss";
    [dismiss.widthAnchor constraintEqualToConstant:kSmallButtonWidth].active = YES;
    [compactRow addView:dismiss inGravity:NSStackViewGravityTrailing];
  }

  [_stack addView:compactRow inGravity:NSStackViewGravityLeading];

  if (state.detail.optLibraryTask)
  {
    _taskText = addMessage(nativeText(state.detail.optLibraryTask->message),
                           NSColor.secondaryLabelColor,
                           NSFontWeightRegular,
                           @"activity-task-message");
    _taskProgress = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    _taskProgress.identifier = @"activity-task-progress";
    _taskProgress.indeterminate = NO;
    _taskProgress.minValue = 0;
    _taskProgress.maxValue = 1;
    _taskProgress.doubleValue = state.detail.optLibraryTask->progressFraction;
    [_taskProgress.widthAnchor constraintEqualToConstant:kActivityTextWidth].active = YES;
    [_stack addView:_taskProgress inGravity:NSStackViewGravityLeading];
  }

  for (auto const& detail : state.detail.items)
  {
    auto* color = NSColor.secondaryLabelColor;
    NSString* severityTitle = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitSeverityInfo);
    NSString* severitySymbol = @"info.circle";

    if (detail.severity == ao::rt::NotificationSeverity::Error)
    {
      color = NSColor.systemRedColor;
      severityTitle = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitSeverityError);
      severitySymbol = @"xmark.octagon";
    }
    else if (detail.severity == ao::rt::NotificationSeverity::Warning)
    {
      color = NSColor.systemOrangeColor;
      severityTitle = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitSeverityWarning);
      severitySymbol = @"exclamationmark.triangle";
    }

    auto* const row = [NSStackView stackViewWithViews:@[]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    row.spacing = 8;
    auto* const severity = [[NSImageView alloc] initWithFrame:NSZeroRect];
    severity.image = [NSImage imageWithSystemSymbolName:severitySymbol accessibilityDescription:severityTitle];
    severity.contentTintColor = color;
    severity.accessibilityLabel = severityTitle;
    [severity.widthAnchor constraintEqualToConstant:kContentInset].active = YES;
    [severity.heightAnchor constraintEqualToAnchor:severity.widthAnchor].active = YES;
    [row addView:severity inGravity:NSStackViewGravityLeading];
    auto* const identifier = nativeText(std::format("activity-notification-{}", detail.id.raw()));
    auto* const text = [NSTextField wrappingLabelWithString:nativeText(detail.message)];
    text.identifier = identifier;
    text.font = [NSFont systemFontOfSize:kBodyFontSize weight:NSFontWeightRegular];
    text.textColor = color;
    text.accessibilityLabel =
      ao::appkit::catalogFormat(*_optCatalog,
                                MessageId::AppKitActivityDetailLabel,
                                {{"severity", utf8(severityTitle)}, {"message", detail.message}});
    [text.widthAnchor constraintEqualToConstant:kActivityTextWidth - kContentInset - 8 -
                                                (detail.dismissible ? kSmallButtonWidth + 8 : 0)]
      .active = YES;
    [row addView:text inGravity:NSStackViewGravityLeading];

    if (detail.dismissible)
    {
      auto* const dismiss = symbolButton(@"xmark.circle",
                                         ao::appkit::catalogText(*_optCatalog, MessageId::AppKitHideNotification),
                                         self,
                                         @selector(hideNotification:));
      auto* const dismissIdentifier = [identifier stringByAppendingString:@"-dismiss"];
      AO_INVARIANT(dismissIdentifier != nil);
      dismiss.identifier = dismissIdentifier;
      _notificationIds[dismissIdentifier] = @(detail.id.raw());
      [dismiss.widthAnchor constraintEqualToConstant:kSmallButtonWidth].active = YES;
      [row addView:dismiss inGravity:NSStackViewGravityTrailing];
    }

    [_stack addView:row inGravity:NSStackViewGravityLeading];
  }

  layout();
  _renderedState = state;
  _hasRenderedState = YES;
}

- (void)dismissActivity:(id) [[maybe_unused]] sender
{
  if (_dismissHandler != nil)
  {
    _dismissHandler();
  }
}

- (void)hideNotification:(NSButton*)sender
{
  if (auto* const key = sender.identifier; key != nil)
  {
    // An update can remove a notification while its old button is tracking a
    // click.
    if (NSNumber* const identifier = _notificationIds[key]; identifier != nil && _hideNotificationHandler != nil)
    {
      _hideNotificationHandler(ao::rt::NotificationId{identifier.unsignedLongLongValue});
    }
  }
}
@end
