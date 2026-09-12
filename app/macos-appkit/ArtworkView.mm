// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ArtworkView.h"

#include <algorithm>

namespace
{
  constexpr auto kCornerRadius = 8;
  constexpr auto kSymbolScale = 0.3;
  constexpr auto kMaximumSymbolSize = 48;
} // namespace

@implementation AobusArtworkView
- (void)drawRect:(NSRect) [[maybe_unused]] dirtyRect
{
  [NSGraphicsContext saveGraphicsState];
  auto* const outline = [NSBezierPath bezierPathWithRoundedRect:self.bounds
                                                        xRadius:kCornerRadius
                                                        yRadius:kCornerRadius];
  [NSColor.controlBackgroundColor setFill];
  [outline fill];
  [outline addClip];

  if (self.image != nil)
  {
    [super drawRect:self.bounds];
  }
  else
  {
    auto const size = std::min(static_cast<CGFloat>(kMaximumSymbolSize), self.bounds.size.width * kSymbolScale);
    auto* symbol = [NSImage imageWithSystemSymbolName:@"music.note" accessibilityDescription:nil];
    symbol = [symbol imageWithSymbolConfiguration:[NSImageSymbolConfiguration
                                                    configurationWithHierarchicalColor:NSColor.secondaryLabelColor]];
    auto const rect = NSMakeRect((self.bounds.size.width - size) / 2, (self.bounds.size.height - size) / 2, size, size);
    [symbol drawInRect:rect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
  }

  [NSGraphicsContext restoreGraphicsState];
}
@end
