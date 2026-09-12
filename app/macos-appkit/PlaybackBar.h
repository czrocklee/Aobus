// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "DesktopControls.h"
#include "PlaybackSeekTarget.h"

#import <AppKit/AppKit.h>

#include <chrono>
#include <optional>

namespace ao::appkit
{
  struct DesktopViewState;
}

namespace ao::i18n
{
  class MessageCatalog;
}

@interface AobusSeekSlider : AobusSlider
- (void)presentSeekTarget:(std::optional<ao::appkit::PlaybackSeekTarget>)optTarget;
- (std::optional<ao::appkit::PlaybackSeekTarget>)seekTarget;
@end

@interface AobusPlaybackBar : NSObject
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithCatalog:(ao::i18n::MessageCatalog const&)catalog
                         target:(id)target
                transportAction:(SEL)transportAction
                     seekAction:(SEL)seekAction
                   volumeAction:(SEL)volumeAction
               showVolumeAction:(SEL)showVolumeAction
               showOutputAction:(SEL)showOutputAction
              showOptionsAction:(SEL)showOptionsAction
           playingArtworkMirror:(NSImageView*)playingArtworkMirror NS_DESIGNATED_INITIALIZER;
- (NSView*)activeView;
- (NSView*)modernView;
- (NSView*)classicView;
- (CGFloat)heightForModern:(BOOL)modern;
- (void)layoutInRoot:(NSView*)root modern:(BOOL)modern;
- (void)renderState:(ao::appkit::DesktopViewState const&)state modern:(BOOL)modern;
- (void)renderFrameForState:(ao::appkit::DesktopViewState const&)state
                    elapsed:(std::chrono::milliseconds)elapsed
                     modern:(BOOL)modern;
- (void)toggleVolumePopover;
- (void)presentOutputMenu:(NSMenu*)menu;
- (void)presentPlaybackOptionsMenu:(NSMenu*)menu;
- (void)closePopover;
- (void)detach;
@end
