// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PlaybackBar.h"

#include "AppKitText.h"
#include "ArtworkView.h"
#include "DesktopControls.h"
#include "LibrarySession.h"
#include "SoulButton.h"
#include <ao/uimodel/playback/command/PlaybackCommandText.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace
{
  constexpr auto kCellInset = 6;
  constexpr auto kContentGap = 12;
  constexpr auto kContentInset = 16;
  constexpr auto kCaptionFontSize = 10;
  constexpr auto kSecondaryFontSize = 11;
  constexpr auto kBodyFontSize = 13;
  constexpr auto kSmallButtonWidth = 26;
  constexpr auto kControlHeight = 28;
  constexpr auto kPlayerArtworkSize = 44;
  constexpr auto kTimeLabelWidth = 42;
  constexpr auto kModernPlayerHeight = 80.0;
  constexpr auto kPlayerProgressHeight = 16.0;
  constexpr auto kMinimumPlayerTitleWidth = 100.0;
  constexpr auto kVolumePopoverWidth = 180.0;
  constexpr auto kVolumePopoverHeight = 44.0;
  constexpr auto kVolumeIconTrailingOffset = 24;
  constexpr auto kClassicPlayerHeight = 48.0;
  constexpr auto kClassicTransportTop = 10;
  constexpr auto kClassicPlayLeading = 56;
  constexpr auto kClassicStopLeading = 92;
  constexpr auto kClassicSeekAndVolumeTop = 14;
  constexpr auto kClassicSeekLeading = 136;
  constexpr auto kClassicTimeWidth = 110;
  constexpr auto kClassicVolumeWidth = 108;
  constexpr auto kClassicVolumeTrailing = 124;

  using ao::appkit::nativeText;
  using ao::appkit::symbolButton;
  using ao::appkit::viewController;
  using ao::i18n::MessageId;

  NSTextField* label(NSString* text, CGFloat size, BOOL secondary = NO)
  {
    auto* const field = [NSTextField labelWithString:text];
    field.font = [NSFont systemFontOfSize:size];
    field.textColor = (secondary != NO) ? NSColor.secondaryLabelColor : NSColor.labelColor;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
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
} // namespace

@implementation AobusSeekSlider {
  std::optional<ao::appkit::PlaybackSeekTarget> _optPresentedTarget;
  std::optional<ao::appkit::PlaybackSeekTarget> _optGestureTarget;
}
- (void)presentSeekTarget:(std::optional<ao::appkit::PlaybackSeekTarget>)optTarget
{
  _optPresentedTarget = optTarget;
}
- (std::optional<ao::appkit::PlaybackSeekTarget>)seekTarget
{
  return [self isTrackingGesture] != NO ? _optGestureTarget : _optPresentedTarget;
}
- (void)mouseDown:(NSEvent*)event
{
  _optGestureTarget = _optPresentedTarget;
  [super mouseDown:event];
  _optGestureTarget.reset();
}
@end

@implementation AobusPlaybackBar {
  std::optional<ao::i18n::MessageCatalog> _optCatalog;
  __weak id _actionTarget;
  SEL _transportAction;
  SEL _showOutputAction;
  AobusSurface* _classicPlayer;
  AobusSurface* _modernPlayer;
  NSView* _activeView;
  AobusFlippedView* _modernPlayerControls;
  NSStackView* _modernTransport;
  NSStackView* _modernNowPlaying;
  NSArray<NSLayoutConstraint*>* _modernControlConstraints;
  NSArray<NSLayoutConstraint*>* _modernRootConstraints;
  __weak NSView* _root;
  NSPopover* _volumePopover;
  AobusFlippedView* _volumePopoverContent;
  NSButton* _volumeButton;
  NSButton* _playbackOptions;
  NSImageView* _volumeIcon;
  NSTextField* _title;
  NSTextField* _artist;
  NSTextField* _elapsed;
  NSTextField* _duration;
  AobusArtworkView* _cover;
  __weak NSImageView* _playingArtworkMirror;
  AobusSoulButton* _soul;
  NSButton* _play;
  NSButton* _stop;
  NSButton* _previous;
  NSButton* _next;
  NSButton* _shuffle;
  NSButton* _repeat;
  NSButton* _output;
  AobusSeekSlider* _seek;
  AobusSlider* _volume;
  ao::rt::ResourceBytes _playingBytes;
  BOOL _modern;
  BOOL _detached;
}

- (instancetype)initWithCatalog:(ao::i18n::MessageCatalog const&)catalog
                         target:(id)target
                transportAction:(SEL)transportAction
                     seekAction:(SEL)seekAction
                   volumeAction:(SEL)volumeAction
               showVolumeAction:(SEL)showVolumeAction
               showOutputAction:(SEL)showOutputAction
              showOptionsAction:(SEL)showOptionsAction
           playingArtworkMirror:(NSImageView*)playingArtworkMirror
{
  self = [super init];

  if (self != nil)
  {
    _optCatalog.emplace(catalog);
    _actionTarget = target;
    id const actionTarget = _actionTarget;
    _transportAction = transportAction;
    _showOutputAction = showOutputAction;
    _playingArtworkMirror = playingArtworkMirror;
    _classicPlayer = [[AobusSurface alloc] initWithFrame:NSZeroRect];
    _classicPlayer.identifier = @"playback.classic";
    _classicPlayer.material = NSVisualEffectMaterialWindowBackground;
    _modernPlayer = [[AobusSurface alloc] initWithFrame:NSZeroRect];
    _modernPlayer.identifier = @"playback.modern";
    _modernPlayer.material = NSVisualEffectMaterialWindowBackground;
    _modernPlayer.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    _modernPlayerControls = [[AobusFlippedView alloc] initWithFrame:NSZeroRect];
    _activeView = _modernPlayer;
    _modern = YES;

    _cover = [[AobusArtworkView alloc] initWithFrame:NSZeroRect];
    _cover.identifier = @"playback.cover";
    _cover.imageScaling = NSImageScaleProportionallyUpOrDown;
    _cover.accessibilityElement = YES;
    _cover.accessibilityLabel = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitPlayingArtwork);
    _cover.accessibilityValue = ao::appkit::catalogText(*_optCatalog, MessageId::CoverArtNone);
    _cover.image = nil;
    _title = label(@"Aobus", kBodyFontSize);
    _title.identifier = @"playback.title";
    _title.font = [NSFont systemFontOfSize:kBodyFontSize weight:NSFontWeightSemibold];
    _artist = label(ao::appkit::catalogText(*_optCatalog, MessageId::AppKitChooseMusic), kSecondaryFontSize, YES);
    _artist.identifier = @"playback.artist";
    _elapsed = label(@"0:00", kCaptionFontSize, YES);
    _elapsed.identifier = @"playback.elapsed";
    _duration = label(@"0:00", kCaptionFontSize, YES);
    _duration.identifier = @"playback.duration";
    _soul = [[AobusSoulButton alloc] initWithFrame:NSZeroRect];
    _soul.identifier = @"playback.soul";
    _soul.bordered = NO;
    _soul.target = actionTarget;
    _soul.action = transportAction;
    _soul.tag = static_cast<NSInteger>(ao::uimodel::PlaybackCommand::PlayPause);

    auto transportButton = [&](NSString* symbol, NSString* identifier, ao::uimodel::PlaybackCommand command)
    {
      auto* const button = symbolButton(
        symbol, nativeText(ao::uimodel::playbackActionLabel(*_optCatalog, command)), actionTarget, transportAction);
      button.identifier = identifier;
      button.tag = static_cast<NSInteger>(command);
      return button;
    };
    _play = transportButton(@"play.fill", @"playback.play", ao::uimodel::PlaybackCommand::PlayPause);
    _stop = transportButton(@"stop.fill", @"playback.stop", ao::uimodel::PlaybackCommand::Stop);
    _previous = transportButton(@"backward.end.fill", @"playback.previous", ao::uimodel::PlaybackCommand::Previous);
    _next = transportButton(@"forward.end.fill", @"playback.next", ao::uimodel::PlaybackCommand::Next);
    _shuffle = transportButton(@"shuffle", @"playback.shuffle", ao::uimodel::PlaybackCommand::ToggleShuffle);
    [_shuffle setButtonType:NSButtonTypeToggle];
    _repeat = transportButton(@"repeat", @"playback.repeat", ao::uimodel::PlaybackCommand::CycleRepeat);
    _output = symbolButton(@"hifispeaker",
                           ao::appkit::catalogText(*_optCatalog, MessageId::AppKitOutputDevice),
                           actionTarget,
                           showOutputAction);
    _output.identifier = @"playback.output";
    _volumeButton = symbolButton(@"speaker.wave.2.fill",
                                 ao::appkit::catalogText(*_optCatalog, MessageId::AppKitVolume),
                                 actionTarget,
                                 showVolumeAction);
    _volumeButton.identifier = @"playback.volume-button";
    _playbackOptions = symbolButton(@"ellipsis",
                                    ao::appkit::catalogText(*_optCatalog, MessageId::AppKitPlaybackOptions),
                                    actionTarget,
                                    showOptionsAction);
    _playbackOptions.identifier = @"playback.options";
    _seek = [AobusSeekSlider sliderWithValue:0 minValue:0 maxValue:1 target:actionTarget action:seekAction];
    _seek.identifier = @"playback.seek";
    _seek.controlSize = NSControlSizeSmall;
    _seek.continuous = NO;
    _seek.accessibilityLabel = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitPlaybackPosition);
    _volume = [AobusSlider sliderWithValue:1 minValue:0 maxValue:1 target:actionTarget action:volumeAction];
    _volume.identifier = @"playback.volume";
    _volume.controlSize = NSControlSizeSmall;
    _volume.accessibilityLabel = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitVolume);
    _volume.toolTip = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitVolume);
    _volumeIcon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _volumeIcon.image =
      [NSImage imageWithSystemSymbolName:@"speaker.wave.2.fill"
                accessibilityDescription:ao::appkit::catalogText(*_optCatalog, MessageId::AppKitVolume)];
    _volumeIcon.contentTintColor = NSColor.secondaryLabelColor;
    _volumePopoverContent =
      [[AobusFlippedView alloc] initWithFrame:NSMakeRect(0, 0, kVolumePopoverWidth, kVolumePopoverHeight)];
    _volumePopoverContent.identifier = @"playback.volume-popover";
    [_volumePopoverContent addSubview:_volumeIcon];
    [_volumePopoverContent addSubview:_volume];
    auto const volumeTop = (kVolumePopoverHeight - kContentInset) / 2;
    auto const volumeLeading = (2 * kContentGap) + kContentInset;
    setFrame(_volumeIcon, kContentGap, volumeTop, kContentInset, kContentInset);
    setFrame(_volume, volumeLeading, volumeTop, kVolumePopoverWidth - volumeLeading - kContentGap, kContentInset);
    _volumePopover = [[NSPopover alloc] init];
    _volumePopover.behavior = NSPopoverBehaviorTransient;
    _volumePopover.contentViewController = viewController(_volumePopoverContent);
    _volumePopover.contentSize = _volumePopoverContent.frame.size;

    auto* const titleStack = [NSStackView stackViewWithViews:@[_title, _artist]];
    titleStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    titleStack.alignment = NSLayoutAttributeLeading;
    titleStack.spacing = 1;
    [titleStack.widthAnchor constraintGreaterThanOrEqualToConstant:kMinimumPlayerTitleWidth].active = YES;
    _modernNowPlaying = [NSStackView stackViewWithViews:@[_cover, titleStack]];
    _modernNowPlaying.identifier = @"playback.now-playing";
    _modernNowPlaying.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _modernNowPlaying.alignment = NSLayoutAttributeCenterY;
    _modernNowPlaying.spacing = kContentGap;
    _modernNowPlaying.detachesHiddenViews = NO;
    [_modernNowPlaying setClippingResistancePriority:NSLayoutPriorityRequired
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    for (auto const& field : std::array<NSTextField*, 2>{_title, _artist})
    {
      [field setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    }

    [_cover.widthAnchor constraintEqualToConstant:kPlayerArtworkSize].active = YES;
    [_cover.heightAnchor constraintEqualToAnchor:_cover.widthAnchor].active = YES;
    _modernTransport = [NSStackView
      stackViewWithViews:@[_output, _elapsed, _shuffle, _previous, _soul, _next, _repeat, _duration, _volumeButton]];
    _modernTransport.identifier = @"playback.transport";
    _modernTransport.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _modernTransport.alignment = NSLayoutAttributeCenterY;
    _modernTransport.spacing = 8;
    [_modernTransport setClippingResistancePriority:NSLayoutPriorityRequired
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];

    for (auto const& button :
         std::array<NSButton*, 7>{_output, _shuffle, _previous, _next, _repeat, _volumeButton, _playbackOptions})
    {
      [button.widthAnchor constraintEqualToConstant:kSmallButtonWidth].active = YES;
      [button.heightAnchor constraintEqualToConstant:kControlHeight].active = YES;
    }

    for (auto const& field : std::array<NSTextField*, 2>{_elapsed, _duration})
    {
      field.font = [NSFont monospacedDigitSystemFontOfSize:kCaptionFontSize weight:NSFontWeightRegular];
      field.alignment = NSTextAlignmentCenter;
    }

    _modernControlConstraints = @[
      [_soul.widthAnchor constraintEqualToConstant:kClassicPlayerHeight],
      [_soul.heightAnchor constraintEqualToAnchor:_soul.widthAnchor],
      [_elapsed.widthAnchor constraintEqualToConstant:kTimeLabelWidth],
      [_duration.widthAnchor constraintEqualToAnchor:_elapsed.widthAnchor],
      [_seek.leadingAnchor constraintEqualToAnchor:_modernPlayer.leadingAnchor constant:kCellInset],
      [_seek.trailingAnchor constraintEqualToAnchor:_modernPlayer.trailingAnchor constant:-kCellInset],
      [_seek.topAnchor constraintEqualToAnchor:_modernPlayer.topAnchor],
      [_seek.heightAnchor constraintEqualToConstant:kPlayerProgressHeight]
    ];

    for (auto const& view :
         std::array<NSView*, 5>{_modernPlayerControls, _modernNowPlaying, _modernTransport, _playbackOptions, _seek})
    {
      view.translatesAutoresizingMaskIntoConstraints = NO;
    }

    [_modernPlayer addSubview:_modernPlayerControls];
    [_modernPlayer addSubview:_seek];
    [_modernPlayerControls addSubview:_modernNowPlaying];
    [_modernPlayerControls addSubview:_modernTransport];
    [_modernPlayerControls addSubview:_playbackOptions];
    [NSLayoutConstraint activateConstraints:@[
      [_modernPlayerControls.leadingAnchor constraintEqualToAnchor:_modernPlayer.leadingAnchor],
      [_modernPlayerControls.trailingAnchor constraintEqualToAnchor:_modernPlayer.trailingAnchor],
      [_modernPlayerControls.topAnchor constraintEqualToAnchor:_modernPlayer.topAnchor constant:kPlayerProgressHeight],
      [_modernPlayerControls.bottomAnchor constraintEqualToAnchor:_modernPlayer.bottomAnchor],
      [_modernTransport.centerXAnchor constraintEqualToAnchor:_modernPlayerControls.centerXAnchor],
      [_modernTransport.centerYAnchor constraintEqualToAnchor:_modernPlayerControls.centerYAnchor],
      [_modernNowPlaying.leadingAnchor constraintEqualToAnchor:_modernPlayerControls.leadingAnchor
                                                      constant:kContentInset],
      [_modernNowPlaying.trailingAnchor constraintLessThanOrEqualToAnchor:_modernTransport.leadingAnchor
                                                                 constant:-kContentGap],
      [_modernNowPlaying.centerYAnchor constraintEqualToAnchor:_modernPlayerControls.centerYAnchor],
      [_playbackOptions.trailingAnchor constraintEqualToAnchor:_modernPlayerControls.trailingAnchor
                                                      constant:-kContentInset],
      [_playbackOptions.leadingAnchor constraintGreaterThanOrEqualToAnchor:_modernTransport.trailingAnchor
                                                                  constant:kContentGap],
      [_playbackOptions.centerYAnchor constraintEqualToAnchor:_modernPlayerControls.centerYAnchor]
    ]];
  }

  return self;
}

- (NSView*)activeView
{
  return _activeView;
}

- (NSView*)modernView
{
  return _modernPlayer;
}

- (NSView*)classicView
{
  return _classicPlayer;
}

- (CGFloat)heightForModern:(BOOL)modern
{
  return (modern != NO) ? kModernPlayerHeight : kClassicPlayerHeight;
}

- (void)layoutInRoot:(NSView*)root modern:(BOOL)modern
{
  if (_detached != NO)
  {
    return;
  }

  _modern = modern;

  if (_root != root)
  {
    [NSLayoutConstraint deactivateConstraints:_modernRootConstraints];
    _root = root;
    _modernPlayer.translatesAutoresizingMaskIntoConstraints = NO;
    _modernRootConstraints = @[
      [_modernPlayer.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
      [_modernPlayer.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
      [_modernPlayer.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
      [_modernPlayer.heightAnchor constraintEqualToConstant:kModernPlayerHeight]
    ];
  }

  if (modern != NO)
  {
    if (_soul.superview != _modernTransport)
    {
      for (auto const& view : std::array<NSView*, 3>{_soul, _seek, _elapsed})
      {
        detachView(view);
        view.translatesAutoresizingMaskIntoConstraints = NO;
      }

      [_modernTransport insertArrangedSubview:_elapsed atIndex:1];
      [_modernTransport insertArrangedSubview:_soul atIndex:4];
      [_modernPlayer addSubview:_seek];
    }

    if (_volume.superview != _volumePopoverContent)
    {
      for (auto const& view : std::array<NSView*, 2>{_volume, _volumeIcon})
      {
        detachView(view);
        view.translatesAutoresizingMaskIntoConstraints = YES;
        [_volumePopoverContent addSubview:view];
      }

      auto const volumeTop = (kVolumePopoverHeight - kContentInset) / 2;
      auto const volumeLeading = (2 * kContentGap) + kContentInset;
      setFrame(_volumeIcon, kContentGap, volumeTop, kContentInset, kContentInset);
      setFrame(_volume, volumeLeading, volumeTop, kVolumePopoverWidth - volumeLeading - kContentGap, kContentInset);
    }

    [_classicPlayer removeFromSuperview];
    _activeView = _modernPlayer;

    if (_modernPlayer.superview != root)
    {
      [root addSubview:_modernPlayer];
    }

    [NSLayoutConstraint activateConstraints:_modernControlConstraints];
    [NSLayoutConstraint activateConstraints:_modernRootConstraints];
  }
  else
  {
    [NSLayoutConstraint deactivateConstraints:_modernRootConstraints];
    [NSLayoutConstraint deactivateConstraints:_modernControlConstraints];
    [self closePopover];

    if (_soul.superview != _classicPlayer)
    {
      for (auto const& view : std::array<NSView*, 5>{_soul, _seek, _elapsed, _volume, _volumeIcon})
      {
        detachView(view);
        [_classicPlayer addSubview:view];
        view.translatesAutoresizingMaskIntoConstraints = YES;
      }
    }

    if (_play.superview != _classicPlayer)
    {
      for (auto const& view : std::array<NSView*, 2>{_play, _stop})
      {
        detachView(view);
        [_classicPlayer addSubview:view];
        view.translatesAutoresizingMaskIntoConstraints = YES;
      }
    }

    [_modernPlayer removeFromSuperview];
    _activeView = _classicPlayer;

    if (_classicPlayer.superview != root)
    {
      [root addSubview:_classicPlayer];
    }
  }

  _soul.action = (modern != NO) ? _transportAction : _showOutputAction;

  if (modern == NO)
  {
    auto* const outputDescription = ao::appkit::catalogText(*_optCatalog, MessageId::AppKitOutputDevice);

    if ([_soul.accessibilityLabel isEqualToString:outputDescription] == NO)
    {
      _soul.accessibilityLabel = outputDescription;
    }

    _soul.toolTip = outputDescription;
  }

  if (modern == NO)
  {
    auto const width = root.bounds.size.width;
    auto const classicTop = root.safeAreaInsets.top;
    auto const volumeLeft = width - kClassicVolumeTrailing;
    auto const volumeIconLeft = volumeLeft - kVolumeIconTrailingOffset;
    auto const timeLeft = volumeIconLeft - kContentGap - kClassicTimeWidth;
    auto const seekWidth = timeLeft - kContentGap - kClassicSeekLeading;
    setFrame(_classicPlayer, 0, classicTop, width, kClassicPlayerHeight);
    setFrame(_soul, kContentGap, 8, 32, 32);
    setFrame(_play, kClassicPlayLeading, kClassicTransportTop, _play.fittingSize.width, _play.fittingSize.height);
    setFrame(_stop, kClassicStopLeading, kClassicTransportTop, _stop.fittingSize.width, _stop.fittingSize.height);
    setFrame(_seek, kClassicSeekLeading, kClassicSeekAndVolumeTop, seekWidth, _seek.fittingSize.height);
    setFrame(_elapsed, timeLeft, kContentInset, kClassicTimeWidth, _elapsed.fittingSize.height);
    setFrame(_volume, volumeLeft, kClassicSeekAndVolumeTop, kClassicVolumeWidth, _volume.fittingSize.height);
    setFrame(_volumeIcon,
             _volume.frame.origin.x - kVolumeIconTrailingOffset,
             _volume.frame.origin.y,
             kContentInset,
             _volume.fittingSize.height);
  }
}

- (void)renderState:(ao::appkit::DesktopViewState const&)state modern:(BOOL)modern
{
  if (_detached != NO)
  {
    return;
  }

  auto const& play = state.transport[static_cast<std::size_t>(ao::uimodel::PlaybackCommand::PlayPause)];
  _title.stringValue = nativeText(state.nowPlaying.title);
  _artist.stringValue = nativeText(state.nowPlaying.artist);
  _title.toolTip = _title.stringValue;
  _soul.enabled = static_cast<BOOL>((modern == NO) || play.enabled);
  _soul.toolTip =
    (modern != NO) ? nativeText(play.tooltip) : nativeText(state.nowPlaying.audioPipeline.plainTextFallback);
  auto* const soulDescription =
    (modern != NO) ? nativeText(play.tooltip) : ao::appkit::catalogText(*_optCatalog, MessageId::AppKitOutputDevice);

  if ([_soul.accessibilityLabel isEqualToString:soulDescription] == NO)
  {
    _soul.accessibilityLabel = soulDescription;
  }

  for (auto const& button : std::array<NSButton*, 6>{_play, _stop, _previous, _next, _shuffle, _repeat})
  {
    auto const& transport = state.transport.at(static_cast<std::size_t>(button.tag));
    button.enabled = static_cast<BOOL>(transport.enabled);
    button.contentTintColor = transport.engaged ? NSColor.controlAccentColor : NSColor.labelColor;
    button.toolTip = nativeText(transport.tooltip);
  }

  if (auto* const playDescription = nativeText(play.tooltip);
      [_play.accessibilityLabel isEqualToString:playDescription] == NO)
  {
    _play.accessibilityLabel = playDescription;
  }

  auto const& shuffle = state.transport[static_cast<std::size_t>(ao::uimodel::PlaybackCommand::ToggleShuffle)];
  auto const shuffleState = shuffle.engaged ? NSControlStateValueOn : NSControlStateValueOff;

  if (_shuffle.state != shuffleState)
  {
    _shuffle.state = shuffleState;
  }

  auto const& repeat = state.transport[static_cast<std::size_t>(ao::uimodel::PlaybackCommand::CycleRepeat)];
  auto repeatMessage = MessageId::AppKitRepeatOff;

  if (repeat.icon == ao::uimodel::TransportIcon::RepeatOne)
  {
    repeatMessage = MessageId::AppKitRepeatOne;
  }
  else if (repeat.engaged)
  {
    repeatMessage = MessageId::AppKitRepeatAll;
  }

  auto* const repeatDescription = ao::appkit::catalogText(*_optCatalog, repeatMessage);

  if ([_repeat.accessibilityValue isEqual:repeatDescription] == NO ||
      [_repeat.accessibilityValueDescription isEqualToString:repeatDescription] == NO)
  {
    _repeat.accessibilityValue = repeatDescription;
    _repeat.accessibilityValueDescription = repeatDescription;
    ::NSAccessibilityPostNotification(_repeat, NSAccessibilityValueChangedNotification);
  }

  _play.image = [NSImage imageWithSystemSymbolName:play.playing ? @"pause.fill" : @"play.fill"
                          accessibilityDescription:nil];
  _repeat.image = [NSImage
    imageWithSystemSymbolName:state.transport[static_cast<std::size_t>(ao::uimodel::PlaybackCommand::CycleRepeat)]
                                    .icon == ao::uimodel::TransportIcon::RepeatOne
                                ? @"repeat.1"
                                : @"repeat"
     accessibilityDescription:nil];
  auto const duration = state.position.duration;
  _duration.stringValue = nativeText(
    ao::uimodel::formatPlaybackTime(ao::uimodel::PlaybackTimeMode::Duration, std::chrono::milliseconds{}, duration));
  _seek.enabled = static_cast<BOOL>(state.position.seekable);
  [_seek presentSeekTarget:state.position.seekable ? std::optional{ao::appkit::PlaybackSeekTarget{
                                                       .revision = state.positionRevision, .duration = duration}}
                                                   : std::nullopt];
  _volume.enabled = static_cast<BOOL>(state.volume.visible);
  _volumeButton.enabled = _volume.enabled;
  _volumeButton.image =
    [NSImage imageWithSystemSymbolName:!state.volume.muted && state.volume.volume > 0 ? @"speaker.wave.2.fill"
                                                                                      : @"speaker.slash.fill"
              accessibilityDescription:ao::appkit::catalogText(*_optCatalog, MessageId::AppKitVolume)];

  if ([_volume isTrackingGesture] == NO)
  {
    _volume.doubleValue = state.volume.volume;
  }

  _volume.accessibilityValueDescription = nativeText(state.volume.tooltip);
  _volume.toolTip = nativeText(state.volume.tooltip);
  _output.toolTip = nativeText(state.output.outputDeviceStatus);

  if (auto const span = state.playingCover.view(); _playingBytes.view().data() != span.data())
  {
    _playingBytes = state.playingCover;
    _cover.image =
      span.empty() ? nil : [[NSImage alloc] initWithData:[NSData dataWithBytes:span.data() length:span.size()]];
    _cover.accessibilityValue = _cover.image == nil
                                  ? ao::appkit::catalogText(*_optCatalog, MessageId::CoverArtNone)
                                  : ao::appkit::catalogText(*_optCatalog, MessageId::AppKitArtworkAvailable);
  }

  _playingArtworkMirror.image = _cover.image;
  _playingArtworkMirror.accessibilityValue = _cover.accessibilityValue;
}

- (void)renderFrameForState:(ao::appkit::DesktopViewState const&)state
                    elapsed:(std::chrono::milliseconds)elapsed
                     modern:(BOOL)modern
{
  if (_detached != NO)
  {
    return;
  }

  auto const& play = state.transport[static_cast<std::size_t>(ao::uimodel::PlaybackCommand::PlayPause)];
  [_soul presentState:state.soul playing:static_cast<BOOL>(play.playing) modern:modern];
  auto const duration = state.position.duration;
  _elapsed.stringValue = nativeText(ao::uimodel::formatPlaybackTime(
    (modern != NO) ? ao::uimodel::PlaybackTimeMode::Elapsed : ao::uimodel::PlaybackTimeMode::Combined,
    elapsed,
    duration));
  _seek.accessibilityValueDescription = ao::appkit::catalogFormat(
    *_optCatalog,
    MessageId::AppKitPlaybackPositionValue,
    {{"elapsed", ao::uimodel::formatPlaybackTime(ao::uimodel::PlaybackTimeMode::Elapsed, elapsed, duration)},
     {"duration", ao::uimodel::formatPlaybackTime(ao::uimodel::PlaybackTimeMode::Duration, elapsed, duration)}});

  if ([_seek isTrackingGesture] == NO)
  {
    _seek.doubleValue =
      duration.count() > 0 ? static_cast<double>(elapsed.count()) / static_cast<double>(duration.count()) : 0;
  }
}

- (void)toggleVolumePopover
{
  if (_detached != NO)
  {
    return;
  }

  if (_modern == NO || _volume.enabled == NO)
  {
    return;
  }

  if (_volumePopover.shown != NO)
  {
    [self closePopover];
    return;
  }

  [_volumePopover showRelativeToRect:_volumeButton.bounds ofView:_volumeButton preferredEdge:NSRectEdgeMinY];
  [_volumePopover.contentViewController.view.window makeFirstResponder:_volume];
}

- (void)presentOutputMenu:(NSMenu*)menu
{
  if (_detached != NO)
  {
    return;
  }

  auto* const anchor = (_modern != NO) ? static_cast<NSView*>(_output) : static_cast<NSView*>(_soul);
  [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, anchor.bounds.size.height) inView:anchor];
}

- (void)presentPlaybackOptionsMenu:(NSMenu*)menu
{
  if (_detached != NO)
  {
    return;
  }

  [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, 0) inView:_playbackOptions];
}

- (void)closePopover
{
  [_volumePopover close];
}
- (void)detach
{
  _detached = YES;
  [_seek presentSeekTarget:std::nullopt];
  [self closePopover];

  for (auto const& control : std::array<NSControl*, 12>{_soul,
                                                        _play,
                                                        _stop,
                                                        _previous,
                                                        _next,
                                                        _shuffle,
                                                        _repeat,
                                                        _output,
                                                        _volumeButton,
                                                        _playbackOptions,
                                                        _seek,
                                                        _volume})
  {
    control.target = nil;
    control.action = nullptr;
  }

  _actionTarget = nil;
  _playingArtworkMirror = nil;
  _playingBytes = {};
}
@end
