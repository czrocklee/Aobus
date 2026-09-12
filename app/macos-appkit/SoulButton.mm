// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "SoulButton.h"

#include <ao/Contract.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace
{
  constexpr auto kMaximumAnimationStepSeconds = 0.1;
  constexpr auto kChannelMaximum = 255.0;
  constexpr auto kPlayLeadingFraction = 0.22;
  constexpr auto kPlayHalfHeightFraction = 0.36;
  constexpr auto kPlayTipFraction = 0.40;
  constexpr auto kPauseLeadingFraction = 0.26;
  constexpr auto kPauseHalfHeightFraction = 0.34;
  constexpr auto kPauseBarWidthFraction = 0.14;
  constexpr auto kPauseBarHeightFraction = 0.68;
  constexpr auto kPauseTrailingFraction = 0.12;
} // namespace

@implementation AobusSoulButton {
  ao::uimodel::AobusSoulViewState _soulState;
  ao::uimodel::AobusSoulAnimationState _animation;
  std::chrono::steady_clock::time_point _lastFrameTime;
  BOOL _playing;
  BOOL _modern;
  BOOL _hasPresentation;
}
- (void)presentState:(ao::uimodel::AobusSoulViewState const&)state playing:(BOOL)playing modern:(BOOL)modern
{
  auto const previousVisual = _animation.visualFrame(ao::uimodel::aobusSoulAuraRgb(_soulState.aura));
  auto const previousPlaying = _playing;
  auto const previousModern = _modern;
  _soulState = state;
  _playing = playing;
  _modern = modern;
  _animation.setMotionMode(state.motionMode);
  auto const now = std::chrono::steady_clock::now();
  auto const animate =
    (NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion == NO) && (NSApp.hidden == NO) &&
    ao::uimodel::shouldAnimateAobusSoul(state.motionMode, self.window.visible != NO, self.window.miniaturized != NO);
  bool advanced = false;

  if (animate)
  {
    if (_lastFrameTime != std::chrono::steady_clock::time_point{})
    {
      auto const previousElapsed = _animation.elapsed();
      _animation.advance(std::min(std::chrono::duration<double>{now - _lastFrameTime},
                                  std::chrono::duration<double>{kMaximumAnimationStepSeconds}));
      advanced = _animation.elapsed() != previousElapsed;
    }

    _lastFrameTime = now;
  }
  else
  {
    _lastFrameTime = {};
  }

  auto const visual = _animation.visualFrame(ao::uimodel::aobusSoulAuraRgb(state.aura));

  if ((_hasPresentation == NO) || visual != previousVisual || playing != previousPlaying || modern != previousModern ||
      advanced)
  {
    self.needsDisplay = YES;
  }

  _hasPresentation = YES;
}

- (void)drawRect:(NSRect) [[maybe_unused]] dirtyRect
{
  auto const bounds = self.bounds;
  auto const center = NSPoint{.x = NSMidX(bounds), .y = NSMidY(bounds)};
  auto const visual = _animation.visualFrame(ao::uimodel::aobusSoulAuraRgb(_soulState.aura));
  auto const& geometry = ao::uimodel::kAobusSoulGeometry;
  auto const expandedStroke = geometry.baseStrokeWidth * ao::uimodel::kAobusSoulGoldenRatio;
  auto const available = std::min(bounds.size.width, bounds.size.height);
  auto const scale = available / ((geometry.radius + (expandedStroke / 2.0)) * 2.0);
  auto const radius = geometry.radius * scale;
  auto const diameter = radius * 2.0;
  auto const baseStroke = geometry.baseStrokeWidth * scale;
  auto const stroke = baseStroke + (((expandedStroke * scale) - baseStroke) * visual.motion.breath);
  auto const core = visual.gradientColors.core;
  auto const body = visual.gradientColors.body;
  auto const alpha = visual.motion.luminance;
  auto const components = std::array<CGFloat, 12>{static_cast<CGFloat>(core.red / kChannelMaximum),
                                                  static_cast<CGFloat>(core.green / kChannelMaximum),
                                                  static_cast<CGFloat>(core.blue / kChannelMaximum),
                                                  static_cast<CGFloat>(alpha),
                                                  static_cast<CGFloat>(body.red / kChannelMaximum),
                                                  static_cast<CGFloat>(body.green / kChannelMaximum),
                                                  static_cast<CGFloat>(body.blue / kChannelMaximum),
                                                  static_cast<CGFloat>(alpha),
                                                  static_cast<CGFloat>(body.red / kChannelMaximum),
                                                  static_cast<CGFloat>(body.green / kChannelMaximum),
                                                  static_cast<CGFloat>(body.blue / kChannelMaximum),
                                                  static_cast<CGFloat>(alpha)};
  auto const locations = std::array<CGFloat, 3>{0, static_cast<CGFloat>(ao::uimodel::kAobusSoulCoreGradientStop), 1};
  auto* const context = NSGraphicsContext.currentContext.CGContext;
  AO_INVARIANT(context != nullptr, "Soul drawing requires a graphics context");
  auto* const colorSpace = ::CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  AO_INVARIANT(colorSpace != nullptr, "Soul drawing requires an sRGB color space");
  auto* const gradient =
    ::CGGradientCreateWithColorComponents(colorSpace, components.data(), locations.data(), locations.size());
  AO_INVARIANT(gradient != nullptr, "Soul drawing requires a linear gradient");
  ::CGContextSaveGState(context);
  ::CGContextAddEllipseInRect(context, CGRectMake(center.x - radius, center.y - radius, diameter, diameter));
  ::CGContextSetLineWidth(context, stroke);
  ::CGContextReplacePathWithStrokedPath(context);
  ::CGContextClip(context);
  auto const diagonal = radius;
  auto const rotation = -visual.motion.rotationRadians;
  auto const startOffsetX = diagonal * (std::cos(rotation) - std::sin(rotation));
  auto const startOffsetY = -diagonal * (std::sin(rotation) + std::cos(rotation));
  auto const start = CGPoint{.x = center.x + startOffsetX, .y = center.y + startOffsetY};
  auto const end = CGPoint{.x = center.x - startOffsetX, .y = center.y - startOffsetY};
  ::CGContextDrawLinearGradient(context, gradient, start, end, static_cast<CGGradientDrawingOptions>(0));
  ::CGContextRestoreGState(context);
  ::CGGradientRelease(gradient);
  ::CGColorSpaceRelease(colorSpace);

  [NSColor.labelColor setFill];
  auto* const glyph = [NSBezierPath bezierPath];

  if (_modern == NO)
  {
    // Classic uses Soul as an output-quality indicator, without a transport glyph.
  }
  else if (_playing == NO)
  {
    [glyph moveToPoint:NSMakePoint(
                         center.x - (radius * kPlayLeadingFraction), center.y - (radius * kPlayHalfHeightFraction))];
    [glyph lineToPoint:NSMakePoint(center.x + (radius * kPlayTipFraction), center.y)];
    [glyph lineToPoint:NSMakePoint(
                         center.x - (radius * kPlayLeadingFraction), center.y + (radius * kPlayHalfHeightFraction))];
    [glyph closePath];
  }
  else
  {
    [glyph appendBezierPathWithRoundedRect:NSMakeRect(center.x - (radius * kPauseLeadingFraction),
                                                      center.y - (radius * kPauseHalfHeightFraction),
                                                      radius * kPauseBarWidthFraction,
                                                      radius * kPauseBarHeightFraction)
                                   xRadius:1
                                   yRadius:1];
    [glyph appendBezierPathWithRoundedRect:NSMakeRect(center.x + (radius * kPauseTrailingFraction),
                                                      center.y - (radius * kPauseHalfHeightFraction),
                                                      radius * kPauseBarWidthFraction,
                                                      radius * kPauseBarHeightFraction)
                                   xRadius:1
                                   yRadius:1];
  }

  [glyph fill];

  if (self.window.firstResponder == self)
  {
    [NSColor.keyboardFocusIndicatorColor setStroke];
    auto* const focus = [NSBezierPath bezierPathWithOvalInRect:(::NSInsetRect(bounds, 1, 1))];
    focus.lineWidth = 2;
    [focus stroke];
  }
}
@end
