// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#import <AppKit/AppKit.h>

namespace ao::appkit
{
  class LibraryEditorModel;
}
namespace ao::rt
{
  class CompletionService;
}

// Inline child of Properties. The parent owns the model and vocabulary and
// detaches this surface before cancelling the authoring session.
@interface AobusTrackCreditsEditor : NSViewController<NSTextFieldDelegate>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithNibName:(NSNibName)nibNameOrNil bundle:(NSBundle*)nibBundleOrNil NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;
- (instancetype)initWithModel:(ao::appkit::LibraryEditorModel&)model
                   completion:(ao::rt::CompletionService&)completion
                      changed:(void (^)(void))changed NS_DESIGNATED_INITIALIZER;
- (void)refreshWithBlocked:(BOOL)blocked;
- (void)detach;
@end
