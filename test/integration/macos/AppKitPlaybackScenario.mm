// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitPlaybackScenario.h"

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/AppKitText.h"
#include "app/macos-appkit/LibrarySession.h"
#include "app/macos-appkit/PlaybackBar.h"
#include "app/macos-appkit/SoulButton.h"
#include <ao/Contract.h>
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Quality.h>
#include <ao/audio/Transport.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <print>
#include <string>
#include <string_view>
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
- (void)setSeekObserver:(std::function<void(ao::uimodel::SeekSliderUpdate const&, bool)>)observer;
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
  std::function<void(ao::uimodel::SeekSliderUpdate const&, bool)> _seekObserver;
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

  auto const update = [static_cast<AobusSeekSlider*>(sender) seekUpdate];
  auto const optTarget = [static_cast<AobusSeekSlider*>(sender) seekTarget];

  if (_seekObserver)
  {
    _seekObserver(update, true);
  }

  if (optTarget)
  {
    _session->seek(update, *optTarget);
  }

  if (_seekObserver)
  {
    _seekObserver(update, false);
  }
}

- (void)setSeekObserver:(std::function<void(ao::uimodel::SeekSliderUpdate const&, bool)>)observer
{
  _seekObserver = std::move(observer);
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

// This target has no runtime collaborator. Any action is a scenario failure;
// the quality-only case inspects bindings without invoking them.
@interface AobusQualityPresentationActionTarget : NSObject
- (void)transport:(id)sender;
- (void)showOutput:(id)sender;
- (void)unexpectedAction:(id)sender;
@end

@implementation AobusQualityPresentationActionTarget
- (void)transport:(id) [[maybe_unused]] sender
{
  AO_INVARIANT(false, "Quality presentation must not invoke transport actions");
}

- (void)showOutput:(id) [[maybe_unused]] sender
{
  AO_INVARIANT(false, "Quality presentation must not invoke output actions");
}

- (void)unexpectedAction:(id) [[maybe_unused]] sender
{
  AO_INVARIANT(false, "Quality presentation must not invoke native actions");
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

    struct SeekGestureOptions final
    {
      bool paused = false;
      // Re-enter the slider's continuous action as AppKit does for each drag event.
      bool scrub = false;
    };

    void exerciseSeekGesture(LibrarySession& session,
                             AobusPlaybackBar* playback,
                             AobusPresentationActionTarget* target,
                             NSWindow* window,
                             TrackId replacement,
                             SeekGestureOptions const options = {})
    {
      if (options.paused)
      {
        session.runtime().playback().commands().pause();
        requireWaitUntil(
          [&] { return session.runtime().playback().snapshot().transport.transport == audio::Transport::Paused; },
          "Paused scrubbing requires a stable paused transport");
      }

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
      struct Observation final
      {
        bool transitioned = false;
        bool previewedWhileTracking = false;
        bool trackedPreviewAction = false;
        bool heldFinalUnchanged = true;
        bool scrubbing = false;
        std::size_t previewCount = 0;
        std::size_t commitCount = 0;
        std::vector<std::chrono::milliseconds> previewElapsed{};
        std::chrono::milliseconds committedElapsed{-1};
        rt::PlaybackFinalSeekRevision finalSeek{};
      };
      auto observation = Observation{.finalSeek = session.runtime().playback().snapshot().transport.finalSeekRevision};
      auto previewSub = session.runtime().playback().events().onSeekPreview(
        [&](std::chrono::milliseconds)
        {
          ++observation.previewCount;
          observation.previewedWhileTracking = observation.previewedWhileTracking || [slider isTrackingGesture] != NO;
        });
      [target
        setSeekObserver:[&](uimodel::SeekSliderUpdate const& update, bool beforeDispatch)
        {
          if (update.action == uimodel::SeekSliderAction::Commit)
          {
            if (beforeDispatch)
            {
              ++observation.commitCount;
              observation.committedElapsed = update.elapsed;
            }

            return;
          }

          if (update.action != uimodel::SeekSliderAction::Preview)
          {
            return;
          }

          if (beforeDispatch)
          {
            observation.previewElapsed.push_back(update.elapsed);
          }

          observation.trackedPreviewAction = observation.trackedPreviewAction || [slider isTrackingGesture] != NO;

          if (beforeDispatch && replacement != kInvalidTrackId && !observation.transitioned)
          {
            session.play(replacement);
            requireWaitUntil(
              [&]
              {
                auto const& snapshot = session.runtime().playback().snapshot().transport;
                return snapshot.nowPlaying.trackId == expectedTrack &&
                       snapshot.transport == audio::Transport::Playing && snapshot.positionRevision != initialRevision;
              },
              "Replacement must publish while the native slider action is tracking");
            observation.transitioned = true;
            observation.finalSeek = session.runtime().playback().snapshot().transport.finalSeekRevision;
            [playback renderState:session.state() modern:YES];
          }

          if (!beforeDispatch && replacement == kInvalidTrackId)
          {
            requireWaitUntil(
              [&]
              {
                return observation.previewCount > 0 ||
                       session.runtime().playback().snapshot().transport.finalSeekRevision != observation.finalSeek;
              },
              "A continuous native action must settle before tracking returns");
            observation.heldFinalUnchanged =
              observation.heldFinalUnchanged &&
              session.runtime().playback().snapshot().transport.finalSeekRevision == observation.finalSeek;
          }

          if (!beforeDispatch && options.scrub && !observation.scrubbing)
          {
            observation.scrubbing = true;

            for (auto const fraction : std::to_array<double>({0.25, 0.75}))
            {
              slider.doubleValue = fraction;
              [slider sendAction:slider.action to:slider.target];
            }
          }
        }];
      // Generated AppKit events can finish continuous cell tracking in the same
      // turn; witness timeline changes inside its real target/action callback,
      // rather than assuming a later tracking-mode timer gets a turn.
      [NSApp sendEvent:seekMouseEvent(window, NSEventTypeLeftMouseDown, start)];
      [target setSeekObserver:{}];
      AO_INVARIANT(target.seekCount > beforeCount && [slider isTrackingGesture] == NO &&
                     observation.trackedPreviewAction && (replacement == kInvalidTrackId || observation.transitioned),
                   "A native continuous seek action must witness tracking and the requested timeline transition");
      settleNativeCallbacks();
      auto const finalSeek = session.runtime().playback().snapshot().transport.finalSeekRevision;
      AO_INVARIANT(observation.heldFinalUnchanged, "A preview must not commit a final seek during native tracking");
      AO_INVARIANT(replacement == kInvalidTrackId
                     ? finalSeek.value == observation.finalSeek.value + 1 && observation.commitCount == 1
                     : finalSeek == observation.finalSeek && observation.commitCount == 0,
                   "Only a current native gesture may deliver and commit exactly one final seek");
      AO_INVARIANT(replacement == kInvalidTrackId ? observation.previewCount > 0 && observation.previewedWhileTracking
                                                  : observation.previewCount == 0,
                   "A current occurrence must preview during tracking; a replaced occurrence must not preview");

      if (options.scrub)
      {
        auto const optSeekTarget = [slider seekTarget];
        AO_INVARIANT(optSeekTarget, "A scrubbed slider must keep its presented seek target");
        auto const duration = optSeekTarget->duration;
        auto const expectedElapsed = [&](double fraction)
        {
          return std::chrono::milliseconds{static_cast<std::int64_t>(fraction * static_cast<double>(duration.count()))};
        };
        AO_INVARIANT(observation.previewElapsed.size() >= 3 &&
                       std::ranges::contains(observation.previewElapsed, expectedElapsed(0.25)) &&
                       std::ranges::contains(observation.previewElapsed, expectedElapsed(0.75)),
                     "Every continuous drag action must preview its own position");
        AO_INVARIANT(observation.commitCount == 1 &&
                       observation.committedElapsed == expectedElapsed(std::clamp(slider.doubleValue, 0.0, 1.0)),
                     "A scrub must commit once, at the slider's final position");
      }
    }
  } // namespace

  std::int32_t runQualityPresentationScenario(std::filesystem::path const& /*musicRoot*/,
                                              std::filesystem::path const& stateRoot)
  {
    AO_INVARIANT([NSThread isMainThread] != NO && NSApp != nil,
                 "Quality presentation requires an initialized main-thread NSApplication");
    std::println("AppKit quality-presentation: synthetic DesktopViewState, real PlaybackBar; "
                 "no session, runtime, providers, devices, playback, seeking, or actions. "
                 "Captures record native bars, not tooltip popups or shipping triggers. OS={}",
                 NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    auto* const window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 980, 720)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
    window.title = @"Aobus AppKit quality presentation scenario";
    window.releasedWhenClosed = NO;
    window.preventsApplicationTerminationWhenModal = NO;
    auto* const root = [[AobusFlippedView alloc] initWithFrame:window.contentView.bounds];
    root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = root;
    [window center];
    [window makeKeyAndOrderFront:nil];
    settleNativeCallbacks();

    struct LocaleCase final
    {
      std::string_view request;
      std::string_view resolved;
    };

    for (auto const& locale : std::array{LocaleCase{"en-US", "en"}, LocaleCase{"zh-CN", "zh-Hans"}})
    {
      auto catalogRes = i18n::MessageCatalog::create(locale.request);
      AO_INVARIANT(catalogRes, "The quality presentation catalog must load");
      auto const& catalog = *catalogRes;
      auto const localized = [&](i18n::MessageId message)
      {
        auto messageRes = catalog.format(message);
        AO_INVARIANT(messageRes && !messageRes->text.empty(), "Quality presentation copy must resolve");
        AO_INVARIANT(messageRes->locale == locale.resolved,
                     "Quality presentation must use its selected English or CJK catalog, not fallback copy");
        std::println("quality-presentation requested={} message={} resolved={} text={}",
                     locale.request,
                     static_cast<std::uint16_t>(message),
                     messageRes->locale,
                     messageRes->text);
        return std::move(messageRes->text);
      };
      auto const heading = localized(i18n::MessageId::PlaybackAudioPipeline);
      auto const headline = localized(i18n::MessageId::AudioQualityBitPerfectPlayback);
      auto const updatedHeadline = localized(i18n::MessageId::AudioQualitySignalPreserved);
      auto const unknownNote = localized(i18n::MessageId::AudioQualityUnknownChannelLayout);
      auto const unverifiedNote = localized(i18n::MessageId::AudioQualityUnverifiedChannelDelivery);
      auto const outputLabel = localized(i18n::MessageId::AppKitOutputDevice);
      auto const playLabel = localized(i18n::MessageId::PlaybackControlPlay);
      auto const pauseLabel = localized(i18n::MessageId::PlaybackControlPause);
      // These are explicit binding inputs, not a second formatter/confidence policy.
      // Shared formatter and NowPlayingViewModel cases own semantic selection/joining.
      auto const withNote = std::format("{}:\n\n{}\n{}", heading, headline, unknownNote);
      auto const withoutNote = std::format("{}:\n\n{}", heading, headline);
      auto const updated = std::format("{}:\n\n{}\n{}", heading, updatedHeadline, unverifiedNote);
      struct FallbackCase final
      {
        std::string_view name;
        std::string_view text;
        bool playing = false;
      };
      auto const cases = std::array{FallbackCase{"note-present", withNote},
                                    FallbackCase{"note-absent", withoutNote},
                                    FallbackCase{"empty", ""},
                                    FallbackCase{"updated", updated, true},
                                    FallbackCase{"note-restored", withNote}};
      auto* const mirror = [[NSImageView alloc] initWithFrame:NSZeroRect];
      auto* const target = [[AobusQualityPresentationActionTarget alloc] init];
      auto* const playback = [[AobusPlaybackBar alloc] initWithCatalog:catalog
                                                                target:target
                                                       transportAction:@selector(transport:)
                                                            seekAction:@selector(unexpectedAction:)
                                                          volumeAction:@selector(unexpectedAction:)
                                                      showVolumeAction:@selector(unexpectedAction:)
                                                      showOutputAction:@selector(showOutput:)
                                                     showOptionsAction:@selector(unexpectedAction:)
                                                  playingArtworkMirror:mirror];
      auto state = DesktopViewState{};
      state.nowPlaying.title = "Synthetic quality binding input";
      state.soul.aura = uimodel::SoulAura::Radiant;
      state.soul.motionMode = uimodel::AobusSoulMotionMode::Dormant;
      auto& play = state.transport[static_cast<std::size_t>(uimodel::PlaybackCommand::PlayPause)];
      play.enabled = true;
      [playback layoutInRoot:root modern:NO];
      auto* const soul = requireControl<AobusSoulButton>(playback.classicView, @"playback.soul");
      auto const requireClassic = [&]
      {
        AO_INVARIANT(playback.activeView == playback.classicView && playback.classicView.superview == root &&
                       playback.modernView.superview == nil &&
                       requireControl<AobusSoulButton>(playback.classicView, @"playback.soul") == soul,
                     "Classic mode must retain the real shared Soul in its active native bar");
        AO_INVARIANT(utf8(soul.toolTip) == state.nowPlaying.audioPipeline.plainTextFallback,
                     "Classic layout and resize must preserve the exact rendered quality tooltip, including empty");
        AO_INVARIANT(soul.target == target && soul.action == @selector(showOutput:) && soul.enabled != NO &&
                       utf8(soul.accessibilityLabel) == outputLabel,
                     "Classic quality text must not replace the enabled Output Device action or accessibility label");
        AO_INVARIANT(
          soul.window == window && soul.hidden == NO && ::NSContainsRect(playback.activeView.bounds, soul.frame) != NO,
          "The captured classic Soul must be visible and bounded by the realized bar");
      };

      for (auto const& testCase : cases)
      {
        state.nowPlaying.audioPipeline.plainTextFallback = testCase.text;
        play.playing = testCase.playing;
        play.tooltip = testCase.playing ? pauseLabel : playLabel;
        [playback renderState:state modern:NO];
        [playback renderFrameForState:state elapsed:0ms modern:NO];
        requireClassic();

        for (auto const size : std::array{NSMakeSize(980, 720), NSMakeSize(760, 520)})
        {
          [window setContentSize:size];
          // No render between resizing/repeated layout and the regression assertion.
          [playback layoutInRoot:root modern:NO];
          [playback layoutInRoot:root modern:NO];
          [root layoutSubtreeIfNeeded];
          requireClassic();
          AO_INVARIANT(
            std::abs(root.bounds.size.width - size.width) < 1 && std::abs(root.bounds.size.height - size.height) < 1,
            "Quality presentation must exercise the requested normal or constrained client size");
          std::println("quality-presentation locale={} case={} client={}x{} backing-scale={}",
                       locale.request,
                       testCase.name,
                       root.bounds.size.width,
                       root.bounds.size.height,
                       window.backingScaleFactor);
          captureView(playback.classicView,
                      stateRoot / std::format("appkit-quality-{}-{}-{}x{}-classic.png",
                                              locale.request,
                                              testCase.name,
                                              static_cast<std::int32_t>(size.width),
                                              static_cast<std::int32_t>(size.height)));

          [playback layoutInRoot:root modern:YES];
          [playback renderState:state modern:YES];
          [playback renderFrameForState:state elapsed:0ms modern:YES];
          [playback layoutInRoot:root modern:YES];
          [playback layoutInRoot:root modern:YES];
          [root layoutSubtreeIfNeeded];
          AO_INVARIANT(playback.activeView == playback.modernView && playback.modernView.superview == root &&
                         playback.classicView.superview == nil &&
                         requireControl<AobusSoulButton>(playback.modernView, @"playback.soul") == soul &&
                         soul.target == target && soul.action == @selector(transport:) && soul.enabled != NO &&
                         utf8(soul.toolTip) == play.tooltip && utf8(soul.accessibilityLabel) == play.tooltip,
                       "Modern mode must retain transport tooltip, action, and accessibility across repeated layout");
          auto* const output = requireControl<NSButton>(playback.modernView, @"playback.output");
          AO_INVARIANT(output.target == target && output.action == @selector(showOutput:) &&
                         utf8(output.accessibilityLabel) == outputLabel,
                       "Modern mode must retain its separate Output Device action and accessibility label");

          if (testCase.name == "note-present")
          {
            captureView(playback.modernView,
                        stateRoot / std::format("appkit-quality-{}-{}x{}-modern.png",
                                                locale.request,
                                                static_cast<std::int32_t>(size.width),
                                                static_cast<std::int32_t>(size.height)));
          }

          [playback layoutInRoot:root modern:NO];
          [playback renderState:state modern:NO];
          [playback renderFrameForState:state elapsed:0ms modern:NO];
          [playback layoutInRoot:root modern:NO];
          [playback layoutInRoot:root modern:NO];
          [root layoutSubtreeIfNeeded];
          requireClassic();
        }
      }

      [playback detach];
      [playback.activeView removeFromSuperview];
    }

    [window orderOut:nil];
    [window close];
    settleNativeCallbacks();
    return 0;
  }

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

    // The AppKit bar renders no formatter of its own; the shared plain-text
    // fallback is the classic Soul tooltip. Build a truthful unknown-layout
    // fallback from the production formatter and catalog. The join shape is
    // pinned by NowPlayingViewModelTest; the scenario provides it as input.
    auto const qualityFormatter = uimodel::AudioQualityFormatter{session.catalog()};
    auto const unknownLayoutQuality = rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                                       .pipelineQuality = audio::Quality::BitwisePerfect,
                                                       .overall = audio::Quality::BitwisePerfect};
    auto const unknownLayoutPresentation = qualityFormatter.presentation(unknownLayoutQuality);
    auto const verifiedLayoutPresentation =
      qualityFormatter.presentation(rt::QualityState{.sourceQuality = audio::Quality::BitwisePerfect,
                                                     .pipelineQuality = audio::Quality::BitwisePerfect,
                                                     .overall = audio::Quality::BitwisePerfect,
                                                     .channelConfidence = audio::ChannelConfidence::Verified});
    AO_INVARIANT(!unknownLayoutPresentation.headline.empty() && !unknownLayoutPresentation.channelNote.empty() &&
                   verifiedLayoutPresentation.channelNote.empty(),
                 "The production formatter must keep the channel note independent of the numerical headline");
    AO_INVARIANT(uimodel::resolveSoulAura(audio::Transport::Playing, true, unknownLayoutQuality) == state.soul.aura,
                 "Unknown channel confidence must not change the numerical Soul aura");
    auto const qualityFallbackWithNote = [&]
    {
      auto fallback = std::string{i18n::requiredText(session.catalog(), i18n::MessageId::PlaybackAudioPipeline)};
      fallback.append(":\n");
      std::format_to(std::back_inserter(fallback), "\n{}", unknownLayoutPresentation.headline);
      std::format_to(std::back_inserter(fallback), "\n{}", unknownLayoutPresentation.channelNote);
      return fallback;
    }();
    auto const qualityFallbackWithoutNote = [&]
    {
      auto fallback = std::string{i18n::requiredText(session.catalog(), i18n::MessageId::PlaybackAudioPipeline)};
      fallback.append(":\n");
      std::format_to(std::back_inserter(fallback), "\n{}", verifiedLayoutPresentation.headline);
      return fallback;
    }();
    state.nowPlaying.audioPipeline.plainTextFallback = qualityFallbackWithNote;

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
    AO_INVARIANT([soul.toolTip isEqualToString:@"Play this fixture"] != 0,
                 "Modern Soul must keep the transport play tooltip and never project the quality fallback");
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
    // The classic Soul tooltip projects the shared quality fallback verbatim;
    // the note follows the channel confidence and clears with it. The capture
    // records the realized classic bar, not hovered tooltip pixels.
    AO_INVARIANT(ao::appkit::utf8(soul.toolTip) == qualityFallbackWithNote,
                 "Classic Soul tooltip must project the localized quality fallback with its independent note");
    classicState.nowPlaying.audioPipeline.plainTextFallback = qualityFallbackWithoutNote;
    [playback renderState:classicState modern:NO];
    AO_INVARIANT(ao::appkit::utf8(soul.toolTip) == qualityFallbackWithoutNote,
                 "A verified delivery must drop only the channel note from the classic tooltip");
    classicState.nowPlaying.audioPipeline.plainTextFallback.clear();
    [playback renderState:classicState modern:NO];
    AO_INVARIANT(soul.toolTip.length == 0, "An empty shared fallback must clear the classic Soul tooltip");
    classicState.nowPlaying.audioPipeline.plainTextFallback = qualityFallbackWithNote;
    [playback renderState:classicState modern:NO];
    captureView(root, stateRoot / "appkit-playback-classic-quality.png");
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
    exerciseSeekGesture(session, playback, target, window, kInvalidTrackId, {.paused = true});
    exerciseSeekGesture(session, playback, target, window, kInvalidTrackId, {.scrub = true});
    session.runtime().playback().commands().stop();
    settleNativeCallbacks();
    [soul presentState:{.aura = uimodel::SoulAura::Radiant, .motionMode = uimodel::AobusSoulMotionMode::Animating}
               playing:YES
                modern:YES];
    [playback detach];
    soul.needsDisplay = NO;
    [playback settleSoul];
    AO_INVARIANT([playback soulNeedsFrames] == NO && [soul needsFrames] != NO && soul.needsDisplay == NO,
                 "Late frame-stop callbacks must not mutate or schedule redraws on a detached playback bar");
    AO_INVARIANT(modernSeek.target == nil && modernSeek.action == nullptr && classicVolume.target == nil &&
                   classicVolume.action == nullptr,
                 "playback detach must revoke native action targets");
  }
} // namespace ao::appkit::test
