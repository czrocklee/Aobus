// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#import <AppKit/AppKit.h>

@interface AobusSoulButton : NSButton
- (void)presentState:(ao::uimodel::AobusSoulViewState const&)state playing:(BOOL)playing modern:(BOOL)modern;
@end
