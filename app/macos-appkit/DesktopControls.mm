// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DesktopControls.h"

namespace ao::appkit
{
  NSButton* symbolButton(NSString* symbol, NSString* title, id target, SEL action)
  {
    auto* const image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
    auto* const button = image != nil ? [NSButton buttonWithImage:image target:target action:action]
                                      : [NSButton buttonWithTitle:title target:target action:action];
    button.bordered = NO;
    button.toolTip = title;
    button.accessibilityLabel = title;
    return button;
  }

  NSViewController* viewController(NSView* view)
  {
    auto* const controller = [[NSViewController alloc] init];
    controller.view = view;
    return controller;
  }
} // namespace ao::appkit

@implementation AobusSurface
- (BOOL)isFlipped
{
  return YES;
}
@end

@implementation AobusRootSurface
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
  return [self.libraryDropDestination validateLibraryDrop:sender];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
  return [self.libraryDropDestination validateLibraryDrop:sender];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
  return [self.libraryDropDestination performLibraryDrop:sender];
}
@end

@implementation AobusFlippedView
- (BOOL)isFlipped
{
  return YES;
}
@end

@implementation AobusSlider {
  BOOL _trackingGesture;
}
- (void)mouseDown:(NSEvent*)event
{
  _trackingGesture = YES;
  [super mouseDown:event];
  _trackingGesture = NO;
}
- (BOOL)isTrackingGesture
{
  return _trackingGesture;
}
@end
