// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitPlaybackScenario.h"

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/LibrarySession.h"
#include "app/macos-appkit/PlaybackBar.h"
#include "app/macos-appkit/SoulButton.h"
#include <ao/Contract.h>
#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

@interface AobusPresentationActionTarget : NSObject
- (instancetype)initWithSession:(ao::appkit::LibrarySession&)session;
- (void)transport:(NSControl*)sender;
- (void)seek:(NSSlider*)sender;
- (void)volume:(NSSlider*)sender;
- (void)showVolume:(id)sender;
- (void)showOutput:(id)sender;
- (void)showOptions:(id)sender;
- (NSUInteger)seekCount;
- (NSUInteger)volumeCount;
- (double)lastSeekFraction;
- (double)lastVolume;
@end

@implementation AobusPresentationActionTarget {
  ao::appkit::LibrarySession* _session;
  NSUInteger _seekCount;
  NSUInteger _volumeCount;
  double _lastSeekFraction;
  double _lastVolume;
}

- (instancetype)initWithSession:(ao::appkit::LibrarySession&)session
{
  self = [super init];

  if (self != nil)
  {
    _session = &session;
  }

  return self;
}

- (void)transport:(NSControl*)sender
{
  _session->execute(static_cast<ao::uimodel::PlaybackCommand>(sender.tag));
}

- (void)seek:(NSSlider*)sender
{
  _lastSeekFraction = sender.doubleValue;
  ++_seekCount;

  if (auto const optTarget = [static_cast<AobusSeekSlider*>(sender) seekTarget]; optTarget)
  {
    _session->seek(_lastSeekFraction, *optTarget);
  }
}

- (void)volume:(NSSlider*)sender
{
  _lastVolume = sender.doubleValue;
  ++_volumeCount;
  _session->setVolume(static_cast<float>(_lastVolume));
}

- (void)showVolume:(id) [[maybe_unused]] sender
{
}

- (void)showOutput:(id) [[maybe_unused]] sender
{
}

- (void)showOptions:(id) [[maybe_unused]] sender
{
}

- (NSUInteger)seekCount
{
  return _seekCount;
}

- (NSUInteger)volumeCount
{
  return _volumeCount;
}

- (double)lastSeekFraction
{
  return _lastSeekFraction;
}

- (double)lastVolume
{
  return _lastVolume;
}
@end

@interface AobusPlaybackHierarchyProbe : NSView
- (void)arm;
- (NSUInteger)windowMoveCount;
@end

@implementation AobusPlaybackHierarchyProbe {
  BOOL _armed;
  NSUInteger _windowMoveCount;
}
- (void)arm
{
  _armed = YES;
}

- (NSUInteger)windowMoveCount
{
  return _windowMoveCount;
}

- (void)viewDidMoveToWindow
{
  [super viewDidMoveToWindow];

  if (_armed != NO)
  {
    ++_windowMoveCount;
  }
}
@end

namespace
{
  using namespace std::chrono_literals;

  constexpr double kColorTolerance = 0.12;
  constexpr double kAmberExclusionTolerance = 0.24;
  constexpr double kControlTolerance = 0.001;
  constexpr double kRingAlphaThreshold = 0.2;
  constexpr double kRingColorTolerance = 0.35;
  constexpr double kMinimumRingStrokeFraction = 0.11;

  template<typename Control>
  Control* requireControl(NSView* root, NSString* identifier)
  {
    auto* const control = ao::appkit::test::findControl(root, identifier);
    AO_INVARIANT(control != nil, "Missing native playback control {}", identifier.UTF8String);
    return static_cast<Control*>(control);
  }

  NSImageView* requireImageView(NSView* root)
  {
    auto* const children = root.subviews;

    for (NSUInteger index = 0; index < children.count; ++index)
    {
      if ([children[index] isKindOfClass:NSImageView.class] != NO)
      {
        return static_cast<NSImageView*>(children[index]);
      }
    }

    AO_INVARIANT(false, "Classic playback must expose its volume image");
    return nil;
  }

  NSBitmapImageRep* renderedBitmap(NSView* view)
  {
    [view displayIfNeeded];
    auto* const bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    AO_INVARIANT(bitmap != nil, "Soul rendering must provide a bitmap representation");
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
    return bitmap;
  }

  double colorDistance(NSColor* color, ao::uimodel::AobusSoulRgb expected)
  {
    auto* const rgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];

    if (rgb == nil)
    {
      return std::numeric_limits<double>::infinity();
    }

    auto const expectedRed = static_cast<double>(expected.red) / 255.0;
    auto const expectedGreen = static_cast<double>(expected.green) / 255.0;
    auto const expectedBlue = static_cast<double>(expected.blue) / 255.0;
    auto const red = rgb.redComponent - expectedRed;
    auto const green = rgb.greenComponent - expectedGreen;
    auto const blue = rgb.blueComponent - expectedBlue;
    return std::sqrt((red * red) + (green * green) + (blue * blue));
  }

  NSColor* pixelColor(NSBitmapImageRep* bitmap, NSInteger x, NSInteger y)
  {
    auto* const sampled = [bitmap colorAtX:x y:y];

    if (sampled == nil)
    {
      return nil;
    }

    // colorAtX:y: can label sRGB bitmap samples as Generic RGB. Keep the
    // bitmap's profile before comparing colors in a calibrated space.
    auto components = std::array<CGFloat, 4>{};
    AO_INVARIANT(sampled.numberOfComponents == components.size(), "Soul capture must contain RGBA samples");
    [sampled getComponents:components.data()];
    return [NSColor colorWithColorSpace:bitmap.colorSpace components:components.data() count:components.size()];
  }

  bool isVisible(NSColor* color)
  {
    auto* const rgb = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    return rgb != nil && rgb.alphaComponent > kRingAlphaThreshold;
  }

  void verifySoulPalette(NSBitmapImageRep* bitmap)
  {
    // The circular stroke samples the diagonal gradient after its cyan endpoint.
    // Pin the visible Radiant palette independently of the production color recipe.
    constexpr auto kVisibleCore = ao::uimodel::AobusSoulRgb{.red = 64, .green = 174, .blue = 252};
    constexpr auto kRadiantBody = ao::uimodel::AobusSoulRgb{.red = 168, .green = 85, .blue = 247};
    constexpr auto kAnchorAmber = ao::uimodel::AobusSoulRgb{.red = 249, .green = 115, .blue = 22};
    double nearestCore = std::numeric_limits<double>::infinity();
    double nearestBody = std::numeric_limits<double>::infinity();
    double nearestAmber = std::numeric_limits<double>::infinity();

    for (NSInteger y = 0; y < bitmap.pixelsHigh; ++y)
    {
      for (NSInteger x = 0; x < bitmap.pixelsWide; ++x)
      {
        auto* const color = pixelColor(bitmap, x, y);

        if (color == nil || !isVisible(color))
        {
          continue;
        }

        nearestCore = std::min(nearestCore, colorDistance(color, kVisibleCore));
        nearestBody = std::min(nearestBody, colorDistance(color, kRadiantBody));
        nearestAmber = std::min(nearestAmber, colorDistance(color, kAnchorAmber));
      }
    }

    AO_INVARIANT(nearestCore < kColorTolerance && nearestBody < kColorTolerance,
                 "Soul palette must include cyan and aura colors: distances {}, {}",
                 nearestCore,
                 nearestBody);
    AO_INVARIANT(
      nearestAmber > kAmberExclusionTolerance, "Soul-only playback control must not render the anchor amber color");
  }

  bool isSoulColor(NSColor* color, ao::uimodel::AobusSoulRgb body)
  {
    return isVisible(color) && std::min(colorDistance(color, ao::uimodel::kAobusSoulUiCyan),
                                        colorDistance(color, body)) < kRingColorTolerance;
  }

  void verifySoulStrokeThickness(NSBitmapImageRep* bitmap, ao::uimodel::AobusSoulRgb body)
  {
    auto const y = bitmap.pixelsHigh / 2;
    auto const center = bitmap.pixelsWide / 2;
    NSInteger outer = -1;

    for (NSInteger x = bitmap.pixelsWide - 1; x >= center; --x)
    {
      if (auto* const color = pixelColor(bitmap, x, y); color != nil && isSoulColor(color, body))
      {
        outer = x;
        break;
      }
    }

    AO_INVARIANT(outer >= center, "Animated Soul must render a visible ring");
    auto inner = outer;

    while (inner >= center)
    {
      if (auto* const color = pixelColor(bitmap, inner, y); color == nil || !isSoulColor(color, body))
      {
        break;
      }

      --inner;
    }

    auto const strokeFraction = static_cast<double>(outer - inner) / static_cast<double>(bitmap.pixelsWide);
    AO_INVARIANT(
      strokeFraction >= kMinimumRingStrokeFraction, "Soul ring must retain the shared visible stroke thickness");
  }

  NSEvent* rightArrowEvent(NSWindow* window)
  {
    unichar const character = NSRightArrowFunctionKey;
    auto* const text = [NSString stringWithCharacters:&character length:1];
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:NSEventModifierFlagFunction | NSEventModifierFlagNumericPad
                           timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:window.windowNumber
                             context:nil
                          characters:text
         charactersIgnoringModifiers:text
                           isARepeat:NO
                             keyCode:124];
  }

  void requireVolumeReadback(ao::appkit::LibrarySession& session, double expected, NSString* obligation)
  {
    auto const reached = ao::appkit::test::tryWaitUntil(
      [&] { return std::abs(session.state().volume.volume - static_cast<float>(expected)) < 0.001F; });
    AO_INVARIANT(reached, "{} did not reach shared playback state", obligation.UTF8String);
  }
} // namespace

namespace ao::appkit::test
{
  namespace
  {
    NSEvent* seekMouseEvent(NSWindow* window, NSEventType type, NSPoint point)
    {
      return [NSEvent mouseEventWithType:type
                                location:point
                           modifierFlags:0
                               timestamp:NSProcessInfo.processInfo.systemUptime
                            windowNumber:window.windowNumber
                                 context:nil
                             eventNumber:0
                              clickCount:1
                                pressure:1];
    }

    void exerciseSeekGesture(LibrarySession& session,
                             AobusPlaybackBar* playback,
                             AobusPresentationActionTarget* target,
                             NSWindow* window,
                             TrackId replacement)
    {
      auto* const slider = requireControl<AobusSeekSlider>(playback.modernView, @"playback.seek");
      [playback renderState:session.state() modern:YES];
      [playback renderFrameForState:session.state() elapsed:session.playbackElapsed() modern:YES];
      auto const initialRevision = session.state().positionRevision;
      auto const expectedTrack = replacement == kInvalidTrackId
                                   ? session.runtime().playback().snapshot().transport.nowPlaying.trackId
                                   : replacement;
      auto const beforeCount = target.seekCount;
      auto const knob = [static_cast<NSSliderCell*>(slider.cell) knobRectFlipped:slider.flipped];
      auto const start = [slider convertPoint:NSMakePoint(NSMidX(knob), NSMidY(knob)) toView:nil];
      auto const end = [slider convertPoint:NSMakePoint(slider.bounds.size.width * 0.7, NSMidY(knob)) toView:nil];
      struct Observation final
      {
        bool requested = false;
        bool released = false;
        rt::PlaybackFinalSeekRevision finalSeek{};
      };
      auto observation = Observation{};
      auto* const observed = &observation;
      auto* const sessionBorrow = &session;
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
      auto* const timer = [NSTimer
        timerWithTimeInterval:0.01
                      repeats:YES
                        block:^(NSTimer* tick) {
                          AO_INVARIANT(
                            [slider isTrackingGesture] != NO, "Transition must occur inside native mouse tracking");

                          if (!observed->requested)
                          {
                            observed->requested = true;

                            if (replacement != kInvalidTrackId)
                            {
                              sessionBorrow->play(replacement);
                            }
                          }

                          auto const& snapshot = sessionBorrow->runtime().playback().snapshot().transport;
                          auto const ready =
                            snapshot.nowPlaying.trackId == expectedTrack &&
                            snapshot.transport == audio::Transport::Playing &&
                            (replacement == kInvalidTrackId || snapshot.positionRevision != initialRevision);

                          if (ready || std::chrono::steady_clock::now() >= deadline)
                          {
                            observed->released = ready;
                            observed->finalSeek = snapshot.finalSeekRevision;
                            [playback renderState:sessionBorrow->state() modern:YES];
                            [playback renderFrameForState:sessionBorrow->state()
                                                  elapsed:sessionBorrow->playbackElapsed()
                                                   modern:YES];
                            [NSApp postEvent:seekMouseEvent(window, NSEventTypeLeftMouseDragged, end) atStart:NO];
                            [NSApp postEvent:seekMouseEvent(window, NSEventTypeLeftMouseUp, end) atStart:NO];
                            [tick invalidate];
                          }
                        }];
      [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
      [slider mouseDown:seekMouseEvent(window, NSEventTypeLeftMouseDown, start)];
      [timer invalidate];
      AO_INVARIANT(observation.released && target.seekCount > beforeCount,
                   "Native drag must observe its transition and deliver mouse-up through target/action");
      settleNativeCallbacks();
      auto const finalSeek = session.runtime().playback().snapshot().transport.finalSeekRevision;
      AO_INVARIANT(
        replacement == kInvalidTrackId ? finalSeek != observation.finalSeek : finalSeek == observation.finalSeek,
        "Only a gesture on the same timeline may commit a final seek");
    }
  } // namespace

  void exercisePlaybackPresentation(LibrarySession& session,
                                    NSWindow* window,
                                    NSView* root,
                                    std::filesystem::path const& stateRoot)
  {
    auto* const mirror = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
    auto* const target = [[AobusPresentationActionTarget alloc] initWithSession:session];
    auto* const playback = [[AobusPlaybackBar alloc] initWithCatalog:session.catalog()
                                                              target:target
                                                     transportAction:@selector(transport:)
                                                          seekAction:@selector(seek:)
                                                        volumeAction:@selector(volume:)
                                                    showVolumeAction:@selector(showVolume:)
                                                    showOutputAction:@selector(showOutput:)
                                                   showOptionsAction:@selector(showOptions:)
                                                playingArtworkMirror:mirror];
    auto state = session.state();
    state.nowPlaying.title = "Presentation scenario title";
    state.nowPlaying.artist = "Presentation scenario artist";
    state.position.duration = 4min;
    state.position.elapsed = 1min;
    state.position.seekable = true;
    state.volume.visible = true;
    state.volume.volume = 0.72F;
    state.volume.tooltip = "Volume 72%";
    state.soul.aura = uimodel::SoulAura::Radiant;
    state.soul.motionMode = uimodel::AobusSoulMotionMode::Dormant;
    auto& play = state.transport[static_cast<std::size_t>(uimodel::PlaybackCommand::PlayPause)];
    play.enabled = true;
    play.playing = false;
    play.tooltip = "Play this fixture";
    auto& shuffle = state.transport[static_cast<std::size_t>(uimodel::PlaybackCommand::ToggleShuffle)];
    shuffle.enabled = true;
    shuffle.engaged = false;
    shuffle.tooltip = "Shuffle Off";
    auto& repeat = state.transport[static_cast<std::size_t>(uimodel::PlaybackCommand::CycleRepeat)];
    repeat.enabled = true;
    repeat.engaged = false;
    repeat.icon = uimodel::TransportIcon::Repeat;
    repeat.tooltip = "Repeat Off";

    [playback layoutInRoot:root modern:YES];
    [playback renderState:state modern:YES];
    [playback renderFrameForState:state elapsed:state.position.elapsed modern:YES];
    [root layoutSubtreeIfNeeded];
    AO_INVARIANT(playback.activeView == playback.modernView && playback.modernView.superview == root,
                 "modern playback mode must install the modern view in the host");
    auto* const modernSeek = requireControl<NSSlider>(playback.modernView, @"playback.seek");
    auto* const soul = requireControl<AobusSoulButton>(playback.modernView, @"playback.soul");
    auto* const cover = requireControl<NSImageView>(playback.modernView, @"playback.cover");
    auto* const shuffleButton = requireControl<NSButton>(playback.modernView, @"playback.shuffle");
    auto* const repeatButton = requireControl<NSButton>(playback.modernView, @"playback.repeat");
    AO_INVARIANT([modernSeek.accessibilityValueDescription isEqualToString:@"1:00 of 4:00"] != 0,
                 "playback seek must expose elapsed and duration accessibility text");
    AO_INVARIANT([soul.accessibilityLabel isEqualToString:@"Play this fixture"] != 0,
                 "Modern Soul must expose the dynamic transport tooltip as its accessible action");
    AO_INVARIANT(shuffleButton.state == NSControlStateValueOff, "Shuffle must expose its initial native toggle state");
    AO_INVARIANT([static_cast<NSString*>(repeatButton.accessibilityValue) isEqualToString:@"Repeat Off"] != 0 &&
                   [repeatButton.accessibilityValueDescription isEqualToString:@"Repeat Off"] != 0,
                 "Repeat Off must expose a complete accessibility state");
    play.playing = true;
    play.tooltip = "Pause this fixture";
    [playback renderState:state modern:YES];
    AO_INVARIANT([soul.accessibilityLabel isEqualToString:@"Pause this fixture"] != 0,
                 "Transport transitions must update the accessible Modern Play/Pause action");
    play.playing = false;
    play.tooltip = "Play this fixture";
    shuffle.engaged = true;
    repeat.engaged = true;
    [playback renderState:state modern:YES];
    AO_INVARIANT(shuffleButton.state == NSControlStateValueOn &&
                   [static_cast<NSString*>(repeatButton.accessibilityValue) isEqualToString:@"Repeat All"] != 0 &&
                   [repeatButton.accessibilityValueDescription isEqualToString:@"Repeat All"] != 0,
                 "Shuffle and Repeat All must update their native accessibility state");
    repeat.icon = uimodel::TransportIcon::RepeatOne;
    [playback renderState:state modern:YES];
    AO_INVARIANT([static_cast<NSString*>(repeatButton.accessibilityValue) isEqualToString:@"Repeat One"] != 0 &&
                   [repeatButton.accessibilityValueDescription isEqualToString:@"Repeat One"] != 0,
                 "Repeat One must remain distinct from Repeat All");
    shuffle.engaged = false;
    repeat.engaged = false;
    repeat.icon = uimodel::TransportIcon::Repeat;
    [playback renderState:state modern:YES];
    AO_INVARIANT(cover.accessibilityLabel.length > 0 && [static_cast<NSString*>(cover.accessibilityValue) length] > 0,
                 "playing artwork must expose label and availability accessibility text");

    auto zeroState = state;
    zeroState.position.duration = 0ms;
    zeroState.position.elapsed = 0ms;
    zeroState.position.seekable = false;
    [playback renderState:zeroState modern:YES];
    [playback renderFrameForState:zeroState elapsed:0ms modern:YES];
    auto* const modernElapsed = requireControl<NSTextField>(playback.modernView, @"playback.elapsed");
    auto* const modernDuration = requireControl<NSTextField>(playback.modernView, @"playback.duration");
    AO_INVARIANT([modernElapsed.stringValue isEqualToString:@"0:00"] != 0 &&
                   [modernDuration.stringValue isEqualToString:@"0:00"] != 0,
                 "zero playback time must remain readable in Modern mode");
    [playback renderState:state modern:YES];
    [playback renderFrameForState:state elapsed:state.position.elapsed modern:YES];

    modernSeek.doubleValue = 0.25;
    auto const seekSent = [NSApp sendAction:modernSeek.action to:modernSeek.target from:modernSeek];
    AO_INVARIANT(seekSent != 0, "playback seek target/action must remain connected");
    AO_INVARIANT(target.seekCount == 1 && std::abs(target.lastSeekFraction - 0.25) < kControlTolerance,
                 "playback seek action must deliver the selected fraction");

    [soul displayIfNeeded];
    [soul.layer displayIfNeeded];
    [soul presentState:state.soul playing:NO modern:YES];
    auto* const soulBitmap = renderedBitmap(soul);
    captureView(soul, stateRoot / "appkit-soul-palette.png");
    verifySoulPalette(soulBitmap);
    [soul presentState:state.soul playing:NO modern:YES];
    AO_INVARIANT(
      soul.needsDisplay == 0 && soul.layer.needsDisplay == 0, "repeating an unchanged soul state must suppress redraw");
    [soul presentState:state.soul playing:YES modern:YES];
    AO_INVARIANT(
      soul.needsDisplay != 0 || soul.layer.needsDisplay != 0, "a changed soul transport glyph must request redraw");
    state.soul.motionMode = uimodel::AobusSoulMotionMode::Animating;
    [soul presentState:state.soul playing:YES modern:YES];
    verifySoulStrokeThickness(renderedBitmap(soul), uimodel::kAobusSoulRadiant);
    state.soul.motionMode = uimodel::AobusSoulMotionMode::Dormant;
    [soul presentState:state.soul playing:NO modern:YES];

    auto const originalSize = root.frame.size;
    [window setContentSize:NSMakeSize(760, 520)];
    [playback layoutInRoot:root modern:YES];
    [root layoutSubtreeIfNeeded];
    auto* const transport = findView(playback.modernView, @"playback.transport");
    auto* const nowPlaying = findView(playback.modernView, @"playback.now-playing");
    AO_INVARIANT(transport != nil && nowPlaying != nil);
    auto const playerRect = [playback.modernView convertRect:playback.modernView.bounds toView:root];
    auto const seekRect = [modernSeek convertRect:modernSeek.bounds toView:root];
    auto const soulRect = [soul convertRect:soul.bounds toView:root];
    auto const transportRect = [transport convertRect:transport.bounds toView:root];
    auto const nowPlayingRect = [nowPlaying convertRect:nowPlaying.bounds toView:root];
    AO_INVARIANT(std::abs(playerRect.size.width - root.bounds.size.width) < 1 &&
                   std::abs(NSMaxY(playerRect) - NSMaxY(root.bounds)) < 1,
                 "Compact playback must span the bottom of its host");
    AO_INVARIANT(playerRect.size.height < 90 && playerRect.size.height >= soulRect.size.height &&
                   std::abs(NSMidX(soulRect) - NSMidX(root.bounds)) < 1,
                 "Compact playback must keep the primary transport centered and bounded");
    AO_INVARIANT(seekRect.size.width >= root.bounds.size.width - 32 && NSMaxY(seekRect) <= NSMinY(soulRect),
                 "Compact seek must span the player above its controls");
    AO_INVARIANT(NSMaxX(nowPlayingRect) < NSMinX(transportRect) && playback.modernView.hasAmbiguousLayout == 0 &&
                   transport.hasAmbiguousLayout == 0,
                 "Compact metadata and transport must not overlap or have ambiguous constraints");
    AO_INVARIANT(cover.hidden == 0 && requireControl<NSButton>(playback.modernView, @"playback.shuffle").hidden == 0 &&
                   requireControl<NSButton>(playback.modernView, @"playback.repeat").hidden == 0,
                 "Compact playback must retain artwork, shuffle, and repeat controls");
    captureView(root, stateRoot / "appkit-playback-compact.png");

    auto classicState = state;
    classicState.position.elapsed = 1h + 1min + 1s;
    classicState.position.duration = 2h + 2min + 2s;
    [playback layoutInRoot:root modern:NO];
    [playback renderState:classicState modern:NO];
    [playback renderFrameForState:classicState elapsed:classicState.position.elapsed modern:NO];
    [playback layoutInRoot:root modern:NO];
    [root layoutSubtreeIfNeeded];
    AO_INVARIANT(playback.activeView == playback.classicView && playback.classicView.superview == root,
                 "classic playback mode must replace the modern view in the host");
    AO_INVARIANT(playback.modernView.superview == nil, "classic playback mode must remove the inactive modern view");
    auto* const classicSeek = requireControl<NSSlider>(playback.classicView, @"playback.seek");
    auto* const classicVolume = requireControl<NSSlider>(playback.classicView, @"playback.volume");
    auto* const classicElapsed = requireControl<NSTextField>(playback.classicView, @"playback.elapsed");
    auto* const volumeIcon = requireImageView(playback.classicView);
    AO_INVARIANT(
      classicSeek == modernSeek && requireControl<AobusSoulButton>(playback.classicView, @"playback.soul") == soul,
      "mode switching must reparent the shared seek and soul controls");
    AO_INVARIANT([soul.accessibilityLabel isEqualToString:@"Output Device"] != 0,
                 "Classic Soul must expose its Output action rather than the play tooltip");
    auto* const playButton = requireControl<NSButton>(playback.classicView, @"playback.play");
    AO_INVARIANT([playButton.accessibilityLabel isEqualToString:@"Play this fixture"] != 0,
                 "Classic Play must expose the dynamic transport action");
    play.playing = true;
    play.tooltip = "Pause this fixture";
    [playback renderState:state modern:NO];
    AO_INVARIANT([playButton.accessibilityLabel isEqualToString:@"Pause this fixture"] != 0,
                 "Transport transitions must update the accessible Classic Play/Pause action");
    play.playing = false;
    play.tooltip = "Play this fixture";
    [playback renderState:state modern:NO];
    AO_INVARIANT([classicElapsed.stringValue isEqualToString:@"61:01 / 122:02"] != 0 &&
                   classicElapsed.fittingSize.width <= classicElapsed.bounds.size.width,
                 "Classic playback must retain readable hour-long elapsed and duration text");
    AO_INVARIANT(NSMaxX(classicSeek.frame) < NSMinX(classicElapsed.frame) &&
                   NSMaxX(classicElapsed.frame) < NSMinX(volumeIcon.frame),
                 "Classic seek, time, and volume glyphs must occupy disjoint geometry");

    [playback renderState:zeroState modern:NO];
    [playback renderFrameForState:zeroState elapsed:0ms modern:NO];
    AO_INVARIANT([classicElapsed.stringValue isEqualToString:@"0:00 / 0:00"] != 0,
                 "zero playback time must remain readable in Classic mode");
    [playback renderState:state modern:NO];
    [playback renderFrameForState:state elapsed:state.position.elapsed modern:NO];
    AO_INVARIANT([classicVolume.accessibilityValueDescription isEqualToString:@"Volume 72%"] != 0,
                 "classic volume must expose accessible state after reparenting");

    [window makeFirstResponder:classicSeek];
    auto* const hierarchyProbe = [[AobusPlaybackHierarchyProbe alloc] initWithFrame:NSZeroRect];
    hierarchyProbe.hidden = YES;
    [classicSeek addSubview:hierarchyProbe];
    AO_INVARIANT(hierarchyProbe.window == window, "Classic hierarchy probe must begin in the host window");
    [hierarchyProbe arm];
    [playback layoutInRoot:root modern:NO];
    [playback layoutInRoot:root modern:NO];
    AO_INVARIANT(window.firstResponder == classicSeek && classicSeek.superview == playback.classicView &&
                   hierarchyProbe.windowMoveCount == 0,
                 "repeated Classic layout must preserve control focus and window hierarchy");

    classicVolume.doubleValue = 0.35;
    auto const volumeSent = [NSApp sendAction:classicVolume.action to:classicVolume.target from:classicVolume];
    AO_INVARIANT(volumeSent != 0, "playback volume target/action must remain connected after reparenting");
    AO_INVARIANT(target.volumeCount == 1 && std::abs(target.lastVolume - 0.35) < kControlTolerance,
                 "playback volume action must deliver the selected value");
    requireVolumeReadback(session, target.lastVolume, @"direct volume action");

    auto const acceptedClassicVolumeFocus = [window makeFirstResponder:classicVolume];
    AO_INVARIANT(acceptedClassicVolumeFocus != 0, "Classic volume must accept keyboard focus");
    auto const keyboardCount = target.volumeCount;
    auto const keyboardValue = classicVolume.doubleValue;
    [classicVolume keyDown:rightArrowEvent(window)];
    AO_INVARIANT(target.volumeCount == keyboardCount + 1 && target.lastVolume > keyboardValue + kControlTolerance,
                 "a keyboard arrow must route the changed volume through target/action");
    requireVolumeReadback(session, target.lastVolume, @"keyboard volume action");

    id<NSAccessibility> const volumeAccessibility = ::NSAccessibilityUnignoredDescendant(classicVolume);
    AO_INVARIANT([volumeAccessibility.accessibilityRole isEqualToString:NSAccessibilitySliderRole] != 0,
                 "volume must expose a native accessibility slider");
    auto const incrementCount = target.volumeCount;
    auto const incrementValue = classicVolume.doubleValue;
    // AppKit can return NO after dispatching a successful slider action.
    // Observe the target and playback state rather than that return value.
    std::ignore = [volumeAccessibility accessibilityPerformIncrement];
    AO_INVARIANT(target.volumeCount == incrementCount + 1 && target.lastVolume > incrementValue + kControlTolerance,
                 "accessibility increment must route the changed volume through target/action");
    requireVolumeReadback(session, target.lastVolume, @"accessibility volume increment");

    auto const decrementCount = target.volumeCount;
    auto const decrementValue = classicVolume.doubleValue;
    std::ignore = [volumeAccessibility accessibilityPerformDecrement];
    AO_INVARIANT(target.volumeCount == decrementCount + 1 && target.lastVolume < decrementValue - kControlTolerance,
                 "accessibility decrement must route the changed volume through target/action");
    requireVolumeReadback(session, target.lastVolume, @"accessibility volume decrement");

    [window setContentSize:originalSize];
    [playback layoutInRoot:root modern:YES];
    [playback renderState:state modern:YES];
    [playback renderFrameForState:state elapsed:state.position.elapsed modern:YES];
    [root layoutSubtreeIfNeeded];
    AO_INVARIANT(classicVolume.superview != nil &&
                   [classicVolume.superview.identifier isEqualToString:@"playback.volume-popover"] != 0,
                 "modern mode must reparent volume into its popover content");
    [playback toggleVolumePopover];
    requireWaitUntil([&] { return classicVolume.window != nil && classicVolume.window.visible != 0; },
                     "enabled modern volume control did not open its popover");
    AO_INVARIANT(classicVolume.window != window, "volume popover content must live in its transient native window");
    captureView(classicVolume.superview, stateRoot / "appkit-volume-popover.png");
    [playback toggleVolumePopover];
    requireWaitUntil([&] { return classicVolume.window == nil || classicVolume.window.visible == 0; },
                     "volume popover did not close on a second toggle");

    auto tracks = std::vector<TrackId>{};

    for (std::size_t index = 0; index < session.displayIndex().rowCount() && tracks.size() < 2; ++index)
    {
      if (auto const* row = session.rowAt(index); row != nullptr)
      {
        tracks.push_back(row->id);
      }
    }

    AO_INVARIANT(tracks.size() == 2, "Seek regression requires two fixture tracks");
    auto const firstId = tracks[0];
    auto const secondId = tracks[1];
    session.play(firstId);
    requireWaitUntil(
      [&]
      {
        auto const& transport = session.runtime().playback().snapshot().transport;
        return transport.nowPlaying.trackId == firstId && transport.transport == audio::Transport::Playing;
      },
      "Seek regression requires active playback");
    exerciseSeekGesture(session, playback, target, window, secondId);
    exerciseSeekGesture(session, playback, target, window, secondId);
    exerciseSeekGesture(session, playback, target, window, kInvalidTrackId);
    session.runtime().playback().commands().stop();
    settleNativeCallbacks();
    [playback detach];
    AO_INVARIANT(modernSeek.target == nil && modernSeek.action == nullptr && classicVolume.target == nil &&
                   classicVolume.action == nullptr,
                 "playback detach must revoke native action targets");
  }
} // namespace ao::appkit::test
