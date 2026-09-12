// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "LibraryEditorModel.h"

#import <AppKit/AppKit.h>

@interface AobusLibraryEditor : NSObject<NSTextFieldDelegate, NSTokenFieldDelegate>
- (instancetype)initWithModel:(ao::appkit::LibraryEditorModel&)model
                       parent:(NSWindow*)parent
                       modern:(BOOL)modern
                      artwork:(NSImage*)artwork;
- (NSWindow*)window;
- (void)present;
- (void)refresh;
- (BOOL)requestClose;
// A lifecycle request waits through saving/menu tracking and joins an existing
// discard confirmation. Only the user's Keep choice completes with NO.
- (void)requestCloseWithCompletion:(void (^)(BOOL closed))completion;
- (void)finish;
- (void)save:(id)sender;
- (void)clearField:(NSMenuItem*)sender;
- (void)fieldActions:(NSButton*)sender;
- (void)toggleSection:(NSButton*)sender;
- (void)cancel:(id)sender;
@end
