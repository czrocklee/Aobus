// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#import <AppKit/AppKit.h>

#include <cstddef>
#include <optional>
#include <vector>

using AobusInspectorAction = void (^)(void);

// Presentation only: all supplied model values are consumed synchronously.
@interface AobusTrackInspector : NSViewController
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithNibName:(NSNibName)nibNameOrNil bundle:(NSBundle*)nibBundleOrNil NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;
- (instancetype)initWithCatalog:(ao::i18n::MessageCatalog)catalog
                  revealHandler:(AobusInspectorAction)revealHandler
              propertiesHandler:(AobusInspectorAction)propertiesHandler
                 dismissHandler:(AobusInspectorAction)dismissHandler NS_DESIGNATED_INITIALIZER;
@property (nonatomic, readonly) NSPanel* sheet;
- (void)renderSelectionCount:(std::size_t)count
                         row:(std::optional<ao::rt::TrackRow> const&)row
                     credits:(ao::uimodel::TrackCreditSections const&)credits
                   canReveal:(BOOL)canReveal;
- (void)renderArtwork:(ao::rt::ResourceBytes const&)bytes;
- (void)layoutForModern:(BOOL)modern;
- (void)presentSheetForWindow:(NSWindow*)window;
- (void)dismissSheet;
- (void)detach;
@end
