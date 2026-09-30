// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#import <AppKit/AppKit.h>

@interface AobusSoulButton : NSButton
- (void)presentState:(ao::uimodel::AobusSoulViewState const&)state playing:(BOOL)playing modern:(BOOL)modern;
/// True while motion runs or a pause coast or aura cross-fade still needs frames.
- (BOOL)needsFrames;
/// Lands pending transitions at once when frame delivery stops.
- (void)settleAnimation;
@end
