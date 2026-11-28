// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#import <AppKit/AppKit.h>

#include <vector>

namespace ao::appkit
{
  class LibraryBrowserModel;
}

@class AobusLibraryBrowser;

@protocol AobusLibraryBrowserDelegate<NSObject>
- (BOOL)isLibraryBrowserClosing:(AobusLibraryBrowser*)browser;
- (BOOL)isLibraryBrowserSheetBlocked:(AobusLibraryBrowser*)browser;
- (void)libraryBrowserSelectionDidChange:(AobusLibraryBrowser*)browser;
- (BOOL)libraryBrowser:(AobusLibraryBrowser*)browser
  requestMembershipForTracks:(std::vector<ao::TrackId> const&)trackIds
                      listId:(ao::ListId)listId;
@end

@interface AobusLibraryBrowser
  : NSObject<NSTableViewDataSource, NSTableViewDelegate, NSOutlineViewDataSource, NSOutlineViewDelegate>
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithModel:(ao::appkit::LibraryBrowserModel&)model
                     delegate:(id<AobusLibraryBrowserDelegate>)delegate NS_DESIGNATED_INITIALIZER;
- (NSTableView*)tracks;
- (NSOutlineView*)lists;
- (NSScrollView*)trackScroll;
- (NSScrollView*)listScroll;
// Detach while the borrowed model is alive; later native callbacks return neutral results.
- (void)detach;
- (void)invalidateProjection;
- (void)refresh;
- (void)layoutForModern:(BOOL)modern browserWidth:(CGFloat)browserWidth;
- (BOOL)prepareContextMenu:(NSMenu*)menu;
- (void)playSelectedTrack;
- (ao::ListId)activeListId;
// The clicked context is independent of the selected List; headings and empty space have no List ID.
- (ao::ListId)clickedListId;
- (NSString*)activeListTitle;
- (std::vector<ao::TrackId>)selectedTrackIds;
@end
