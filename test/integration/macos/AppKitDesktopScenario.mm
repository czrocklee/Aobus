// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitDesktopScenarioHelpers.h"
#include "AppKitScenarioSupport.h"
#include <ao/Contract.h>

#include <CoreFoundation/CFRunLoop.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace
{
  constexpr auto kPollIntervalSeconds = 0.05;
  constexpr auto kScenarioTimeout = std::chrono::seconds{90};
  constexpr double kSeekFraction = 0.25;
  constexpr double kVolume = 0.35;
  constexpr double kControlTolerance = 0.02;
  constexpr double kMinimumPlaybackProgress = 0.005;

  enum class Stage : std::uint8_t
  {
    WaitForWindow,
    WaitForLibrary,
    SelectAlbums,
    WaitForAlbums,
    SelectTrack,
    WaitForSelection,
    PlayTrack,
    WaitForPlayback,
    WaitForPlaybackProgress,
    OccludeWindow,
    WaitForOcclusion,
    VerifyOccludedRendering,
    RevealWindow,
    WaitForRenderingResume,
    PausePlayback,
    WaitForPause,
    Seek,
    WaitForSeek,
    ShowVolume,
    SetVolume,
    WaitForVolume,
    SortTitle,
    WaitForTitleSort,
    ModernSearch,
    SelectClassic,
    WaitForClassic,
    ClassicSearch,
    SelectModern,
    WaitForModern,
    ResizeForInspector,
    ShowCompactInspector,
    WaitForCompactInspector,
    DismissCompactInspector,
    RestoreWindowSize,
    CloseWindow,
    WaitForClosedWindow,
    ReopenWindow,
    WaitForReopenedWindow,
    OpenDirtyEditor,
    EditForQuitCancellation,
    WaitForQuitConfirmation,
    VerifyQuitCancellation,
    VerifySwitchUnavailable,
    CancelDirtyEditor,
    WaitForDiscardConfirmation,
    WaitForEditorClose,
    SwitchWhileScanning,
    EditSuccessorForQuit,
    JoinSuccessorQuit,
    DiscardSuccessorAfterQuit,
    Complete,
  };

  using namespace ao::appkit::test::desktop;
} // namespace

@interface AobusDesktopScenarioDriver : NSObject {
  ao::appkit::DesktopLaunch _launch;
  std::filesystem::path _successorRoot;
  NSTimer* _timer;
  NSWindow* _window;
  NSWindow* _occluder;
  NSTableView* _table;
  NSString* _selectedIdentity;
  NSString* _idleTransportText;
  NSString* _playingTransportText;
  NSString* _volumeDescription;
  NSString* _initialPlayingElapsed;
  NSString* _occludedElapsed;
  NSString* _unsortedFirstIdentity;
  double _initialPlayingFraction;
  NSTextView* _searchEditor;
  NSSize _initialContentSize;
  std::chrono::steady_clock::time_point _deadline;
  std::chrono::steady_clock::time_point _occlusionObservationDeadline;
  Stage _stage;
  std::int32_t _searchStep;
  std::int32_t _settleTicks;
  BOOL _successor;
  BOOL _completed;
}
- (instancetype)initWithLaunch:(ao::appkit::DesktopLaunch const&)launch successor:(BOOL)successor;
- (void)start;
- (void)stop;
- (BOOL)completed;
@end

@implementation AobusDesktopScenarioDriver
- (instancetype)initWithLaunch:(ao::appkit::DesktopLaunch const&)launch successor:(BOOL)successor
{
  self = [super init];

  if (self != nil)
  {
    _launch = launch;
    _successorRoot = launch.stateRoot / "successor-library";
    _successor = successor;
    _stage = Stage::WaitForWindow;
    _deadline = std::chrono::steady_clock::now() + kScenarioTimeout;
  }

  return self;
}

- (void)start
{
  _timer = [NSTimer timerWithTimeInterval:kPollIntervalSeconds
                                   target:self
                                 selector:@selector(tick:)
                                 userInfo:nil
                                  repeats:YES];
  [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)stop
{
  [_timer invalidate];
  _timer = nil;
}

- (BOOL)completed
{
  return _completed;
}

- (void)requestTermination
{
  ::CFRunLoopPerformBlock(::CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ [NSApp terminate:nil]; });
  ::CFRunLoopWakeUp(::CFRunLoopGetMain());
}

- (NSControl*)control:(NSString*)identifier
{
  return ao::appkit::test::findControl(_window.contentView, identifier);
}

- (NSControl*)controlInAnyWindow:(NSString*)identifier
{
  auto* const windows = NSApp.windows;

  for (NSUInteger index = 0; index < windows.count; ++index)
  {
    auto* const window = windows[index];

    if (auto* const control = ao::appkit::test::findControl(window.contentView, identifier); control != nil)
    {
      return control;
    }
  }

  return nil;
}

- (NSMenuItem*)menuItem:(NSString*)selector
{
  return findMenuItem(NSApp.mainMenu, ::NSSelectorFromString(selector));
}

- (NSMenuItem*)modeItem:(NSInteger)tag
{
  return findMenuItem(NSApp.mainMenu, ::NSSelectorFromString(@"selectMode:"), true, tag);
}

- (NSMenuItem*)successorItem
{
  return findMenuItem(
    NSApp.mainMenu, ::NSSelectorFromString(@"openRecentLibrary:"), false, 0, nativePath(_successorRoot));
}

- (void)activateControl:(NSControl*)control obligation:(NSString*)obligation
{
  AO_INVARIANT(
    control != nil && control.action != nullptr, "Missing native control action for {}", obligation.UTF8String);
  AO_INVARIANT(control.enabled != 0, "Native control is disabled for {}", obligation.UTF8String);
  auto const sent = [NSApp sendAction:control.action to:control.target from:control];
  AO_INVARIANT(sent != 0, "Native control action failed for {}", obligation.UTF8String);
}

- (BOOL)driveSearch
{
  switch (_searchStep)
  {
    case 0:
      activateMenuItem([self menuItem:@"focusSearch:"], @"focus search");
      _searchStep = 1;
      return NO;
    case 1:
    {
      auto* const field = findSearchField(_window);

      if (field == nil || field.currentEditor == nil)
      {
        return NO;
      }

      [field selectText:nil];
      _searchEditor = static_cast<NSTextView*>(field.currentEditor);
      AO_INVARIANT(_searchEditor != nil, "Search must use the native field editor");
      [_searchEditor insertText:@"abcd" replacementRange:(::NSMakeRange(0, _searchEditor.string.length))];
      _searchEditor.selectedRange = ::NSMakeRange(2, 0);
      [_searchEditor insertText:@"x" replacementRange:_searchEditor.selectedRange];
      [self activateControl:field obligation:@"filter tracks"];
      _searchStep = 2;
      return NO;
    }
    case 2:
    {
      auto* const field = findSearchField(_window);

      if (_table.numberOfRows != 0)
      {
        return NO;
      }

      AO_INVARIANT(field.currentEditor == _searchEditor && [_searchEditor.string isEqual:@"abxcd"] != 0,
                   "Filtering must retain the active native field editor");
      AO_INVARIANT(::NSEqualRanges(_searchEditor.selectedRange, ::NSMakeRange(3, 0)),
                   "Filtering must retain the mid-string caret");
      [_searchEditor insertText:@"y" replacementRange:_searchEditor.selectedRange];
      [self activateControl:field obligation:@"mid-string filter update"];
      _settleTicks = 0;
      _searchStep = 3;
      return NO;
    }
    case 3:
    {
      if (++_settleTicks < 2)
      {
        return NO;
      }

      auto* const field = findSearchField(_window);
      AO_INVARIANT(field.currentEditor == _searchEditor && [_searchEditor.string isEqual:@"abxycd"] != 0,
                   "The second filter update must keep the same field editor");
      AO_INVARIANT(::NSEqualRanges(_searchEditor.selectedRange, ::NSMakeRange(4, 0)),
                   "The second filter update must preserve the mid-string caret");
      [_searchEditor insertText:@"" replacementRange:(::NSMakeRange(0, _searchEditor.string.length))];
      [self activateControl:field obligation:@"clear filter"];
      [_window makeFirstResponder:_table];
      _searchStep = 4;
      return NO;
    }
    case 4:
      if (selectableRowCount(_table) < 3 || _table.selectedRow < 0)
      {
        return NO;
      }

      AO_INVARIANT([rowIdentity(_table, _table.selectedRow) isEqual:_selectedIdentity] != NO,
                   "Filtering must preserve the selected track: expected {}, observed {}",
                   _selectedIdentity.UTF8String,
                   rowIdentity(_table, _table.selectedRow).UTF8String);
      _searchStep = 0;
      _searchEditor = nil;
      return YES;
    default: AO_INVARIANT(false, "Unknown native search stage");
  }
}

- (void)runSuccessor
{
  if (_stage == Stage::EditSuccessorForQuit)
  {
    if (_window.attachedSheet != nil)
    {
      ao::appkit::test::editText(_window.attachedSheet, @"Name", @"Discard before quitting successor");
      auto* const cancel = findActionControl(_window.attachedSheet.contentView, ::NSSelectorFromString(@"cancel:"));
      [self activateControl:cancel obligation:@"open discard confirmation before Quit"];
      _stage = Stage::JoinSuccessorQuit;
    }

    return;
  }

  if (_stage == Stage::JoinSuccessorQuit)
  {
    if (_window.attachedSheet.attachedSheet != nil)
    {
      [self requestTermination];
      _stage = Stage::DiscardSuccessorAfterQuit;
    }

    return;
  }

  if (_stage == Stage::DiscardSuccessorAfterQuit)
  {
    auto* const editor = _window.attachedSheet;
    AO_INVARIANT(editor.attachedSheet != nil, "Quit must join the already-open discard confirmation");
    _completed = YES;
    _stage = Stage::Complete;
    [editor endSheet:editor.attachedSheet returnCode:NSAlertSecondButtonReturn];
    return;
  }

  auto* const seek = static_cast<NSSlider*>([self control:@"playback.seek"]);
  auto* const soul = [self control:@"playback.soul"];

  if (selectableRowCount(_table) < 3 || seek == nil || soul == nil || seek.accessibilityValueDescription.length == 0 ||
      !hasSavedLibrary(_launch.stateRoot, _launch.optRequest->libraryRoot))
  {
    return;
  }

  AO_INVARIANT(seek.enabled == 0 && std::abs(seek.doubleValue) < kControlTolerance,
               "The successor must start with idle native seek state");
  AO_INVARIANT([seek.accessibilityValueDescription isEqual:@"0:00 of 0:00"] != 0,
               "The successor seek control must expose its idle accessibility value");
  auto* const title = static_cast<NSTextField*>([self control:@"playback.title"]);
  auto* const artist = static_cast<NSTextField*>([self control:@"playback.artist"]);
  AO_INVARIANT([soul.toolTip isEqual:@"Play"] != 0 && [title.stringValue isEqual:@"Not Playing"] != 0 &&
                 artist != nil && artist.stringValue.length == 0,
               "The successor must clear the previous now-playing presentation and show idle transport");
  activateMenuItem([self menuItem:@"newList:"], @"open successor list editor");
  _stage = Stage::EditSuccessorForQuit;
}

- (void)tick:(NSTimer*) [[maybe_unused]] timer
{
  @autoreleasepool
  {
    AO_INVARIANT(std::chrono::steady_clock::now() <= _deadline,
                 "Desktop scenario timed out at stage {}",
                 static_cast<std::int32_t>(_stage));

    if (_stage == Stage::Complete)
    {
      return;
    }

    if (_window == nil)
    {
      _window = findDesktopWindow();

      if (_window == nil)
      {
        return;
      }

      AO_INVARIANT(_window.restorable == NO, "The production window must not participate in AppKit restoration");
      _initialContentSize = _window.contentView.bounds.size;
      _table = findTrackTable(_window.contentView);
      AO_INVARIANT(_table != nil, "The production window must expose its native track table");
      _stage = Stage::WaitForLibrary;
    }

    if (_successor != NO)
    {
      [self runSuccessor];
      return;
    }

    switch (_stage)
    {
      case Stage::WaitForLibrary:
      {
        if (auto* const soul = [self control:@"playback.soul"];
            selectableRowCount(_table) >= 3 && soul.enabled != NO &&
            hasSavedLibrary(_launch.stateRoot, _launch.optRequest->libraryRoot))
        {
          _idleTransportText = [soul.toolTip copy];
          _stage = Stage::SelectAlbums;
        }

        break;
      }
      case Stage::SelectAlbums:
      {
        auto* const control = findActionControl(_window.contentView, ::NSSelectorFromString(@"selectPresentation:"));
        AO_INVARIANT([control isKindOfClass:NSPopUpButton.class] != 0,
                     "The production window must expose its native presentation control");
        auto* const presentation = static_cast<NSPopUpButton*>(control);
        AO_INVARIANT(presentation.numberOfItems == 4, "The native presentation control must expose four choices");
        auto const rowCount = _table.numberOfRows;
        [presentation selectItem:nil];
        AO_INVARIANT(
          presentation.indexOfSelectedItem == -1, "The native control must expose its no-selection sentinel");
        [self activateControl:presentation obligation:@"ignore an absent presentation selection"];
        AO_INVARIANT(presentation.indexOfSelectedItem == -1 && _table.numberOfRows == rowCount,
                     "An absent presentation selection must leave browsing unchanged");
        [presentation addItemWithTitle:@"Unsupported scenario presentation"];
        [presentation selectItemAtIndex:4];
        [self activateControl:presentation obligation:@"ignore an unsupported presentation selection"];
        AO_INVARIANT(presentation.indexOfSelectedItem == 4 && _table.numberOfRows == rowCount,
                     "An unsupported presentation selection must leave browsing unchanged");
        [presentation removeItemAtIndex:4];
        [presentation selectItemAtIndex:0];
        [self activateControl:presentation obligation:@"select Albums presentation"];
        _stage = Stage::WaitForAlbums;
        break;
      }
      case Stage::WaitForAlbums:
        if (selectableRowCount(_table) >= 3 && _table.numberOfRows > selectableRowCount(_table))
        {
          _stage = Stage::SelectTrack;
        }

        break;
      case Stage::SelectTrack:
      {
        auto const row = firstSelectableRow(_table);
        AO_INVARIANT(row >= 0, "A scanned native track row must be selectable");
        [_table scrollRowToVisible:row];
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
        _selectedIdentity = [rowIdentity(_table, row) copy];
        AO_INVARIANT(_selectedIdentity.length > 0, "The selected native row must render track identity");
        _stage = Stage::WaitForSelection;
        break;
      }
      case Stage::WaitForSelection:
        if (_table.selectedRow >= 0 && [rowIdentity(_table, _table.selectedRow) isEqual:_selectedIdentity] != NO)
        {
          _stage = Stage::PlayTrack;
        }

        break;
      case Stage::PlayTrack:
      {
        auto* const menu = _table.menu;
        auto const delegate = menu.delegate;
        AO_INVARIANT([delegate respondsToSelector:@selector(menuNeedsUpdate:)] != 0,
                     "The native track menu must have a public update delegate");
        [delegate menuNeedsUpdate:menu];
        auto* const play = findMenuItem(menu, ::NSSelectorFromString(@"playSelection:"));
        activateMenuItem(play, @"play selected track");
        _stage = Stage::WaitForPlayback;
        break;
      }
      case Stage::WaitForPlayback:
      {
        auto* const soul = [self control:@"playback.soul"];
        auto* const seek = static_cast<NSSlider*>([self control:@"playback.seek"]);
        auto* const title = static_cast<NSTextField*>([self control:@"playback.title"]);

        if (seek.enabled != NO && title != nil && [title.stringValue isEqual:@"Not Playing"] == NO &&
            [soul.toolTip isEqual:_idleTransportText] == NO)
        {
          _playingTransportText = [soul.toolTip copy];
          _initialPlayingElapsed = [static_cast<NSTextField*>([self control:@"playback.elapsed"]).stringValue copy];
          _initialPlayingFraction = seek.doubleValue;
          _stage = Stage::WaitForPlaybackProgress;
        }

        break;
      }
      case Stage::WaitForPlaybackProgress:
      {
        auto* const seek = static_cast<NSSlider*>([self control:@"playback.seek"]);
        auto* const elapsed = static_cast<NSTextField*>([self control:@"playback.elapsed"]);

        if (auto* const soul = [self control:@"playback.soul"];
            seek.doubleValue > _initialPlayingFraction + kMinimumPlaybackProgress &&
            [elapsed.stringValue isEqualToString:_initialPlayingElapsed] == NO &&
            [soul.toolTip isEqualToString:_playingTransportText] != NO)
        {
          _stage = Stage::OccludeWindow;
        }

        break;
      }
      case Stage::OccludeWindow:
        _occluder = [[NSWindow alloc] initWithContentRect:_window.frame
                                                styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        _occluder.releasedWhenClosed = NO;
        _occluder.backgroundColor = NSColor.blackColor;
        _occluder.opaque = YES;
        _occluder.level = NSFloatingWindowLevel;
        [_occluder orderFrontRegardless];
        _stage = Stage::WaitForOcclusion;
        break;
      case Stage::WaitForOcclusion:
        if ((_window.occlusionState & NSWindowOcclusionStateVisible) == 0)
        {
          _occludedElapsed = [static_cast<NSTextField*>([self control:@"playback.elapsed"]).stringValue copy];
          _occlusionObservationDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1250};
          _stage = Stage::VerifyOccludedRendering;
        }

        break;
      case Stage::VerifyOccludedRendering:
        if (std::chrono::steady_clock::now() >= _occlusionObservationDeadline)
        {
          auto* const elapsed = static_cast<NSTextField*>([self control:@"playback.elapsed"]);
          AO_INVARIANT([elapsed.stringValue isEqualToString:_occludedElapsed] != 0,
                       "A fully occluded window must stop rendering playback frames");
          _stage = Stage::RevealWindow;
        }

        break;
      case Stage::RevealWindow:
        [_occluder orderOut:nil];
        _stage = Stage::WaitForRenderingResume;
        break;
      case Stage::WaitForRenderingResume:
        if ((_window.occlusionState & NSWindowOcclusionStateVisible) != 0)
        {
          auto* const elapsed = static_cast<NSTextField*>([self control:@"playback.elapsed"]);

          if ([elapsed.stringValue isEqualToString:_occludedElapsed] == NO)
          {
            [_occluder close];
            _occluder = nil;
            _stage = Stage::PausePlayback;
          }
        }

        break;
      case Stage::PausePlayback:
        [self activateControl:[self control:@"playback.soul"] obligation:@"pause playback"];
        _stage = Stage::WaitForPause;
        break;
      case Stage::WaitForPause:
      {
        if (auto* const soul = [self control:@"playback.soul"]; [soul.toolTip isEqual:_playingTransportText] == NO)
        {
          _stage = Stage::Seek;
        }

        break;
      }
      case Stage::Seek:
      {
        auto* const seek = static_cast<NSSlider*>([self control:@"playback.seek"]);
        AO_INVARIANT(seek.enabled != NO, "The paused playable track must remain seekable");
        seek.doubleValue = kSeekFraction;
        [self activateControl:seek obligation:@"seek playback"];
        // The next projection must restore the accepted position from playback,
        // rather than accepting the value this driver just wrote as readback.
        seek.doubleValue = 0.5;
        _stage = Stage::WaitForSeek;
        break;
      }
      case Stage::WaitForSeek:
      {
        auto* const seek = static_cast<NSSlider*>([self control:@"playback.seek"]);
        auto* const elapsed = static_cast<NSTextField*>([self control:@"playback.elapsed"]);
        auto* const duration = static_cast<NSTextField*>([self control:@"playback.duration"]);

        if (std::abs(seek.doubleValue - kSeekFraction) < kControlTolerance)
        {
          AO_INVARIANT(elapsed.stringValue.length > 0 && duration.stringValue.length > 0,
                       "Seeking must retain the public playback time labels");
          auto* const expectedDescription =
            [NSString stringWithFormat:@"%@ of %@", elapsed.stringValue, duration.stringValue];

          if ([seek.accessibilityValueDescription isEqualToString:expectedDescription] != NO)
          {
            _stage = Stage::ShowVolume;
          }
        }

        break;
      }
      case Stage::ShowVolume:
        [self activateControl:[self control:@"playback.volume-button"] obligation:@"show volume"];
        _stage = Stage::SetVolume;
        break;
      case Stage::SetVolume:
      {
        auto* const volume = static_cast<NSSlider*>([self controlInAnyWindow:@"playback.volume"]);

        if (volume == nil || volume.window == _window)
        {
          return;
        }

        _volumeDescription = [volume.accessibilityValueDescription copy];
        volume.doubleValue = kVolume;
        [self activateControl:volume obligation:@"set volume"];
        _stage = Stage::WaitForVolume;
        break;
      }
      case Stage::WaitForVolume:
      {
        auto* const volume = static_cast<NSSlider*>([self controlInAnyWindow:@"playback.volume"]);

        if (volume != nil && std::abs(volume.doubleValue - kVolume) < kControlTolerance &&
            [volume.accessibilityValueDescription isEqual:_volumeDescription] == NO)
        {
          AO_INVARIANT(volume.accessibilityValueDescription.length > 0,
                       "Volume changes must refresh the native accessibility value");
          [self activateControl:[self control:@"playback.volume-button"] obligation:@"dismiss volume"];
          _stage = Stage::SortTitle;
        }

        break;
      }
      case Stage::SortTitle:
        _unsortedFirstIdentity = [rowIdentity(_table, firstSelectableRow(_table)) copy];
        _table.sortDescriptors = @[[[NSSortDescriptor alloc] initWithKey:@"Title" ascending:NO]];
        _stage = Stage::WaitForTitleSort;
        break;
      case Stage::WaitForTitleSort:
        if (_table.selectedRow >= 0 &&
            [rowIdentity(_table, firstSelectableRow(_table)) isEqual:_unsortedFirstIdentity] == NO &&
            [rowIdentity(_table, _table.selectedRow) isEqual:_selectedIdentity] != NO)
        {
          AO_INVARIANT(_table.sortDescriptors.count == 1 &&
                         [_table.sortDescriptors.firstObject.key isEqual:@"Title"] != 0 &&
                         _table.sortDescriptors.firstObject.ascending == 0,
                       "The native Title header must retain descending sort state");
          _stage = Stage::ModernSearch;
        }

        break;
      case Stage::ModernSearch:
        if ([self driveSearch] != NO)
        {
          _stage = Stage::SelectClassic;
        }

        break;
      case Stage::SelectClassic:
        activateMenuItem([self modeItem:0], @"select Classic mode");
        _stage = Stage::WaitForClassic;
        break;
      case Stage::WaitForClassic:
        if (ao::appkit::test::findView(_window.contentView, @"playback.classic").window == _window)
        {
          _stage = Stage::ClassicSearch;
        }

        break;
      case Stage::ClassicSearch:
        if ([self driveSearch] != NO)
        {
          _stage = Stage::SelectModern;
        }

        break;
      case Stage::SelectModern:
        activateMenuItem([self modeItem:1], @"select Modern mode");
        _stage = Stage::WaitForModern;
        break;
      case Stage::WaitForModern:
        if (ao::appkit::test::findView(_window.contentView, @"playback.modern").window == _window)
        {
          _stage = Stage::ResizeForInspector;
        }

        break;
      case Stage::ResizeForInspector:
        [_window setContentSize:(::NSMakeSize(780, 520))];
        _stage = Stage::ShowCompactInspector;
        break;
      case Stage::ShowCompactInspector:
        if (_window.contentView.bounds.size.width < 1000)
        {
          activateMenuItem([self menuItem:@"toggleInspector:"], @"show compact inspector");
          _stage = Stage::WaitForCompactInspector;
        }

        break;
      case Stage::WaitForCompactInspector:
        if (_window.attachedSheet != nil)
        {
          auto* const scroll = static_cast<NSScrollView*>(
            findAccessibilityView(_window.attachedSheet.contentView, @"compact-inspector-scroll"));
          AO_INVARIANT([scroll isKindOfClass:NSScrollView.class] != 0 && scroll.hasVerticalScroller &&
                         [scroll.documentView isKindOfClass:NSTextView.class] != 0 &&
                         static_cast<NSTextView*>(scroll.documentView).string.length > 0,
                       "Compact inspector must present selected track details in a native scrolling sheet");
          _stage = Stage::DismissCompactInspector;
        }

        break;
      case Stage::DismissCompactInspector:
        if (auto* const sheet = _window.attachedSheet; sheet == nil)
        {
          _stage = Stage::RestoreWindowSize;
        }
        else if (sheet.keyWindow != NO)
        {
          // AppKit may ignore input during sheet presentation; retry only while this sheet remains attached.
          [NSApp sendEvent:[NSEvent keyEventWithType:NSEventTypeKeyDown
                                                location:NSZeroPoint
                                           modifierFlags:0
                                               timestamp:NSProcessInfo.processInfo.systemUptime
                                            windowNumber:sheet.windowNumber
                                                 context:nil
                                              characters:@"\x1b"
                             charactersIgnoringModifiers:@"\x1b"
                                               isARepeat:NO
                                                 keyCode:53]];
        }

        break;
      case Stage::RestoreWindowSize:
        if (_window.attachedSheet == nil)
        {
          [_window setContentSize:_initialContentSize];
          _stage = Stage::CloseWindow;
        }

        break;
      case Stage::CloseWindow:
        [_window performClose:nil];
        _stage = Stage::WaitForClosedWindow;
        break;
      case Stage::WaitForClosedWindow:
        if (_window.visible == NO)
        {
          AO_INVARIANT(
            [NSApp.windows containsObject:_window] != 0, "Closing must retain the production desktop window");
          _stage = Stage::ReopenWindow;
        }

        break;
      case Stage::ReopenWindow:
        activateMenuItem([self menuItem:@"showMainWindow:"], @"show retained desktop window");
        _stage = Stage::WaitForReopenedWindow;
        break;
      case Stage::WaitForReopenedWindow:
        if (_window.visible != NO && selectableRowCount(_table) >= 3)
        {
          AO_INVARIANT(
            _table.selectedRow >= 0 && [rowIdentity(_table, _table.selectedRow) isEqual:_selectedIdentity] != NO,
            "Reopening must retain the library and selected track");
          _stage = Stage::OpenDirtyEditor;
        }

        break;
      case Stage::OpenDirtyEditor:
        activateMenuItem([self menuItem:@"showProperties:"], @"open Properties editor");
        _stage = Stage::EditForQuitCancellation;
        break;
      case Stage::EditForQuitCancellation:
        if (_window.attachedSheet != nil)
        {
          ao::appkit::test::editText(_window.attachedSheet, @"genre", @"Quit cancellation smoke");
          _stage = Stage::WaitForQuitConfirmation;
          [self requestTermination];
        }

        break;
      case Stage::WaitForQuitConfirmation:
      {
        if (auto* const editor = _window.attachedSheet; editor.attachedSheet != nil)
        {
          [editor endSheet:editor.attachedSheet returnCode:NSAlertFirstButtonReturn];
          _settleTicks = 0;
          _stage = Stage::VerifyQuitCancellation;
        }

        break;
      }
      case Stage::VerifyQuitCancellation:
      {
        if (auto* const editor = _window.attachedSheet;
            editor != nil && editor.attachedSheet == nil && _window.visible != NO && ++_settleTicks >= 2)
        {
          AO_INVARIANT(ao::appkit::test::findControl(editor.contentView, @"genre") != nil,
                       "Cancelling Quit must retain the dirty editor");
          _stage = Stage::VerifySwitchUnavailable;
        }

        break;
      }
      case Stage::VerifySwitchUnavailable:
      {
        auto* const recent = [self successorItem];
        auto* const openLibrary = [self menuItem:@"openLibrary:"];
        AO_INVARIANT(recent != nil && openLibrary != nil, "The native library commands must exist");
        [recent.menu update];
        [openLibrary.menu update];
        AO_INVARIANT(recent.enabled == 0 && openLibrary.enabled == 0 && _window.attachedSheet != nil &&
                       hasSavedLibrary(_launch.stateRoot, _launch.optRequest->libraryRoot),
                     "A Properties sheet must block menu-driven library switching without changing settings");
        _stage = Stage::CancelDirtyEditor;
        break;
      }
      case Stage::CancelDirtyEditor:
      {
        auto* const cancel = findActionControl(_window.attachedSheet.contentView, ::NSSelectorFromString(@"cancel:"));
        [self activateControl:cancel obligation:@"cancel dirty Properties editor"];
        _stage = Stage::WaitForDiscardConfirmation;
        break;
      }
      case Stage::WaitForDiscardConfirmation:
      {
        if (auto* const editor = _window.attachedSheet; editor.attachedSheet != nil)
        {
          [editor endSheet:editor.attachedSheet returnCode:NSAlertSecondButtonReturn];
          _stage = Stage::WaitForEditorClose;
        }

        break;
      }
      case Stage::WaitForEditorClose:
        if (_window.attachedSheet == nil)
        {
          _stage = Stage::SwitchWhileScanning;
        }

        break;
      case Stage::SwitchWhileScanning:
        activateMenuItem([self menuItem:@"rescan:"], @"rescan active library");
        activateMenuItem([self successorItem], @"accept recent library switch while scan is active");
        _completed = YES;
        _stage = Stage::Complete;
        break;
      case Stage::Complete:
      case Stage::EditSuccessorForQuit:
      case Stage::JoinSuccessorQuit:
      case Stage::DiscardSuccessorAfterQuit:
      case Stage::WaitForWindow: break;
    }
  }
}
@end

namespace ao::appkit::test
{
  std::int32_t runDesktopScenario(DesktopLaunch launch, i18n::MessageCatalog catalog, bool successor)
  {
    NSString* const runToken = [NSProcessInfo.processInfo.environment[@"AOBUS_APPKIT_TEST_RUN_ID"] copy];
    AO_INVARIANT(runToken.length > 0, "The desktop scenario requires its isolated run token");
    auto* const markerContents = [runToken stringByAppendingString:@"\n"];

    if (!successor)
    {
      std::filesystem::create_directories(launch.stateRoot);
      preseedSettings(launch);
    }

    auto const stateRoot = launch.stateRoot;
    auto* const driver = [[AobusDesktopScenarioDriver alloc] initWithLaunch:launch
                                                                  successor:static_cast<BOOL>(successor)];
    auto const terminationObserver = [NSNotificationCenter.defaultCenter
      addObserverForName:NSApplicationWillTerminateNotification
                  object:NSApp
                   queue:nil
              usingBlock:^(NSNotification*) {
                AO_INVARIANT(driver.completed != 0, "The application must not terminate before its scenario completes");

                if (successor)
                {
                  // AppKit termination can exit without returning from -run. The
                  // production delegate has released its session before this notification.
                  writeMarker(stateRoot / "successor-pass.txt", markerContents);
                }
              }];
    [driver start];
    auto const applicationResult = runDesktopApplication(std::move(launch), std::move(catalog));
    [driver stop];
    [NSNotificationCenter.defaultCenter removeObserver:terminationObserver];

    if (applicationResult != 0)
    {
      return applicationResult;
    }

    if (driver.completed == NO)
    {
      return 1;
    }

    writeMarker(stateRoot / (successor ? "successor-pass.txt" : "desktop-pass.txt"), markerContents);
    return 0;
  }
} // namespace ao::appkit::test
