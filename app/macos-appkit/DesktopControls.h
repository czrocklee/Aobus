// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#import <AppKit/AppKit.h>

@interface AobusSurface : NSVisualEffectView
@end

@protocol AobusLibraryDropDestination
- (NSDragOperation)validateLibraryDrop:(id<NSDraggingInfo>)sender;
- (BOOL)performLibraryDrop:(id<NSDraggingInfo>)sender;
@end

@interface AobusRootSurface : AobusSurface
@property (nonatomic, weak) id<AobusLibraryDropDestination> libraryDropDestination;
@end

@interface AobusFlippedView : NSView
@end

@interface AobusSlider : NSSlider
- (BOOL)isTrackingGesture;
@end

namespace ao::appkit
{
  NSButton* symbolButton(NSString* symbol, NSString* title, id target, SEL action);
  NSViewController* viewController(NSView* view);
} // namespace ao::appkit
