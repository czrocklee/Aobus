// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/SoulButton.h"
#include <ao/Contract.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#import <AppKit/AppKit.h>

#include <array>

// Control compositor evidence without depending on window stacking or delays.
@interface AobusSoulOcclusionWindow : NSWindow
@property (nonatomic) NSWindowOcclusionState reportedOcclusionState;
@end

@implementation AobusSoulOcclusionWindow
- (NSWindowOcclusionState)occlusionState
{
  return _reportedOcclusionState;
}
@end

namespace ao::appkit::test
{
  void exerciseSoulOcclusion()
  {
    auto* const window = [[AobusSoulOcclusionWindow alloc] initWithContentRect:NSMakeRect(0, 0, 64, 64)
                                                                     styleMask:NSWindowStyleMaskBorderless
                                                                       backing:NSBackingStoreBuffered
                                                                         defer:NO];
    window.releasedWhenClosed = NO;
    auto* const soul = [[AobusSoulButton alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
    window.contentView = soul;
    [window orderFront:nil];
    AO_INVARIANT(window.visible != NO && NSApp.hidden == NO, "Soul occlusion scenario requires a visible application");

    for (auto const mode : std::to_array({uimodel::AobusSoulMotionMode::Frozen, uimodel::AobusSoulMotionMode::Dormant}))
    {
      window.reportedOcclusionState = NSWindowOcclusionStateVisible;
      auto state = uimodel::AobusSoulViewState{
        .aura = uimodel::SoulAura::Radiant, .motionMode = uimodel::AobusSoulMotionMode::Animating};
      [soul presentState:state playing:YES modern:YES];
      // Timer shutdown settles the previous presentation before its final
      // repaint observes a newly paused or dormant transport and changed aura.
      [soul settleAnimation];
      window.reportedOcclusionState = static_cast<NSWindowOcclusionState>(0);
      state.motionMode = mode;
      state.aura = uimodel::SoulAura::Turbulent;
      [soul presentState:state playing:NO modern:YES];
      AO_INVARIANT([soul needsFrames] == NO, "An occluded final repaint must settle its new motion and aura targets");

      window.reportedOcclusionState = NSWindowOcclusionStateVisible;
      [soul presentState:state playing:NO modern:YES];
      AO_INVARIANT([soul needsFrames] == NO, "Revealing a settled Soul must not replay hidden transitions");
    }

    [window close];
  }
} // namespace ao::appkit::test
