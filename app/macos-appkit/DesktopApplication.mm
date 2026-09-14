// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DesktopApplication.h"

#include "ActivityExpirationTimer.h"
#include "ActivityPopover.h"
#include "AppKitText.h"
#include "DesktopApplicationInternal.h"
#include "DesktopControls.h"
#include "LibraryBrowser.h"
#include "LibraryEditor.h"
#include "LibrarySession.h"
#include "PlaybackBar.h"
#include "TrackInspector.h"
#include <ao/Contract.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/desktop/DetachedProcessLauncher.h>
#include <ao/desktop/LibraryStartupPlanner.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/rt/Log.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>
#include <ao/utility/Path.h>

#import <AppKit/AppKit.h>
#include <CoreFoundation/CFRunLoop.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <sys/file.h>
#include <system_error>
#include <utility>
#include <vector>

constexpr auto kCpp23Version = 202302L;
static_assert(__cplusplus > kCpp23Version, "The AppKit bridge must compile as Objective-C++26");

namespace
{
  using ao::appkit::kContentInset;
  using ao::appkit::nativeAppearance;
  using ao::appkit::nativeCallback;
  using ao::appkit::nativeText;
  using ao::appkit::utf8;
  using ao::i18n::MessageId;
  constexpr auto kPrivateStateFileMode = 0600;
  constexpr auto kMaximumRecentLibraries = std::size_t{8};
  constexpr auto kFrameRefreshIntervalSeconds = 0.05;
  constexpr auto kActivityMaximumHeight = 420.0;

  void showError(NSWindow* window, NSString* title, NSString* message)
  {
    auto* const alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = message;

    if (window != nil)
    {
      [alert beginSheetModalForWindow:window completionHandler:nil];
    }
    else
    {
      [alert runModal];
    }
  }

  ao::Result<> createDesktopDirectory(std::filesystem::path const& path)
  {
    auto error = std::error_code{};
    std::filesystem::create_directories(path, error);

    if (error)
    {
      return ao::makeError(
        ao::Error::Code::IoError,
        std::format("Failed to create directory '{}': {}", ao::utility::pathToUtf8(path), error.message()));
    }

    return {};
  }

  // This lease guards the native settings document across different library roots.
  class ApplicationStateLease final
  {
  public:
    // POSIX open requires its variadic mode argument when creating the lock file.
    explicit ApplicationStateLease(std::filesystem::path const& path)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      : _descriptor{::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, kPrivateStateFileMode)}
      , _locked{_descriptor >= 0 && ::flock(_descriptor, LOCK_EX | LOCK_NB) == 0}
    {
    }
    ~ApplicationStateLease()
    {
      if (_descriptor >= 0)
      {
        ::close(_descriptor);
      }
    }
    ApplicationStateLease(ApplicationStateLease const&) = delete;
    ApplicationStateLease& operator=(ApplicationStateLease const&) = delete;
    ApplicationStateLease(ApplicationStateLease&&) = delete;
    ApplicationStateLease& operator=(ApplicationStateLease&&) = delete;
    bool isLocked() const noexcept { return _locked; }

  private:
    int _descriptor = -1;
    bool _locked = false;
  };
} // namespace

namespace ao::appkit
{
  void disableWindowRestoration()
  {
    AO_EXPECTS(NSApp == nil, "Disable window restoration before creating NSApplication");
    auto* const defaults = NSUserDefaults.standardUserDefaults;
    NSMutableDictionary* argumentDomain = [[defaults volatileDomainForName:NSArgumentDomain] mutableCopy];

    if (argumentDomain == nil)
    {
      argumentDomain = [NSMutableDictionary dictionary];
    }

    argumentDomain[@"ApplePersistenceIgnoreState"] = @YES;
    [defaults setVolatileDomain:argumentDomain forName:NSArgumentDomain];
  }

  NSTextField* label(NSString* text, CGFloat size, BOOL secondary)
  {
    auto* const field = [NSTextField labelWithString:text];
    field.font = [NSFont systemFontOfSize:size];
    field.textColor = (secondary != NO) ? NSColor.secondaryLabelColor : NSColor.labelColor;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
  }
  NSAppearance* nativeAppearance(NSString* name)
  {
    if ([name isEqual:@"Dark"] != NO)
    {
      return [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    }

    if ([name isEqual:@"Light"] != NO)
    {
      return [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    }

    return nil;
  }
} // namespace ao::appkit

@interface AobusDesktopDelegate () {
  std::optional<ao::i18n::MessageCatalog> _optCatalog;
  std::unique_ptr<ApplicationStateLease> _leasePtr;
  std::optional<ao::desktop::LibrarySwitchRequest> _optSuccessor;
  NSURL* _settingsURL;
  NSTimer* _frameTimer;
  AobusActivityExpirationTimer* _activityExpirationTimer;
  std::vector<ao::TrackId> _inspectorSelection;
  BOOL _stopped;
  BOOL _terminationPending;
  BOOL _serviceScheduled;
  BOOL _editorDirty;
  BOOL _activityTimerDirty;
  BOOL _quitPending;
  BOOL _hideWindowPending;
  BOOL _editorClosePending;
  AobusLibraryEditor* _libraryEditor;
  std::int32_t _exitCode;
}
- (void)configureActivityExpirationTimer;
@end

@implementation AobusDesktopDelegate
- (instancetype)initWithLaunch:(ao::appkit::DesktopLaunch)launch catalog:(ao::i18n::MessageCatalog)catalog
{
  self = [super init];

  if (self != nil)
  {
    _launch = std::move(launch);
    _optCatalog.emplace(std::move(catalog));
  }

  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*) [[maybe_unused]] notification
{
  nativeCallback(
    [&]
    {
      if (auto directoryRes = createDesktopDirectory(_launch.stateRoot); !directoryRes)
      {
        showError(nil, [self text:MessageId::AppKitOperationFailed], nativeText(directoryRes.error().message));
        _exitCode = 1;
        [self finishClosing];
        return;
      }

      _leasePtr = std::make_unique<ApplicationStateLease>(_launch.stateRoot / "desktop.lock");

      if (!_leasePtr->isLocked())
      {
        showError(nil, [self text:MessageId::AppKitOperationFailed], [self text:MessageId::AppKitSettingsAlreadyOpen]);
        _exitCode = 1;
        [self finishClosing];
        return;
      }

      ao::rt::Log::initialize(ao::rt::LogLevel::Info, _launch.stateRoot / "logs");
      _settingsURL = [NSURL fileURLWithPath:nativeText(ao::utility::pathToUtf8(_launch.stateRoot / "desktop.plist"))];
      _settings = [[NSDictionary dictionaryWithContentsOfURL:_settingsURL] mutableCopy];

      if (_settings == nil)
      {
        _settings = [NSMutableDictionary dictionary];
      }

      auto optRoot = std::optional<std::filesystem::path>{};

      if (NSString* const savedRoot = _settings[@"libraryRoot"];
          [savedRoot isKindOfClass:NSString.class] && [savedRoot length] > 0)
      {
        optRoot = ao::utility::pathFromUtf8(utf8(savedRoot));
      }

      auto planRes = ao::desktop::planLibraryStartup({
        .optSuccessorRequest = _launch.optRequest,
        .optPersistedRoot = std::move(optRoot),
        .emptyLibraryRoot = _launch.stateRoot / "empty-library",
      });

      if (!planRes)
      {
        showError(nil, [self text:MessageId::AppKitOperationFailed], nativeText(planRes.error().message));
        _exitCode = 1;
        [self finishClosing];
        return;
      }

      if (planRes->source == ao::desktop::LibraryStartupRootSource::EmptyLibraryFallback)
      {
        if (auto directoryRes = createDesktopDirectory(planRes->libraryRoot); !directoryRes)
        {
          showError(nil, [self text:MessageId::AppKitOperationFailed], nativeText(directoryRes.error().message));
          _exitCode = 1;
          [self finishClosing];
          return;
        }
      }

      auto const shouldScan =
        planRes->scanAfterOpen || !ao::rt::LibraryPaths{planRes->libraryRoot}.hasExistingDatabase();
      __weak AobusDesktopDelegate* weakSelf = self;
      auto sessionRes = ao::appkit::LibrarySession::create(
        planRes->libraryRoot,
        _launch.stateRoot,
        planRes->playbackPersistence == ao::desktop::PlaybackPersistenceStartup::Restore,
        *_optCatalog,
        [weakSelf](ao::appkit::DesktopInvalidation const invalidation)
        {
          if (auto* const owner = weakSelf; owner != nil)
          {
            [owner invalidate:invalidation];
          }
        });

      if (!sessionRes)
      {
        showError(nil, [self text:MessageId::AppKitOperationFailed], nativeText(sessionRes.error().message));
        _exitCode = 1;
        [self finishClosing];
        return;
      }

      _sessionPtr = std::move(*sessionRes);
      [self configureActivityExpirationTimer];

      if (NSDictionary* const route = _settings[@"output"];
          [route isKindOfClass:NSDictionary.class] != 0 && [route[@"backend"] isKindOfClass:NSString.class] != 0 &&
          [route[@"device"] isKindOfClass:NSString.class] != 0 && [route[@"profile"] isKindOfClass:NSString.class] != 0)
      {
        _sessionPtr->selectOutput({.backendId = ao::audio::BackendId{utf8(route[@"backend"])},
                                   .deviceId = ao::audio::DeviceId{utf8(route[@"device"])},
                                   .profileId = ao::audio::ProfileId{utf8(route[@"profile"])}});
      }

      _modern = static_cast<BOOL>([_settings[@"mode"] isEqual:@"classic"] == 0);
      _inspectorVisible = [_settings[_modern ? @"modernInspector" : @"classicInspector"] boolValue];
      NSApp.appearance = nativeAppearance(_settings[@"appearance"]);
      [self buildMenu];
      [self buildWindow];
      [_window makeKeyAndOrderFront:nil];
      [NSApp activateIgnoringOtherApps:YES];

      if (planRes->optSelectedRootCommit)
      {
        NSString* const previousRoot = _settings[@"libraryRoot"];
        NSArray* const previousRecent = [_settings[@"recentLibraries"] copy];
        _settings[@"libraryRoot"] = nativeText(ao::utility::pathToUtf8(*planRes->optSelectedRootCommit));
        [self rememberLibraryURL:[NSURL fileURLWithPath:_settings[@"libraryRoot"]]];

        if ([self saveSettings])
        {
          _sessionPtr->runtime().startPlaybackSessionPersistence();
        }
        else
        {
          if (previousRoot != nil)
          {
            _settings[@"libraryRoot"] = previousRoot;
          }
          else
          {
            [_settings removeObjectForKey:@"libraryRoot"];
          }

          if (previousRecent != nil)
          {
            _settings[@"recentLibraries"] = previousRecent;
          }
          else
          {
            [_settings removeObjectForKey:@"recentLibraries"];
          }

          [self refreshRecentLibrariesMenu];

          _sessionPtr->runtime().sealPlaybackSessionPersistenceWrites();
        }
      }
      else if (planRes->source != ao::desktop::LibraryStartupRootSource::EmptyLibraryFallback)
      {
        [self rememberLibraryURL:[NSURL fileURLWithPath:nativeText(ao::utility::pathToUtf8(planRes->libraryRoot))]];
        [self saveSettings];
      }

      if (shouldScan)
      {
        _sessionPtr->rescan();
      }

      [NSNotificationCenter.defaultCenter addObserver:self
                                             selector:@selector(sheetDidEnd:)
                                                 name:NSWindowDidEndSheetNotification
                                               object:nil];

      [self invalidateAll];
      APP_LOG_INFO("AppKit desktop ready: {}", ao::utility::pathToUtf8(planRes->libraryRoot));
    });
}

- (NSString*)text:(MessageId)message
{
  return ao::appkit::catalogText(*_optCatalog, message);
}

- (void)windowDidMiniaturize:(NSNotification*) [[maybe_unused]] notification
{
  [self updateFrameTimer];
}

- (void)windowDidDeminiaturize:(NSNotification*) [[maybe_unused]] notification
{
  [self invalidateAll];
}

- (void)windowDidChangeOcclusionState:(NSNotification*) [[maybe_unused]] notification
{
  nativeCallback(
    [&]
    {
      if ([self isWindowVisibleForRendering] != NO)
      {
        [self invalidateAll];
      }
      else
      {
        [self updateFrameTimer];
      }
    });
}

- (void)applicationDidHide:(NSNotification*) [[maybe_unused]] notification
{
  [self updateFrameTimer];
}

- (void)applicationDidUnhide:(NSNotification*) [[maybe_unused]] notification
{
  [self invalidateAll];
}

- (void)invalidate:(ao::appkit::DesktopInvalidation)invalidation
{
  switch (invalidation)
  {
    case ao::appkit::DesktopInvalidation::Library:
      _libraryDirty = YES;
      _playbackDirty = YES;
      break;
    case ao::appkit::DesktopInvalidation::Playback: _playbackDirty = YES; break;
    case ao::appkit::DesktopInvalidation::Activity:
      _playbackDirty = YES;
      _activityTimerDirty = YES;
      break;
    case ao::appkit::DesktopInvalidation::Editor: _editorDirty = YES; break;
  }

  [self requestService];
}

- (void)invalidateAll
{
  _libraryDirty = YES;
  _playbackDirty = YES;
  _editorDirty = YES;
  _activityTimerDirty = YES;
  _layoutPending = YES;
  [self requestService];
}

- (void)requestService
{
  if (_serviceScheduled != NO || _stopped != NO)
  {
    return;
  }

  _serviceScheduled = YES;
  __weak AobusDesktopDelegate* weakSelf = self;
  ::CFRunLoopPerformBlock(::CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    auto* const owner = weakSelf;

    if (owner == nil || owner->_stopped != NO)
    {
      return;
    }

    owner->_serviceScheduled = NO;
    [owner service];
  });
  ::CFRunLoopWakeUp(::CFRunLoopGetMain());
}

- (void)requestEditorCloseForLifecycle
{
  if (_libraryEditor == nil || _editorClosePending != NO)
  {
    return;
  }

  _editorClosePending = YES;
  __weak AobusDesktopDelegate* weakSelf = self;
  __attribute__((objc_precise_lifetime)) AobusLibraryEditor* const editor = _libraryEditor;
  __weak AobusLibraryEditor* weakEditor = editor;
  [editor requestCloseWithCompletion:^(BOOL closed) {
    auto* const owner = weakSelf;

    if (owner == nil || owner->_stopped != NO)
    {
      return;
    }

    owner->_editorClosePending = NO;

    if (closed != NO)
    {
      if (owner->_libraryEditor == weakEditor)
      {
        owner->_libraryEditor = nil;
      }
    }
    else
    {
      owner->_hideWindowPending = NO;
      owner->_quitPending = NO;
      owner->_optPendingSuccessor.reset();

      if (owner->_terminationPending != NO)
      {
        owner->_terminationPending = NO;
        [NSApp replyToApplicationShouldTerminate:NO];
      }
    }

    [owner requestService];
  }];
}

- (BOOL)tryServiceLibrarySwitch
{
  if (!_optPendingSuccessor)
  {
    return NO;
  }

  if (_libraryEditor != nil)
  {
    [self requestEditorCloseForLifecycle];
    return YES;
  }

  if (_presentingMenu != NO || _window.attachedSheet != nil)
  {
    return YES;
  }

  if (!_sessionPtr->canClose())
  {
    [self requestService];
    return YES;
  }

  [self prepareLibrarySwitch];

  if (_closing != NO)
  {
    [self requestService];
  }

  return YES;
}

- (BOOL)tryServiceQuit
{
  if (_quitPending == NO)
  {
    return NO;
  }

  if (_libraryEditor != nil)
  {
    [self requestEditorCloseForLifecycle];
    return YES;
  }

  if (_presentingMenu != NO)
  {
    return YES;
  }

  if (_window.attachedSheet != nil)
  {
    if (_window.attachedSheet == _trackInspector.sheet)
    {
      [self dismissInspector:nil];
    }
    else
    {
      [_window endSheet:_window.attachedSheet returnCode:NSModalResponseCancel];
    }

    return YES;
  }

  if ([self prepareToClose] == NO)
  {
    _quitPending = NO;

    if (_terminationPending != NO)
    {
      _terminationPending = NO;
      [NSApp replyToApplicationShouldTerminate:NO];
    }

    return YES;
  }

  if (_sessionPtr)
  {
    _sessionPtr->sealMediaPlayerAdmission();
  }

  _quitPending = NO;
  _closing = YES;
  [self updateFrameTimer];
  [self requestService];
  return YES;
}

- (BOOL)tryFinishClosing
{
  if (_closing == NO)
  {
    return NO;
  }

  if (_presentingMenu != NO || _window.attachedSheet != nil)
  {
    return YES;
  }

  if (_sessionPtr && !_sessionPtr->canClose())
  {
    [self requestService];
    return YES;
  }

  [self finishClosing];
  return YES;
}

- (void)serviceViewUpdates
{
  if (_hideWindowPending != NO)
  {
    if (_libraryEditor != nil)
    {
      [self requestEditorCloseForLifecycle];
      return;
    }

    if (_presentingMenu != NO || _window.attachedSheet != nil)
    {
      return;
    }

    _hideWindowPending = NO;
    [_window orderOut:nil];
  }

  if (!_sessionPtr)
  {
    return;
  }

  if (_activityTimerDirty != NO)
  {
    _activityTimerDirty = NO;
    [_activityExpirationTimer refresh];
  }

  [self updateFrameTimer];

  if ([self isWindowVisibleForRendering] == NO)
  {
    return;
  }

  if (_libraryDirty != NO)
  {
    _libraryDirty = NO;
    [self refreshLibrary];
  }

  if (_playbackDirty != NO)
  {
    _playbackDirty = NO;
    [self refreshPlayback];
  }

  if (_layoutPending != NO)
  {
    _layoutPending = NO;
    [self layoutContent];
  }

  [self updateFrameTimer];
}

- (void)service
{
  nativeCallback(
    [&]
    {
      if (_stopped != NO)
      {
        return;
      }

      // Saving and deferred close completion must settle even when a lifecycle
      // request or an invisible window prevents ordinary view rendering.
      if (_editorDirty != 0 && _sessionPtr)
      {
        _editorDirty = NO;
        [self refreshEditor];
      }

      if ([self tryServiceLibrarySwitch] != 0 || [self tryServiceQuit] != 0 || [self tryFinishClosing] != 0)
      {
        return;
      }

      [self serviceViewUpdates];
    });
}

- (void)updateFrameTimer
{
  BOOL const soulAnimating = static_cast<BOOL>(
    _sessionPtr && _sessionPtr->state().soul.motionMode == ao::uimodel::AobusSoulMotionMode::Animating &&
    NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion == NO);
  BOOL const shouldRun = static_cast<BOOL>(_closing == NO && [self isWindowVisibleForRendering] != NO && _sessionPtr &&
                                           (_sessionPtr->state().position.isPlaying || soulAnimating != NO));

  if (shouldRun != NO && _frameTimer == nil)
  {
    _frameTimer = [NSTimer timerWithTimeInterval:kFrameRefreshIntervalSeconds
                                          target:self
                                        selector:@selector(frameTick:)
                                        userInfo:nil
                                         repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_frameTimer forMode:NSRunLoopCommonModes];
  }
  else if (shouldRun == NO && _frameTimer != nil)
  {
    [_frameTimer invalidate];
    _frameTimer = nil;

    if (_sessionPtr && _closing == NO)
    {
      auto const& state = _sessionPtr->state();
      auto const elapsed = _sessionPtr->playbackElapsed();
      [_playbackBar renderFrameForState:state elapsed:elapsed modern:_modern];
    }
  }
}

- (BOOL)isWindowVisibleForRendering
{
  return static_cast<BOOL>(_window.visible != NO && _window.miniaturized == NO && NSApp.hidden == NO &&
                           (_window.occlusionState & NSWindowOcclusionStateVisible) != 0);
}

- (void)configureActivityExpirationTimer
{
  __weak AobusDesktopDelegate* weakSelf = self;
  _activityExpirationTimer = [[AobusActivityExpirationTimer alloc]
    initWithRefresh:[weakSelf]
    {
      auto* const owner = weakSelf;
      return owner != nil && owner->_sessionPtr && owner->_closing == NO ? owner->_sessionPtr->refreshActivity()
                                                                         : std::nullopt;
    }];
}

- (void)frameTick:(NSTimer*) [[maybe_unused]] timer
{
  nativeCallback(
    [&]
    {
      if (!_sessionPtr || _closing != NO || [self isWindowVisibleForRendering] == NO)
      {
        [self updateFrameTimer];
        return;
      }

      [self refreshPlaybackProgress];
    });
}

- (void)sheetDidEnd:(NSNotification*) [[maybe_unused]] notification
{
  [self requestService];
}

- (void)refreshLibrary
{
  [_libraryBrowser refresh];
  auto const selection = [_libraryBrowser selectedTrackIds];
  [self refreshInspectorWithSelection:selection];
  auto const activeListId = [_libraryBrowser activeListId];
  auto const& filter = _sessionPtr->state().filter;
  _empty.hidden = static_cast<BOOL>(_sessionPtr->displayIndex().rowCount() > 0);
  _emptyHint.hidden = _empty.hidden;
  _emptyAction.hidden = _empty.hidden;
  _empty.stringValue = filter.entryText.empty() ? [self text:MessageId::AppKitLibraryEmpty]
                                                : [self text:MessageId::AppKitNoMatchingTracks];
  _emptyHint.stringValue = filter.entryText.empty() ? [self text:MessageId::AppKitEmptyLibraryHint]
                                                    : [self text:MessageId::AppKitNoMatchingTracksHint];
  _emptyAction.title = filter.entryText.empty() ? [self text:MessageId::AppKitOpenLibraryAction]
                                                : [self text:MessageId::AppKitClearFilter];

  if (filter.entryText.empty() && activeListId != ao::rt::kAllTracksListId)
  {
    _empty.stringValue = [self text:MessageId::AppKitListEmpty];
    _emptyHint.stringValue = [self text:MessageId::AppKitEmptyListHint];
    _emptyAction.title = [self text:MessageId::AppKitShowAllTracks];
  }

  if (_search.currentEditor == nil && utf8(_search.stringValue) != filter.entryText)
  {
    _search.stringValue = nativeText(filter.entryText);
  }

  if (_toolbarSearch != nil && _toolbarSearch.currentEditor == nil &&
      utf8(_toolbarSearch.stringValue) != filter.entryText)
  {
    _toolbarSearch.stringValue = nativeText(filter.entryText);
  }

  _count.textColor = filter.hasError ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
  _count.stringValue = filter.hasError ? [self text:MessageId::AppKitInvalidFilter]
                                       : ao::appkit::catalogFormat(_sessionPtr->catalog(),
                                                                   MessageId::TrackCount,
                                                                   {{"count", _sessionPtr->displayIndex().rowCount()}});
  _count.toolTip = nativeText(filter.tooltip);
  _toolbarSearch.textColor = filter.hasError ? NSColor.systemRedColor : NSColor.controlTextColor;
  _toolbarSearch.toolTip = nativeText(filter.tooltip);

  auto const identifiers = std::array<NSString*, 4>{@"albums", @"songs", @"artists", @"library"};

  for (std::size_t index = 0; index < identifiers.size(); ++index)
  {
    if ([_libraryBrowser.presentationIdentifier isEqual:identifiers[index]] != NO)
    {
      [_presentation selectItemAtIndex:static_cast<NSInteger>(index)];
      break;
    }
  }

  _browserTitle.stringValue = _libraryBrowser.activeListTitle;
  _toolbarTitle.stringValue = _browserTitle.stringValue;
  _toolbarCount.stringValue = _count.stringValue;
}

- (void)refreshPlayback
{
  auto const& state = _sessionPtr->state();
  [_playbackBar renderState:state modern:_modern];
  _search.textColor = state.filter.hasError ? NSColor.systemRedColor : NSColor.controlTextColor;
  _search.toolTip = nativeText(state.filter.tooltip);
  auto const status = state.activity.compact.text;
  auto statusText = status;

  if (_modern == NO)
  {
    statusText.clear();

    for (auto const& part : {state.nowPlaying.streamInfo,
                             state.nowPlaying.combinedStatus,
                             status,
                             ao::i18n::requiredFormat(_sessionPtr->catalog(),
                                                      MessageId::AppKitSelectionTotal,
                                                      {{"selected", _inspectorSelection.size()},
                                                       {"total", _sessionPtr->displayIndex().rowCount()}})})
    {
      if (!part.empty())
      {
        if (!statusText.empty())
        {
          statusText += "   ·   ";
        }

        statusText += part;
      }
    }
  }

  _status.stringValue = nativeText(statusText);
  _activityButton.toolTip = status.empty() ? [self text:MessageId::AppKitNoActivity] : nativeText(status);
  NSString* activitySymbol = @"checkmark.circle";

  switch (state.activity.compact.kind)
  {
    case ao::uimodel::ActivityStatusKind::Processing: activitySymbol = @"clock.arrow.circlepath"; break;
    case ao::uimodel::ActivityStatusKind::Info: activitySymbol = @"info.circle"; break;
    case ao::uimodel::ActivityStatusKind::Warning: activitySymbol = @"exclamationmark.triangle"; break;
    case ao::uimodel::ActivityStatusKind::Error: activitySymbol = @"xmark.octagon"; break;
    case ao::uimodel::ActivityStatusKind::Idle: break;
  }

  _activityButton.image = [NSImage imageWithSystemSymbolName:activitySymbol
                                    accessibilityDescription:[self text:MessageId::AppKitActivity]];

  if (_activityPopover.shown != NO)
  {
    [self refreshActivity];
  }

  [_trackInspector renderArtwork:state.selectedCover];
  [self refreshPlaybackProgress];
}

- (void)refreshPlaybackProgress
{
  auto const& state = _sessionPtr->state();
  auto const elapsed = _sessionPtr->playbackElapsed();
  [_playbackBar renderFrameForState:state elapsed:elapsed modern:_modern];
}

- (void)refreshInspector
{
  [self refreshInspectorWithSelection:_sessionPtr->selection()];
}

- (void)refreshInspectorWithSelection:(std::vector<ao::TrackId> const&)selection
{
  auto const hadSelection = !_inspectorSelection.empty();
  _inspectorSelection = selection;
  _layoutPending = static_cast<BOOL>(_layoutPending != NO || hadSelection != !selection.empty());
  auto const optRow =
    selection.size() == 1 ? _sessionPtr->runtime().library().snapshot().trackRow(selection.front()) : std::nullopt;
  [_trackInspector renderSelectionCount:selection.size()
                                    row:optRow
                              canReveal:static_cast<BOOL>(_closing == NO && optRow && optRow->optUriPath)];
}

- (void)transport:(NSControl*)sender
{
  if (_closing == NO)
  {
    nativeCallback([&] { _sessionPtr->execute(static_cast<ao::uimodel::PlaybackCommand>(sender.tag)); });
  }
}

- (void)seek:(NSSlider*)sender
{
  if (_closing == NO)
  {
    nativeCallback(
      [&]
      {
        if (auto const optTarget = [static_cast<AobusSeekSlider*>(sender) seekTarget]; optTarget)
        {
          _sessionPtr->seek(sender.doubleValue, *optTarget);
        }
      });
  }
}

- (void)volume:(NSSlider*)sender
{
  if (_closing == NO)
  {
    nativeCallback([&] { _sessionPtr->setVolume(sender.floatValue); });
  }
}

- (void)rescan:(id) [[maybe_unused]] sender
{
  if (_closing == NO)
  {
    nativeCallback([&] { _sessionPtr->rescan(); });
  }
}

- (void)showActivity:(id) [[maybe_unused]] sender
{
  if ((_closing != NO) || !_sessionPtr || _window.attachedSheet != nil)
  {
    return;
  }

  if (_activityPopover.shown != NO)
  {
    [_activityPopover close];
    return;
  }

  __weak AobusDesktopDelegate* weakSelf = self;
  _activityPopover = [[AobusActivityPopover alloc] initWithCatalog:_sessionPtr->catalog()
    dismissHandler:^{
      if (auto* const owner = weakSelf; owner != nil && owner->_closing == NO && owner->_sessionPtr)
      {
        nativeCallback([&] { owner->_sessionPtr->dismissActivity(); });
      }
    }
    hideNotificationHandler:^(ao::rt::NotificationId identifier) {
      if (auto* const owner = weakSelf; owner != nil && owner->_closing == NO && owner->_sessionPtr)
      {
        nativeCallback([&] { owner->_sessionPtr->hideActivityNotification(identifier); });
      }
    }];
  [self refreshActivity];
  auto* const anchor = _activityButton.window != nil ? static_cast<NSView*>(_activityButton) : _root;
  auto const anchorRect =
    anchor == _root ? NSMakeRect(NSMaxX(_root.bounds) - 1, NSMinY(_root.bounds), 1, 1) : _activityButton.bounds;
  [_activityPopover showRelativeToRect:anchorRect ofView:anchor preferredEdge:NSRectEdgeMaxY];
}

- (void)refreshActivity
{
  if (!_sessionPtr || _activityPopover == nil)
  {
    return;
  }

  auto maximumHeight = static_cast<CGFloat>(kActivityMaximumHeight);

  if (auto* const screen = _window.screen; screen != nil)
  {
    maximumHeight = std::min(maximumHeight, screen.visibleFrame.size.height - (2 * kContentInset));
  }

  [_activityPopover render:_sessionPtr->state().activity maximumHeight:maximumHeight];
}

- (void)selectOutput:(NSMenuItem*)sender
{
  if (_closing == NO)
  {
    nativeCallback(
      [&]
      {
        NSDictionary* const route = sender.representedObject;
        _sessionPtr->selectOutput({.backendId = ao::audio::BackendId{utf8(route[@"backend"])},
                                   .deviceId = ao::audio::DeviceId{utf8(route[@"device"])},
                                   .profileId = ao::audio::ProfileId{utf8(route[@"profile"])}});
        _settings[@"output"] = route;
        [self saveSettings];
      });
  }
}

- (void)revealSelection:(id)sender
{
  nativeCallback(
    [&]
    {
      if (auto* const url = [self revealURL:sender]; url != nil)
      {
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[url]];
      }
    });
}

- (NSURL*)revealURL:(id)sender
{
  if (_closing != NO || !_sessionPtr)
  {
    return nil;
  }

  if (auto const ids = [self capturedTracks:sender]; ids.size() == 1)
  {
    if (auto const optRow = _sessionPtr->runtime().library().snapshot().trackRow(ids.front());
        optRow && optRow->optUriPath)
    {
      return [NSURL fileURLWithPath:nativeText(ao::utility::pathToUtf8(*optRow->optUriPath))];
    }
  }

  return nil;
}

- (BOOL)saveSettings
{
  NSError* error = nil;
  auto* const data = [NSPropertyListSerialization dataWithPropertyList:_settings
                                                                format:NSPropertyListBinaryFormat_v1_0
                                                               options:0
                                                                 error:&error];

  if (data == nil || ([data writeToURL:_settingsURL options:NSDataWritingAtomic error:&error] == NO))
  {
    showError(_window, [self text:MessageId::AppKitOperationFailed], error.localizedDescription);
    return NO;
  }

  return YES;
}

- (void)openLibrary:(id) [[maybe_unused]] sender
{
  if ((_closing != NO) || _optPendingSuccessor || _window.attachedSheet != nil)
  {
    return;
  }

  auto* const panel = [NSOpenPanel openPanel];
  panel.canChooseDirectories = YES;
  panel.canChooseFiles = NO;
  panel.allowsMultipleSelection = NO;
  panel.message = [self text:MessageId::AppKitChooseLibraryFolder];
  __weak AobusDesktopDelegate* weakSelf = self;
  [panel beginSheetModalForWindow:_window
                completionHandler:^(NSModalResponse response) {
                  auto* const owner = weakSelf;

                  if (owner == nil)
                  {
                    return;
                  }

                  if ((owner->_closing == NO) && response == NSModalResponseOK)
                  {
                    [owner openLibraryAtURL:panel.URL];
                  }

                  [owner requestService];
                }];
}

- (void)rememberLibraryURL:(NSURL*)url
{
  auto* const path = url.URLByStandardizingPath.path;

  if (path.length == 0)
  {
    return;
  }

  auto* const recent = [NSMutableArray<NSString*> array];

  if (NSArray* const saved = _settings[@"recentLibraries"]; [saved isKindOfClass:NSArray.class] != NO)
  {
    for (NSUInteger index = 0; index < saved.count; ++index)
    {
      if (id const item = saved[index]; [item isKindOfClass:NSString.class] != NO && [item isEqual:path] == NO)
      {
        [recent addObject:item];
      }
    }
  }

  [recent insertObject:path atIndex:0];

  if (recent.count > kMaximumRecentLibraries)
  {
    [recent removeObjectsInRange:NSMakeRange(kMaximumRecentLibraries, recent.count - kMaximumRecentLibraries)];
  }

  _settings[@"recentLibraries"] = recent;
  [self refreshRecentLibrariesMenu];
}

- (void)openRecentLibrary:(NSMenuItem*)sender
{
  NSString* const path = sender.representedObject;

  if (BOOL directory = NO; [path isKindOfClass:NSString.class] == NO ||
                           [NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] == NO ||
                           directory == NO)
  {
    NSMutableArray* const recent = [_settings[@"recentLibraries"] mutableCopy];

    if ([path isKindOfClass:NSString.class] != NO)
    {
      [recent removeObject:path];
    }

    _settings[@"recentLibraries"] = recent != nil ? recent : @[];
    [self saveSettings];
    [self refreshRecentLibrariesMenu];
    showError(
      _window,
      [self text:MessageId::AppKitOperationFailed],
      ao::appkit::catalogFormat(
        _sessionPtr->catalog(),
        MessageId::AppKitRecentUnavailable,
        {{"path",
          utf8([path isKindOfClass:NSString.class] != NO ? path : [self text:MessageId::AppKitUnknownFolder])}}));
    return;
  }

  [self openLibraryAtURL:[NSURL fileURLWithPath:path isDirectory:YES]];
}

- (void)clearRecentLibraries:(id) [[maybe_unused]] sender
{
  _settings[@"recentLibraries"] = @[];
  [self saveSettings];
  [self refreshRecentLibrariesMenu];
}

- (NSDragOperation)validateLibraryDrop:(id<NSDraggingInfo>)sender
{
  if (_closing != NO || _optPendingSuccessor || !_sessionPtr || _window.attachedSheet != nil)
  {
    return NSDragOperationNone;
  }

  NSArray<NSURL*>* const urls =
    [sender.draggingPasteboard readObjectsForClasses:@[NSURL.class]
                                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];

  if (urls.count != 1)
  {
    return NSDragOperationNone;
  }

  NSNumber* directory = nil;
  NSURL* const url = urls.firstObject;
  return [url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil] != NO && directory.boolValue != NO
           ? NSDragOperationCopy
           : NSDragOperationNone;
}

- (BOOL)performLibraryDrop:(id<NSDraggingInfo>)sender
{
  if ([self validateLibraryDrop:sender] == NSDragOperationNone)
  {
    return NO;
  }

  NSArray<NSURL*>* const urls =
    [sender.draggingPasteboard readObjectsForClasses:@[NSURL.class]
                                             options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
  [self openLibraryAtURL:urls.firstObject];
  return YES;
}

- (void)showMainWindow:(id) [[maybe_unused]] sender
{
  if (_closing == NO && _quitPending == NO && !_optPendingSuccessor)
  {
    _hideWindowPending = NO;
    [_window makeKeyAndOrderFront:nil];
    [self invalidateAll];
  }
}

- (void)openLibraryAtURL:(NSURL*)url
{
  nativeCallback(
    [&]
    {
      if (_closing != NO || _optPendingSuccessor || !_sessionPtr || url == nil)
      {
        return;
      }

      auto planRes = ao::desktop::planLibrarySwitch(
        _sessionPtr->runtime().musicRoot(), ao::utility::pathFromNative(url.fileSystemRepresentation), true);

      if (!planRes)
      {
        showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(planRes.error().message));
        return;
      }

      if (planRes->disposition == ao::desktop::LibrarySwitchDisposition::ReuseActive)
      {
        _sessionPtr->rescan();
        return;
      }

      _optPendingSuccessor = std::move(planRes->request);
      [self requestService];
    });
}

- (void)prepareLibrarySwitch
{
  AO_EXPECTS(_optPendingSuccessor && !_optSuccessor && _closing == NO);

  if ([self prepareToClose] == NO)
  {
    _optPendingSuccessor.reset();
    return;
  }

  if (auto retiredRes = _sessionPtr->runtime().retirePlaybackSessionForLibrarySwitch(); !retiredRes)
  {
    _optPendingSuccessor.reset();
    showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(retiredRes.error().message));
    return;
  }

  _sessionPtr->sealMediaPlayerAdmission();
  _optSuccessor = std::move(_optPendingSuccessor);
  _optPendingSuccessor.reset();
  _closing = YES;
  [self updateFrameTimer];
}

- (BOOL)windowShouldClose:(NSWindow*)window
{
  AO_INVARIANT(window == _window);
  _hideWindowPending = YES;
  [self requestService];
  return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*) [[maybe_unused]] app
                    hasVisibleWindows:(BOOL) [[maybe_unused]] visible
{
  [self showMainWindow:nil];
  return NO;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*) [[maybe_unused]] app
{
  return NO;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*) [[maybe_unused]] app
{
  // An admitted library switch owns shutdown; a competing quit must not report success.
  if (_optSuccessor || _optPendingSuccessor)
  {
    return NSTerminateCancel;
  }

  [self requestQuit:nil];

  if (_closing != NO || _quitPending != NO)
  {
    _terminationPending = YES;
    return NSTerminateLater;
  }

  return NSTerminateCancel;
}

- (void)requestQuit:(id) [[maybe_unused]] sender
{
  if (_optPendingSuccessor || _closing != NO || _quitPending != NO)
  {
    return;
  }

  _quitPending = YES;
  [self requestService];
}

- (BOOL)prepareToClose
{
  if (_sessionPtr)
  {
    _settings[@"windowFrame"] = ::NSStringFromRect(_window.frame);
    auto* const toolbar = _window.toolbar;

    if (auto* const identifiers = toolbar.itemIdentifiers; identifiers != nil)
    {
      _settings[@"toolbarConfiguration"] = @{@"itemIdentifiers": identifiers, @"displayMode": @(toolbar.displayMode)};
    }

    if ([self saveSettings] == NO)
    {
      return NO;
    }

    _sessionPtr->checkpoint();
  }

  return YES;
}

- (void)finishClosing
{
  if (_stopped != NO)
  {
    return;
  }

  AO_INVARIANT(_libraryEditor == nil && _editorClosePending == NO && _window.attachedSheet == nil,
               "Native editor close completion and sheets must settle before session teardown");
  _stopped = YES;
  _closing = YES;
  [NSNotificationCenter.defaultCenter removeObserver:self name:NSSplitViewDidResizeSubviewsNotification object:_split];
  [NSNotificationCenter.defaultCenter removeObserver:self name:NSWindowDidEndSheetNotification object:nil];
  [_frameTimer invalidate];
  _frameTimer = nil;
  [_activityExpirationTimer invalidate];
  _activityExpirationTimer = nil;

  [_trackInspector detach];
  [_playbackBar detach];
  [_activityPopover close];

  // Detaching can reload or tile the views, so keep their model alive here.
  [_libraryBrowser detach];
  _window.toolbar.delegate = nil;
  _root.libraryDropDestination = nil;
  _recentLibrariesMenu.delegate = nil;
  _listMenu.delegate = nil;
  _trackMenu.delegate = nil;
  [_root unregisterDraggedTypes];

  _sessionPtr.reset();

  _leasePtr.reset();

  if (_optSuccessor)
  {
    auto argumentsRes = ao::desktop::librarySuccessorArguments(*_optSuccessor);
    AO_INVARIANT(argumentsRes);
    argumentsRes->emplace_back("--state-root");
    argumentsRes->push_back(ao::utility::pathToUtf8(_launch.stateRoot));

    auto res =
      ao::desktop::launchDetachedProcess({.executable = _launch.executable, .arguments = std::move(*argumentsRes)});

    if (!res)
    {
      showError(nil, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
      _exitCode = 1;
    }
  }

  [_window orderOut:nil];

  if (_terminationPending != NO)
  {
    ao::rt::Log::shutdown();
    [NSApp replyToApplicationShouldTerminate:YES];
    return;
  }

  [NSApp stop:nil];
  auto* const wakeEvent = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:0
                                         windowNumber:0
                                              context:nil
                                              subtype:0
                                                data1:0
                                                data2:0];
  AO_INVARIANT(wakeEvent != nil);
  [NSApp postEvent:wakeEvent atStart:NO];
}

- (ao::ListId)activeListId
{
  return [_libraryBrowser activeListId];
}

- (BOOL)isLibraryBrowserClosing:(AobusLibraryBrowser*)browser
{
  return static_cast<BOOL>(browser != _libraryBrowser || _closing != NO || !_sessionPtr);
}

- (BOOL)isLibraryBrowserSheetBlocked:(AobusLibraryBrowser*)browser
{
  return static_cast<BOOL>(browser != _libraryBrowser || _window.attachedSheet != nil);
}

- (void)libraryBrowserSelectionDidChange:(AobusLibraryBrowser*)browser
{
  if (browser == _libraryBrowser && _closing == NO && _sessionPtr)
  {
    [self refreshInspectorWithSelection:[browser selectedTrackIds]];
  }
}

- (BOOL)libraryBrowser:(AobusLibraryBrowser*)browser
  requestMembershipForTracks:(std::vector<ao::TrackId> const&)trackIds
                      listId:(ao::ListId)listId
{
  if (browser != _libraryBrowser || _closing != NO || _window.attachedSheet != nil || !_sessionPtr)
  {
    return NO;
  }

  if (auto res = _sessionPtr->editor().beginMembership(trackIds, listId, false); !res)
  {
    showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
    return NO;
  }

  [self presentEditor];
  return YES;
}

- (ao::ListId)capturedList:(id)sender
{
  if ([sender isKindOfClass:NSMenuItem.class] != NO)
  {
    NSMenuItem* const item = sender;

    if (NSDictionary* const payload = item.representedObject;
        [payload isKindOfClass:NSDictionary.class] != NO && payload[@"list"] != nil)
    {
      return ao::ListId{[payload[@"list"] unsignedIntValue]};
    }
  }

  return [self activeListId];
}

- (std::vector<ao::TrackId>)capturedTracks:(id)sender
{
  if ([sender isKindOfClass:NSMenuItem.class] != NO)
  {
    NSMenuItem* const item = sender;

    if (NSDictionary* const payload = item.representedObject;
        [payload isKindOfClass:NSDictionary.class] != NO && payload[@"tracks"] != nil)
    {
      NSArray<NSNumber*>* const values = payload[@"tracks"];
      auto ids = std::vector<ao::TrackId>{};

      for (NSUInteger index = 0; index < values.count; ++index)
      {
        ids.emplace_back(values[index].unsignedIntValue);
      }

      return ids;
    }
  }

  return [_libraryBrowser selectedTrackIds];
}

- (void)showProperties:(id)sender
{
  nativeCallback(
    [&]
    {
      if (_closing != NO || _window.attachedSheet != nil)
      {
        return;
      }

      if (auto res = _sessionPtr->editor().beginProperties([self capturedTracks:sender]); !res)
      {
        showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
        return;
      }

      [self presentEditor];
    });
}

- (void)newList:(id) [[maybe_unused]] sender
{
  nativeCallback(
    [&]
    {
      if (_closing == NO && _window.attachedSheet == nil)
      {
        if (auto res = _sessionPtr->editor().beginList(); !res)
        {
          showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
          return;
        }

        [self presentEditor];
      }
    });
}

- (void)newChildList:(id)sender
{
  nativeCallback(
    [&]
    {
      if (_closing == NO && _window.attachedSheet == nil)
      {
        if (auto res = _sessionPtr->editor().beginList(ao::kInvalidListId, [self capturedList:sender]); !res)
        {
          showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
          return;
        }

        [self presentEditor];
      }
    });
}

- (void)editList:(id)sender
{
  nativeCallback(
    [&]
    {
      if (_closing == NO && _window.attachedSheet == nil)
      {
        if (auto res = _sessionPtr->editor().beginList([self capturedList:sender]); !res)
        {
          showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
          return;
        }

        [self presentEditor];
      }
    });
}

- (void)deleteList:(id)sender
{
  nativeCallback(
    [&]
    {
      if (_closing == NO && _window.attachedSheet == nil)
      {
        if (auto res = _sessionPtr->editor().beginDeletion([self capturedList:sender]); !res)
        {
          showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
          return;
        }

        [self presentEditor];
      }
    });
}

- (void)changeMembership:(NSMenuItem*)sender
{
  nativeCallback(
    [&]
    {
      if (_closing != NO || _window.attachedSheet != nil)
      {
        return;
      }

      NSDictionary* const payload = sender.representedObject;
      auto const listId = ao::ListId{[payload[@"list"] unsignedIntValue]};

      if (auto res = _sessionPtr->editor().beginMembership([self capturedTracks:sender], listId, sender.tag != 0); !res)
      {
        showError(_window, [self text:MessageId::AppKitOperationFailed], nativeText(res.error().message));
        return;
      }

      [self presentEditor];
    });
}

- (void)presentEditor
{
  auto const cover = _sessionPtr->state().selectedCover.view();
  NSImage* artwork = nil;

  if (_sessionPtr->editor().state().trackIds.size() == 1 && !cover.empty())
  {
    artwork = [[NSImage alloc] initWithData:[NSData dataWithBytes:cover.data() length:cover.size()]];
  }

  _libraryEditor = [[AobusLibraryEditor alloc] initWithModel:_sessionPtr->editor()
                                                      parent:_window
                                                      modern:_modern
                                                     artwork:artwork];
  [_libraryEditor present];
}

- (void)refreshEditor
{
  if (auto const& state = _sessionPtr->editor().state(); state.completed)
  {
    auto const savedId = state.savedListId;
    auto const deletedActive =
      state.optDeletion && std::ranges::any_of(state.optDeletion->deletedLists,
                                               [&](auto const& list) { return list.listId == [self activeListId]; });
    [_libraryEditor finish];
    _libraryEditor = nil;

    if (savedId != ao::kInvalidListId)
    {
      _sessionPtr->navigate(savedId);
    }
    else if (deletedActive)
    {
      _sessionPtr->navigate(ao::rt::kAllTracksListId);
    }

    [_libraryBrowser invalidateProjection];
    _libraryDirty = YES;
    _playbackDirty = YES;
  }
  else if (state.kind == ao::appkit::LibraryEditorKind::None)
  {
    _libraryEditor = nil;
  }
  else
  {
    [_libraryEditor refresh];
  }
}

- (void)playSelection:(id) [[maybe_unused]] sender
{
  [_libraryBrowser playSelectedTrack];
}

- (std::int32_t)exitCode
{
  return _exitCode;
}

@end

namespace ao::appkit
{
  std::int32_t runDesktopApplication(DesktopLaunch launch, i18n::MessageCatalog catalog)
  {
    auto* const app = NSApp;
    auto* delegate = [[AobusDesktopDelegate alloc] initWithLaunch:std::move(launch) catalog:std::move(catalog)];
    app.delegate = delegate;
    [app run];
    auto const exitCode = [delegate exitCode];
    app.delegate = nil;
    delegate = nil;
    rt::Log::shutdown();
    return exitCode;
  }
} // namespace ao::appkit
