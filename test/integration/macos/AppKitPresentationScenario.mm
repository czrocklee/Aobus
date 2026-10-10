// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitPlaybackScenario.h"
#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/ActivityPopover.h"
#include "app/macos-appkit/AppKitText.h"
#include "app/macos-appkit/DesktopControls.h"
#include "app/macos-appkit/LibraryBrowser.h"
#include "app/macos-appkit/LibrarySession.h"
#include "app/macos-appkit/TrackInspector.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

@interface AobusPresentationDraggingInfo : NSObject
@property (nonatomic, strong) NSPasteboard* draggingPasteboard;
@end

@implementation AobusPresentationDraggingInfo
@end

@interface AobusPresentationBrowserDelegate : NSObject<AobusLibraryBrowserDelegate>
@property (nonatomic) BOOL closing;
@property (nonatomic) BOOL sheetBlocked;
- (NSUInteger)selectionChangeCount;
- (NSUInteger)membershipRequestCount;
- (std::vector<ao::TrackId>)membershipTrackIds;
- (ao::ListId)membershipListId;
@end

@implementation AobusPresentationBrowserDelegate {
  NSUInteger _selectionChangeCount;
  NSUInteger _membershipRequestCount;
  std::vector<ao::TrackId> _membershipTrackIds;
  ao::ListId _membershipListId;
}

- (BOOL)isLibraryBrowserClosing:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  return _closing;
}

- (BOOL)isLibraryBrowserSheetBlocked:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  return _sheetBlocked;
}

- (void)libraryBrowserSelectionDidChange:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  ++_selectionChangeCount;
}

- (BOOL)libraryBrowser:(AobusLibraryBrowser*) [[maybe_unused]] browser
  requestMembershipForTracks:(std::vector<ao::TrackId> const&)trackIds
                      listId:(ao::ListId)listId
{
  AO_INVARIANT(!trackIds.empty(), "library membership request must contain tracks");
  AO_INVARIANT(listId != ao::kInvalidListId, "library membership request must target a list");
  ++_membershipRequestCount;
  _membershipTrackIds = trackIds;
  _membershipListId = listId;
  return YES;
}

- (NSUInteger)selectionChangeCount
{
  return _selectionChangeCount;
}

- (NSUInteger)membershipRequestCount
{
  return _membershipRequestCount;
}

- (std::vector<ao::TrackId>)membershipTrackIds
{
  return _membershipTrackIds;
}

- (ao::ListId)membershipListId
{
  return _membershipListId;
}
@end

namespace
{
  constexpr auto kActivityMaximumHeight = 360.0;
  constexpr auto kNotificationCount = std::size_t{40};
  constexpr auto kFixturePrefix = "Activity scenario ";
  constexpr auto kInitialUpdate = "Activity scenario live update: initial";
  constexpr auto kFinalUpdate = "Activity scenario live update: refreshed";
  constexpr auto kTopGrowth = "Activity scenario top growth";
  constexpr auto kHistoryGrowth = "Activity scenario history growth";

  template<typename Control>
  Control* requireControl(NSView* root, NSString* identifier)
  {
    auto* const control = ao::appkit::test::findControl(root, identifier);
    AO_INVARIANT(control != nil, "required AppKit control is missing");
    return static_cast<Control*>(control);
  }

  NSEvent* keyEvent(NSWindow* window, NSString* characters, std::uint16_t keyCode)
  {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:NSProcessInfo.processInfo.systemUptime
                        windowNumber:window.windowNumber
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:characters
                           isARepeat:NO
                             keyCode:keyCode];
  }

  NSString* notificationIdentifier(ao::rt::NotificationId const id)
  {
    return ao::appkit::nativeText(std::format("activity-notification-{}", id.raw()));
  }

  std::size_t activityFixtureCount(ao::appkit::LibrarySession const& session)
  {
    return static_cast<std::size_t>(std::ranges::count_if(session.state().activity.detail.items,
                                                          [](auto const& item)
                                                          { return item.message.starts_with(kFixturePrefix); }));
  }

  void exerciseActivity(ao::appkit::LibrarySession& session,
                        NSWindow* window,
                        NSButton* anchor,
                        std::filesystem::path const& stateRoot)
  {
    auto* const sessionBorrow = &session;
    auto* const popover = [[AobusActivityPopover alloc] initWithCatalog:session.catalog()
      dismissHandler:^{ sessionBorrow->dismissActivity(); }
      hideNotificationHandler:^(ao::rt::NotificationId const id) { sessionBorrow->hideActivityNotification(id); }];
    auto& notifications = session.runtime().notifications();

    for (std::size_t index = 0; index < kNotificationCount; ++index)
    {
      notifications.post(ao::rt::NotificationSeverity::Warning,
                         std::format("{}row {:02}", kFixturePrefix, index),
                         ao::rt::NotificationLifetime::history());
    }

    auto const updateKey = ao::rt::NotificationReportKey{"appkit.presentation.scenario.live"};
    notifications.createOrUpdate(updateKey,
                                 ao::rt::NotificationRequest{.severity = ao::rt::NotificationSeverity::Warning,
                                                             .message = kInitialUpdate,
                                                             .lifetime = ao::rt::NotificationLifetime::history()});
    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == kNotificationCount + 1; },
                                       "activity projection did not publish the scenario notifications");

    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    [popover showRelativeToRect:anchor.bounds ofView:anchor preferredEdge:NSRectEdgeMinY];
    ao::appkit::test::requireWaitUntil(
      [&] { return popover.shown != 0 && popover.contentViewController.view.window != nil; },
      "activity popover did not open");
    auto* const scroll = static_cast<NSScrollView*>(popover.contentViewController.view);
    AO_INVARIANT([scroll isKindOfClass:NSScrollView.class] != 0, "activity content must be a scroll view");
    [scroll layoutSubtreeIfNeeded];
    auto* const stack = scroll.documentView;
    AO_INVARIANT([stack isKindOfClass:NSStackView.class] != 0, "activity document must be a stack view");
    AO_INVARIANT(scroll.hasVerticalScroller != 0, "activity history must expose vertical scrolling");
    AO_INVARIANT(
      popover.contentSize.height <= kActivityMaximumHeight, "activity history must respect the supplied height bound");
    AO_INVARIANT(
      stack.frame.size.height > scroll.contentSize.height, "the populated history must exceed its bounded viewport");
    auto* const compact = requireControl<NSTextField>(stack, @"activity-compact");
    AO_INVARIANT(NSContainsRect(scroll.documentVisibleRect, [compact convertRect:compact.bounds toView:stack]),
                 "activity compact status must be visible when the popover opens");

    notifications.post(ao::rt::NotificationSeverity::Warning, kTopGrowth, ao::rt::NotificationLifetime::history());
    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == kNotificationCount + 2; },
                                       "activity projection did not publish a notification added while open");
    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    auto* const grownCompact = requireControl<NSTextField>(stack, @"activity-compact");
    AO_INVARIANT(
      NSContainsRect(scroll.documentVisibleRect, [grownCompact convertRect:grownCompact.bounds toView:stack]),
      "activity growth must keep the compact status visible when the user was at the top");

    auto const& items = session.state().activity.detail.items;
    auto const initial =
      std::ranges::find(items, std::string{kInitialUpdate}, &ao::uimodel::ActivityDetailItem::message);
    auto const newest =
      std::ranges::find(items, std::string{"Activity scenario row 39"}, &ao::uimodel::ActivityDetailItem::message);
    auto const oldest =
      std::ranges::find(items, std::string{"Activity scenario row 00"}, &ao::uimodel::ActivityDetailItem::message);
    AO_INVARIANT(initial != items.end() && newest != items.end() && oldest != items.end(),
                 "activity projection omitted a requested history item");
    auto const updatedId = initial->id;
    auto const oldestId = oldest->id;
    auto* const newestControl = requireControl<NSTextField>(stack, notificationIdentifier(newest->id));
    auto* const oldestControl = requireControl<NSTextField>(stack, notificationIdentifier(oldest->id));
    [newestControl scrollRectToVisible:newestControl.bounds];
    AO_INVARIANT(
      NSContainsRect(scroll.documentVisibleRect, [newestControl convertRect:newestControl.bounds toView:stack]),
      "newest activity history item must be reachable by scrolling");
    [oldestControl scrollRectToVisible:oldestControl.bounds];
    AO_INVARIANT(
      NSContainsRect(scroll.documentVisibleRect, [oldestControl convertRect:oldestControl.bounds toView:stack]),
      "oldest activity history item must be reachable by scrolling");

    notifications.post(ao::rt::NotificationSeverity::Warning, kHistoryGrowth, ao::rt::NotificationLifetime::history());
    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == kNotificationCount + 3; },
                                       "activity projection did not publish history growth");
    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    auto* const preservedOldest = requireControl<NSTextField>(stack, notificationIdentifier(oldestId));
    AO_INVARIANT(
      NSIntersectsRect(scroll.documentVisibleRect, [preservedOldest convertRect:preservedOldest.bounds toView:stack]),
      "activity growth must preserve the visible history item when the user is away from the top");
    ao::appkit::test::captureView(scroll, stateRoot / "appkit-activity-scroll.png");

    notifications.createOrUpdate(updateKey,
                                 ao::rt::NotificationRequest{.severity = ao::rt::NotificationSeverity::Warning,
                                                             .message = kFinalUpdate,
                                                             .lifetime = ao::rt::NotificationLifetime::history()});
    ao::appkit::test::requireWaitUntil(
      [&]
      {
        return std::ranges::contains(
          session.state().activity.detail.items, std::string{kFinalUpdate}, &ao::uimodel::ActivityDetailItem::message);
      },
      "activity keyed update did not reach the projection");
    [popover render:session.state().activity maximumHeight:kActivityMaximumHeight];
    AO_INVARIANT(popover.shown != 0, "a live activity update must preserve the open popover");
    auto* const updated = requireControl<NSTextField>(stack, notificationIdentifier(updatedId));
    AO_INVARIANT([updated.stringValue isEqualToString:@"Activity scenario live update: refreshed"] != 0,
                 "open activity content must render keyed notification updates");

    auto* const compactDismiss = requireControl<NSButton>(stack, @"activity-dismiss");
    AO_INVARIANT(compactDismiss.enabled != 0, "compact activity dismissal must be enabled");
    [compactDismiss performClick:nil];
    auto const fixtures = session.state().activity.detail.items;

    for (auto const& item : fixtures)
    {
      if (!item.message.starts_with(kFixturePrefix))
      {
        continue;
      }

      AO_INVARIANT(item.dismissible, "scenario activity notifications must be dismissible");
      auto* const dismiss =
        requireControl<NSButton>(stack, [notificationIdentifier(item.id) stringByAppendingString:@"-dismiss"]);
      AO_INVARIANT(dismiss.enabled != 0, "notification dismissal must be enabled");
      [dismiss performClick:nil];
    }

    ao::appkit::test::requireWaitUntil([&] { return activityFixtureCount(session) == 0; },
                                       "notification dismissal controls did not remove the scenario history");
    [popover close];
    ao::appkit::test::requireWaitUntil([&] { return popover.shown == 0; }, "activity popover did not close");
    AO_INVARIANT(window.attachedSheet == nil, "activity presentation must not attach a sheet");
  }

  NSTextView* inspectorDetailTextView(NSView* view)
  {
    if ([view isKindOfClass:NSTextView.class] != NO)
    {
      return static_cast<NSTextView*>(view);
    }

    auto* const children = view.subviews;

    for (NSUInteger index = 0; index < children.count; ++index)
    {
      if (auto* const found = inspectorDetailTextView(children[index]); found != nil)
      {
        return found;
      }
    }

    return nil;
  }

  // The inspector's main view owns exactly one non-editable detail text view;
  // renderSelectionCount replaces its text storage synchronously, so the
  // scenario can assert on the rendered rows before any sheet presentation.
  NSString* inspectorDetailString(NSView* view)
  {
    if (auto* const text = inspectorDetailTextView(view); text != nil)
    {
      return text.string;
    }

    return nil;
  }

  std::size_t countRenderedOccurrences(std::string const& text, std::string const& needle)
  {
    std::size_t count = 0;
    std::size_t position = 0;

    while ((position = text.find(needle, position)) != std::string::npos)
    {
      ++count;
      position += needle.size();
    }

    return count;
  }

  // The caller keeps the session and completion outputs alive through its
  // bounded, fatal-on-timeout native main-loop wait. Binding and patch are
  // owned by the coroutine. Its nonowning pointers are used only before the
  // callback-executor completion publication; the caller reads the failure
  // slot only after observing the atomic flag.
  ao::async::Task<void> seedPerformanceMetadataSubmissionAsync(ao::appkit::LibrarySession* session,
                                                               ao::rt::BoundTrackTargets binding,
                                                               ao::rt::MetadataPatch patch,
                                                               std::atomic<bool>* done,
                                                               std::optional<ao::Error>* failure)
  {
    AO_INVARIANT(session != nullptr && done != nullptr && failure != nullptr,
                 "the inspector performance seed requires its live session and completion outputs");
    co_await session->runtime().async().resumeOnCallbackExecutorAsync();
    auto res =
      co_await session->runtime().library().commands().updateMetadataAsync(std::move(binding), std::move(patch));
    co_await session->runtime().async().resumeOnCallbackExecutorAsync();

    if (!res)
    {
      *failure = res.error();
    }
    else if (res->status != ao::rt::AuthoringStatus::Applied && res->status != ao::rt::AuthoringStatus::NoOp)
    {
      *failure =
        ao::Error{.code = ao::Error::Code::Conflict, .message = "the inspector performance seed did not apply"};
    }

    done->store(true);
  }

  void exerciseInspector(ao::appkit::LibrarySession& session,
                         NSWindow* window,
                         ao::TrackId const trackId,
                         std::filesystem::path const& stateRoot)
  {
    __block NSUInteger revealCount = 0;
    __block NSUInteger propertiesCount = 0;
    __block NSUInteger dismissCount = 0;
    auto* const inspector = [[AobusTrackInspector alloc] initWithCatalog:session.catalog()
      revealHandler:^{ ++revealCount; }
      propertiesHandler:^{ ++propertiesCount; }
      dismissHandler:^{ ++dismissCount; }];

    auto const utf8Of = [](NSString* value) { return std::string{value != nil ? value.UTF8String : ""}; };
    auto const dateLabelText =
      utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::TrackFieldRecordingDate));
    auto const yearLabelText = utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::TrackFieldYear));
    auto const creditsHeadingText =
      utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::TrackCreditsHeading));
    auto const performerText =
      utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::TrackCreditPerformer));
    auto const renderedRows = [inspector, &utf8Of] { return utf8Of(inspectorDetailString(inspector.view)); };

    auto const detailSnapshot = [&](std::vector<ao::TrackId> ids)
    {
      return session.runtime()
        .workspace()
        .detailProjection(ao::rt::ExplicitSelectionTarget{.trackIds = std::move(ids)})
        ->snapshot();
    };

    // Copy the owning display inputs on the owner executor, without waiting or
    // retaining a read transaction across native rendering or authoring.
    auto renderFromSnapshot = [&]
    {
      auto optSeededRow = std::optional<ao::rt::TrackRow>{};
      {
        auto snapshot = session.runtime().library().snapshot();
        optSeededRow = snapshot.trackRow(trackId);
      }
      auto const detail = detailSnapshot({trackId});
      AO_INVARIANT(optSeededRow, "selected track must remain available to the inspector");
      [inspector renderSelectionCount:1 row:optSeededRow credits:detail.credits canReveal:YES];
    };

    // Bounded authoring seed through the public runtime authoring path; the
    // completion flag is written on the callback executor after both resumes.
    auto seedPerformanceMetadata =
      [&](ao::rt::MetadataPatch patch, char const* obligation, ao::TrackId targetId = ao::kInvalidTrackId)
    {
      auto bindingRes = session.runtime().library().bindTrackTargets(
        std::array<ao::TrackId, 1>{targetId == ao::kInvalidTrackId ? trackId : targetId});
      AO_INVARIANT(bindingRes, "the inspector performance seed must bind its fixture track");

      auto done = std::atomic<bool>{false};
      auto optFailure = std::optional<ao::Error>{};
      session.runtime().async().spawnLogged(
        seedPerformanceMetadataSubmissionAsync(&session, std::move(*bindingRes), std::move(patch), &done, &optFailure),
        "AppKit presentation inspector performance seed");
      ao::appkit::test::requireWaitUntil([&] { return done.load(); }, obligation);
      AO_INVARIANT(!optFailure, "{}: {}", obligation, optFailure->message);
    };

    // Absent initial: the raw scanned fixture carries neither field.
    renderFromSnapshot();
    AO_INVARIANT(!renderedRows().contains(dateLabelText), "an absent recording date must not render its row");
    AO_INVARIANT(!renderedRows().contains(creditsHeadingText), "an absent Credits list must not render its heading");

    // A stored release year must not infer a recording date on the native surface.
    seedPerformanceMetadata(
      ao::rt::MetadataPatch{.optYear = std::uint16_t{1970}}, "the inspector seed year must apply");
    renderFromSnapshot();
    AO_INVARIANT(
      renderedRows().contains(yearLabelText + "\n1970\n"), "the seeded release year must render its own row");
    AO_INVARIANT(!renderedRows().contains(dateLabelText), "a release year must not infer a recording date");

    // Year-only precision.
    seedPerformanceMetadata(ao::rt::MetadataPatch{.optRecordingDate = ao::library::RecordingDate{.year = 1981}},
                            "the inspector seed year-only recording date must apply");
    renderFromSnapshot();
    AO_INVARIANT(renderedRows().contains(dateLabelText + "\n1981\n"),
                 "a year-only recording date must render its stored precision");

    // Month precision replaces the coarser row.
    seedPerformanceMetadata(
      ao::rt::MetadataPatch{.optRecordingDate = ao::library::RecordingDate{.year = 1981, .month = 5}},
      "the inspector seed month recording date must apply");
    renderFromSnapshot();
    AO_INVARIANT(renderedRows().contains(dateLabelText + "\n1981-05\n"),
                 "a month-precision recording date must render its stored precision");
    AO_INVARIANT(
      !renderedRows().contains(dateLabelText + "\n1981\n"), "a replaced year-only recording date must not linger");

    // Full precision plus an ordered Credits list with a role, an absent
    // role, and a repeated credit. The same value restores that state after
    // the clear and selection-replacement checks below.
    auto const populatedPatch = ao::rt::MetadataPatch{
      .optRecordingDate = ao::library::RecordingDate{.year = 1981, .month = 5, .day = 12},
      .optCredits =
        ao::rt::CreditReplacement{
          .kinds = ao::uimodel::allTrackCreditKinds(),
          .entries = {{.name = "Shared leader", .kind = ao::library::CreditKind::Conductor, .role = "direction"},
                      {.name = "Ada", .role = "piano"},
                      {.name = "Bob"},
                      {.name = "Ada", .role = "piano"}}},
    };
    seedPerformanceMetadata(populatedPatch, "the inspector seed full recording date and credits must apply");
    renderFromSnapshot();
    AO_INVARIANT(renderedRows().contains(dateLabelText + "\n1981-05-12\n"),
                 "a full recording date must render its stored precision");
    AO_INVARIANT(
      !renderedRows().contains(dateLabelText + "\n1981-05\n"), "a replaced month recording date must not linger");
    AO_INVARIANT(renderedRows().contains(creditsHeadingText), "a present Credits list must render its heading");
    {
      auto const rows = renderedRows();
      auto const creditedWithPiano = performerText + "\nAda\npiano\n";
      auto const creditedRoleless = performerText + "\nBob\n";
      auto const firstAda = rows.find(creditedWithPiano);
      auto const bob = rows.find(creditedRoleless);
      auto const secondAda =
        firstAda == std::string::npos ? std::string::npos : rows.find(creditedWithPiano, firstAda + 1);
      AO_INVARIANT(firstAda != std::string::npos && bob != std::string::npos && secondAda != std::string::npos,
                   "the Credits rows must render every credited entry with its kind and optional role");
      AO_INVARIANT(firstAda < bob && bob < secondAda, "the Credits rows must preserve stored order with duplicates");
      AO_INVARIANT(
        countRenderedOccurrences(rows, creditedWithPiano) == std::size_t{2}, "a repeated credit must render twice");
    }

    // Clear both populated fields, then replace the inspector selection.
    // Copied row values are rendered only after the snapshot is released, and
    // that release happens before the restore wait below.
    auto* const owningDetail = inspectorDetailTextView(inspector.view);
    AO_INVARIANT(
      owningDetail != nil && owningDetail.editable == 0, "the inspector must own one non-editable detail text view");
    seedPerformanceMetadata(
      ao::rt::MetadataPatch{.optRecordingDate = ao::library::RecordingDate{},
                            .optCredits = ao::rt::CreditReplacement{.kinds = ao::uimodel::allTrackCreditKinds()}},
      "the inspector clear of recording date and credits must apply");
    auto clearedTitle = std::string{};
    auto optClearedRow = std::optional<ao::rt::TrackRow>{};
    {
      auto snapshot = session.runtime().library().snapshot();
      optClearedRow = snapshot.trackRow(trackId);
      auto const optCredits = snapshot.trackCredits(trackId);
      AO_INVARIANT(optClearedRow && !optClearedRow->recordingDate.isPresent(),
                   "clearing the recording date must store the absence sentinel");
      AO_INVARIANT(
        optCredits && optCredits->empty(), "clearing credits must replace the ordered list with an empty list");
      clearedTitle = optClearedRow->title;
    }
    auto const clearedSections = detailSnapshot({trackId}).credits;
    AO_INVARIANT(!clearedTitle.empty(), "the cleared inspector row must keep its track title");
    [inspector renderSelectionCount:1 row:optClearedRow credits:clearedSections canReveal:YES];
    {
      auto const cleared = renderedRows();
      AO_INVARIANT(inspectorDetailTextView(inspector.view) == owningDetail,
                   "the cleared render must replace text in the owning detail view");
      AO_INVARIANT(cleared.contains(clearedTitle + "\n") && cleared.contains(yearLabelText + "\n1970\n"),
                   "the cleared inspector must replace rows from the owning snapshot");
      AO_INVARIANT(!cleared.contains(dateLabelText) && !cleared.contains("1981-05") &&
                     !cleared.contains(dateLabelText + "\n1981\n"),
                   "a cleared recording date must not leave a partial or full row");
      AO_INVARIANT(
        !cleared.contains(creditsHeadingText) && !cleared.contains("\nAda\n") && !cleared.contains("\nBob\n"),
        "a cleared Credits list must not leave ordered or duplicate rows");
    }
    auto const emptySelection =
      utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::AppKitTrackDetails)) + "\n" +
      utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::AppKitSelectionDetailsHint)) + "\n";
    auto const multipleSelection =
      utf8Of(ao::appkit::catalogFormat(
        session.catalog(), ao::i18n::MessageId::AppKitTracksSelected, {{"count", std::size_t{2}}})) +
      "\n" + utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::AppKitSharedDetailsHint)) + "\n";
    [inspector renderSelectionCount:0 row:optClearedRow credits:clearedSections canReveal:YES];
    AO_INVARIANT(inspectorDetailTextView(inspector.view) == owningDetail && renderedRows() == emptySelection,
                 "an empty selection must replace the populated inspector text in the owning detail view");
    [inspector renderSelectionCount:2 row:optClearedRow credits:clearedSections canReveal:YES];
    AO_INVARIANT(inspectorDetailTextView(inspector.view) == owningDetail && renderedRows() == multipleSelection,
                 "a multiple selection must replace the empty-selection text in the owning detail view");
    [inspector renderSelectionCount:1 row:std::nullopt credits:{} canReveal:YES];
    AO_INVARIANT(inspectorDetailTextView(inspector.view) == owningDetail && renderedRows().empty(),
                 "a missing selection row must replace the previous detail text in the owning detail view");
    seedPerformanceMetadata(
      populatedPatch, "the inspector must restore the populated recording date and credits before scrolling");
    {
      auto snapshot = session.runtime().library().snapshot();
      auto const optRestoredRow = snapshot.trackRow(trackId);
      auto const optRestoredCredits = snapshot.trackCredits(trackId);
      AO_INVARIANT(optRestoredRow && populatedPatch.optRecordingDate &&
                     optRestoredRow->recordingDate == *populatedPatch.optRecordingDate,
                   "the restored recording date must match the populated seed");
      AO_INVARIANT(
        optRestoredCredits && populatedPatch.optCredits && *optRestoredCredits == populatedPatch.optCredits->entries,
        "the restored Credits list must match the ordered duplicate seed");
    }

    auto optRow = std::optional<ao::rt::TrackRow>{};
    {
      auto snapshot = session.runtime().library().snapshot();
      optRow = snapshot.trackRow(trackId);
    }
    auto const credits = detailSnapshot({trackId}).credits;
    AO_INVARIANT(optRow, "selected track must remain available to the inspector");

    auto const missingId = ao::TrackId{999999};
    AO_INVARIANT(!session.runtime().library().snapshot().containsTrack(missingId),
                 "The missing display target must not belong to the fixture library");
    auto const missingSelection = std::vector{trackId, missingId};
    auto const survivingDetail = detailSnapshot(missingSelection);
    [inspector renderSelectionCount:missingSelection.size()
                                row:std::nullopt
                            credits:survivingDetail.credits
                          canReveal:NO];
    AO_INVARIANT(renderedRows().contains("Shared leader\ndirection\n") &&
                   countRenderedOccurrences(renderedRows(), performerText + "\nAda\npiano\n") == 2,
                 "A missing display target must not erase the surviving track's full Credits sections");
    auto const missingBindingRes = session.runtime().library().bindTrackTargets(missingSelection);
    AO_INVARIANT(!missingBindingRes && missingBindingRes.error().code == ao::Error::Code::NotFound,
                 "Skipping missing display targets must not relax edit binding admission");
    auto const missingDetail = detailSnapshot({missingId});
    [inspector renderSelectionCount:1 row:std::nullopt credits:missingDetail.credits canReveal:NO];
    AO_INVARIANT(renderedRows().empty(), "An entirely missing display selection must clear the previous Credits");

    auto otherId = ao::kInvalidTrackId;

    for (std::size_t index = 0; index < session.displayIndex().displayCount(); ++index)
    {
      if (auto const* row = session.rowAt(index); row != nullptr && row->id != trackId)
      {
        otherId = row->id;
        break;
      }
    }

    AO_INVARIANT(otherId != ao::kInvalidTrackId, "Mixed display requires a second fixture track");
    auto const optOtherCredits = session.runtime().library().snapshot().trackCredits(otherId);
    AO_INVARIANT(optOtherCredits, "The second display fixture track must remain available");
    seedPerformanceMetadata(
      ao::rt::MetadataPatch{
        .optCredits =
          ao::rt::CreditReplacement{
            .kinds = ao::uimodel::allTrackCreditKinds(),
            .entries = {{.name = "Shared leader", .kind = ao::library::CreditKind::Conductor, .role = "direction"},
                        {.name = "Different performer", .role = "cello"}}}},
      "The second track must supply common and mixed display sections",
      otherId);
    auto const mixedDetail = detailSnapshot({trackId, otherId, missingId});
    [inspector renderSelectionCount:3 row:std::nullopt credits:mixedDetail.credits canReveal:NO];
    auto const multipleValuesText =
      utf8Of(ao::appkit::catalogText(session.catalog(), ao::i18n::MessageId::TrackMultipleValues));
    AO_INVARIANT(renderedRows().contains("Shared leader\ndirection\n") &&
                   renderedRows().contains(performerText + "\n" + multipleValuesText + "\n") &&
                   !renderedRows().contains("\nAda\n") && !renderedRows().contains("\nDifferent performer\n"),
                 "Mixed native details must retain common kinds and never show a first-target list or union");
    seedPerformanceMetadata(
      ao::rt::MetadataPatch{.optCredits = ao::rt::CreditReplacement{.kinds = ao::uimodel::allTrackCreditKinds(),
                                                                    .entries = *optOtherCredits}},
      "The second display fixture must restore its original Credits",
      otherId);

    for (std::int32_t line = 0; line < 80; ++line)
    {
      optRow->genre += "Long metadata scroll probe\n";
    }

    inspector.view.frame = NSMakeRect(0, 0, 270, 340);
    [inspector renderSelectionCount:1 row:optRow credits:credits canReveal:YES];
    [inspector renderArtwork:session.state().selectedCover];
    [inspector layoutForModern:YES];
    [inspector presentSheetForWindow:window];
    ao::appkit::test::requireWaitUntil([&]
                                       { return window.attachedSheet == inspector.sheet && inspector.sheet != nil; },
                                       "compact inspector sheet did not attach to its host window");
    auto* const scroll =
      static_cast<NSScrollView*>(ao::appkit::test::findView(inspector.sheet.contentView, @"compact-inspector-scroll"));
    AO_INVARIANT(
      [scroll isKindOfClass:NSScrollView.class] != 0, "compact inspector must expose its metadata scroll view");
    AO_INVARIANT(scroll.hasVerticalScroller != 0, "compact inspector metadata must be scrollable");
    auto* const text = static_cast<NSTextView*>(scroll.documentView);
    AO_INVARIANT([text isKindOfClass:NSTextView.class] != 0, "inspector scroll view must own metadata text");
    [text.layoutManager ensureLayoutForTextContainer:text.textContainer];
    [text scrollRangeToVisible:NSMakeRange(text.string.length - 1, 1)];
    AO_INVARIANT(text.bounds.size.height > scroll.contentView.bounds.size.height,
                 "long inspector metadata must exceed the compact viewport");
    AO_INVARIANT(
      scroll.contentView.bounds.origin.y > 0, "the end of long inspector metadata must be reachable by scrolling");
    ao::appkit::test::captureView(inspector.sheet.contentView, stateRoot / "appkit-inspector-sheet.png");

    auto* const done = static_cast<NSButton*>(inspector.sheet.defaultButtonCell.controlView);
    AO_INVARIANT([done isKindOfClass:NSButton.class] != 0, "The compact inspector must expose its default Done button");
    auto const returnHandled = [done performKeyEquivalent:keyEvent(inspector.sheet, @"\r", 36)];
    AO_INVARIANT(returnHandled != 0, "The Done button must accept its native Return key equivalent");
    ao::appkit::test::requireWaitUntil(
      [&] { return window.attachedSheet == nil; }, "Return did not detach the compact inspector sheet");
    AO_INVARIANT(dismissCount == 1, "Return dismissal must invoke its handler once");

    [inspector presentSheetForWindow:window];
    AO_INVARIANT(window.attachedSheet == inspector.sheet, "inspector sheet must support an Escape presentation");
    [inspector.sheet cancelOperation:nil];
    ao::appkit::test::requireWaitUntil(
      [&] { return window.attachedSheet == nil; }, "Escape did not detach the compact inspector sheet");
    AO_INVARIANT(dismissCount == 2, "Escape dismissal must invoke its handler once");

    [inspector presentSheetForWindow:window];
    AO_INVARIANT(
      window.attachedSheet == inspector.sheet, "inspector sheet must support a VoiceOver Escape presentation");
    auto const accessibilityEscaped = [inspector.sheet accessibilityPerformCancel];
    AO_INVARIANT(accessibilityEscaped != 0, "VoiceOver Escape must report handled");
    ao::appkit::test::requireWaitUntil(
      [&] { return window.attachedSheet == nil; }, "VoiceOver Escape did not detach the compact inspector sheet");
    AO_INVARIANT(dismissCount == 3, "VoiceOver Escape dismissal must invoke its handler once");
    AO_INVARIANT(
      revealCount == 0 && propertiesCount == 0, "keyboard inspector dismissal must not invoke unrelated actions");

    [inspector presentSheetForWindow:window];
    AO_INVARIANT(window.attachedSheet == inspector.sheet, "inspector sheet must support a final presentation");
    [inspector detach];
    ao::appkit::test::requireWaitUntil([&] { return window.attachedSheet == nil && inspector.sheet == nil; },
                                       "inspector detach must end an attached sheet");
    AO_INVARIANT(dismissCount == 3, "inspector detach must clear handlers before ending its sheet");
    [inspector renderSelectionCount:1 row:optRow credits:credits canReveal:YES];
    [inspector layoutForModern:NO];
  }

  void exerciseBrowserCells(ao::appkit::LibrarySession& session, AobusLibraryBrowser* browser)
  {
    auto selectPresentation = [&](std::string const& identifier)
    {
      auto const viewId = session.runtime().workspace().snapshot().activeViewId;
      auto viewRes = session.runtime().views().findTrackListState(viewId);
      AO_INVARIANT(viewRes, "browser cell scenario requires an active track-list view");

      if (viewRes->presentation.id == identifier)
      {
        return;
      }

      auto const revision = session.state().tableRevision;
      auto const presentationRes = session.setPresentation(identifier);
      AO_INVARIANT(presentationRes, "The native presentation choice must be accepted");
      ao::appkit::test::requireWaitUntil(
        [&]
        {
          auto const stateRes = session.runtime().views().findTrackListState(viewId);
          return stateRes && stateRes->presentation.id == identifier && session.state().tableRevision != revision;
        },
        "browser presentation did not publish");
    };
    auto groupRow = [&]
    {
      for (NSInteger row = 0; row < browser.tracks.numberOfRows; ++row)
      {
        if ([browser tableView:browser.tracks isGroupRow:row] != 0)
        {
          return row;
        }
      }

      return NSInteger{-1};
    };
    auto directTextFieldCount = [](NSView* view)
    {
      NSUInteger count = 0;
      auto* const subviews = view.subviews;

      for (NSUInteger index = 0; index < subviews.count; ++index)
      {
        auto* const subview = subviews[index];
        count += [subview isKindOfClass:NSTextField.class] != 0 ? 1 : 0;
      }

      return count;
    };
    auto visibleTitleCells = [&]
    {
      auto* const cells = [NSMutableArray<NSView*> array];
      auto const titleColumn = [browser.tracks columnWithIdentifier:@"Title"];
      auto const rows = [browser.tracks rowsInRect:browser.tracks.visibleRect];

      if (titleColumn < 0 || rows.location == NSNotFound)
      {
        return cells;
      }

      for (NSUInteger row = rows.location; row < NSMaxRange(rows); ++row)
      {
        auto const* track = session.rowAt(row);

        if (track == nullptr)
        {
          continue;
        }

        if (auto* const view = [browser.tracks viewAtColumn:titleColumn
                                                        row:static_cast<NSInteger>(row)
                                            makeIfNecessary:NO];
            view != nil)
        {
          auto* const trackCell = static_cast<NSTableCellView*>(view);
          AO_INVARIANT([trackCell.identifier isEqualToString:@"Title"] != 0 &&
                         [trackCell.textField.stringValue isEqualToString:ao::appkit::nativeText(track->title)] != 0,
                       "identified title cells must refresh from their current projected row");
          [cells addObject:view];
        }
      }

      return cells;
    };

    selectPresentation("albums");
    [browser layoutForModern:YES browserWidth:760];
    [browser refresh];
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto const modernGroupRow = groupRow();
    AO_INVARIANT(modernGroupRow >= 0, "album presentation must expose a group row");
    auto* const modernGroup = [browser tableView:browser.tracks viewForTableColumn:nil row:modernGroupRow];
    AO_INVARIANT([modernGroup.identifier isEqualToString:@"track-group-modern"] != 0 &&
                   [browser tableView:browser.tracks heightOfRow:modernGroupRow] == 44 &&
                   [browser.tracks rectOfRow:modernGroupRow].size.height == 44 &&
                   directTextFieldCount(modernGroup) == 2,
                 "modern group cells must have a reusable two-line presentation");

    auto* const originalCells = visibleTitleCells();
    AO_INVARIANT(originalCells.count >= 2, "browser reuse scenario requires visible track title cells");
    [browser.tracks reloadData];
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto* const reloadedCells = visibleTitleCells();
    AO_INVARIANT(reloadedCells.count == originalCells.count,
                 "reloading reusable cells must preserve the visible title-cell population");

    auto const originalScrollFrame = browser.trackScroll.frame;
    auto compactScrollFrame = originalScrollFrame;
    compactScrollFrame.size.height = 96;
    browser.trackScroll.frame = compactScrollFrame;
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks scrollRowToVisible:0];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto const firstVisibleRows = [browser.tracks rowsInRect:browser.tracks.visibleRect];
    AO_INVARIANT(visibleTitleCells().count > 0, "the compact browser viewport must show track cells");
    [browser.tracks scrollRowToVisible:browser.tracks.numberOfRows - 1];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    auto const lastVisibleRows = [browser.tracks rowsInRect:browser.tracks.visibleRect];
    AO_INVARIANT(lastVisibleRows.location > firstVisibleRows.location && visibleTitleCells().count > 0,
                 "scrolling must populate cells with a different set of projected tracks");
    browser.trackScroll.frame = originalScrollFrame;
    [browser.tracks scrollRowToVisible:0];

    [browser layoutForModern:NO browserWidth:620];
    [browser refresh];
    auto const classicGroupRow = groupRow();
    AO_INVARIANT(classicGroupRow >= 0, "classic album presentation must retain a group row");
    auto* const classicGroup = static_cast<NSTableCellView*>([browser tableView:browser.tracks
                                                             viewForTableColumn:nil
                                                                            row:classicGroupRow]);
    AO_INVARIANT([classicGroup.identifier isEqualToString:@"track-group-classic"] != 0 &&
                   [browser tableView:browser.tracks heightOfRow:classicGroupRow] == 38 &&
                   [browser.tracks rectOfRow:classicGroupRow].size.height == 38 &&
                   directTextFieldCount(classicGroup) == 1 &&
                   [classicGroup.textField.stringValue containsString:@"·"] != 0,
                 "classic mode must refresh group height and one-line cell state");

    auto const projectionRevision = session.state().tableRevision;
    selectPresentation("songs");
    AO_INVARIANT(session.state().tableRevision == projectionRevision + 1,
                 "A replacement projection's synchronous subscription reset must rebuild rows exactly once");
    [browser refresh];

    for (NSInteger row = 0; row < browser.tracks.numberOfRows; ++row)
    {
      AO_INVARIANT([browser tableView:browser.tracks isGroupRow:row] == 0,
                   "songs projection must replace grouped rows with tracks");
    }

    AO_INVARIANT(browser.tracks.numberOfRows == static_cast<NSInteger>(session.displayIndex().displayCount()),
                 "projection refresh must preserve the native row/data-index contract");
    [browser.trackScroll layoutSubtreeIfNeeded];
    [browser.tracks layoutSubtreeIfNeeded];
    [browser.tracks displayIfNeeded];
    AO_INVARIANT(visibleTitleCells().count >= 2, "projection refresh must repopulate identified title cells");
    selectPresentation("albums");
    [browser refresh];
    [browser layoutForModern:YES browserWidth:760];
  }

  ao::TrackId exerciseBrowser(ao::appkit::LibrarySession& session,
                              AobusLibraryBrowser* browser,
                              AobusPresentationBrowserDelegate* delegate)
  {
    exerciseBrowserCells(session, browser);
    [browser layoutForModern:YES browserWidth:760];
    [browser refresh];
    AO_INVARIANT(browser.tracks.numberOfRows == static_cast<NSInteger>(session.displayIndex().displayCount()),
                 "library table rows must reflect the current display index");
    std::size_t selectedIndex = 0;
    auto selectedId = ao::kInvalidTrackId;

    for (std::size_t index = 0; index < session.displayIndex().displayCount(); ++index)
    {
      if (auto const* row = session.rowAt(index); row != nullptr)
      {
        selectedIndex = index;
        selectedId = row->id;
        break;
      }
    }

    AO_INVARIANT(selectedId != ao::kInvalidTrackId, "library scenario requires a selectable track row");
    [browser.tracks selectRowIndexes:[NSIndexSet indexSetWithIndex:selectedIndex] byExtendingSelection:NO];
    ao::appkit::test::requireWaitUntil([&] { return session.selection() == std::vector{selectedId}; },
                                       "native table selection did not reach the library session");
    AO_INVARIANT(browser.selectedTrackIds == std::vector<ao::TrackId>{selectedId},
                 "browser selected-track state must reflect the session selection");
    AO_INVARIANT(delegate.selectionChangeCount > 0, "native library selection must notify its delegate");

    auto const initialRevision = session.state().tableRevision;
    auto* const titleSort = [[NSSortDescriptor alloc] initWithKey:@"Title" ascending:NO];
    browser.tracks.sortDescriptors = @[titleSort];
    ao::appkit::test::requireWaitUntil(
      [&]
      {
        auto const viewId = session.runtime().workspace().snapshot().activeViewId;
        auto const viewRes = session.runtime().views().findTrackListState(viewId);
        return viewRes && session.state().tableRevision != initialRevision &&
               viewRes->presentation.id == "appkit-column-sort" && viewRes->presentation.sortBy.size() == 1 &&
               viewRes->presentation.sortBy.front().field == ao::rt::TrackSortField::Title &&
               !viewRes->presentation.sortBy.front().ascending;
      },
      "title column sort did not update the shared presentation state");
    [browser refresh];
    AO_INVARIANT([browser.presentationIdentifier isEqualToString:@"appkit-column-sort"] != 0,
                 "browser presentation identifier must reflect the shared sort state");
    AO_INVARIANT(browser.tracks.sortDescriptors.count == 1 &&
                   [browser.tracks.sortDescriptors.firstObject.key isEqualToString:@"Title"] != 0 &&
                   browser.tracks.sortDescriptors.firstObject.ascending == 0,
                 "browser sort descriptor must reflect the shared presentation state");
    AO_INVARIANT(
      session.selection() == std::vector<ao::TrackId>{selectedId}, "sorting must preserve the selected track identity");
    auto const titleRevision = session.state().tableRevision;
    browser.tracks.sortDescriptors = @[[[NSSortDescriptor alloc] initWithKey:@"#" ascending:YES]];
    ao::appkit::test::requireWaitUntil(
      [&]
      {
        auto const viewId = session.runtime().workspace().snapshot().activeViewId;
        auto const viewRes = session.runtime().views().findTrackListState(viewId);
        return viewRes && session.state().tableRevision != titleRevision && viewRes->presentation.sortBy.size() == 2 &&
               viewRes->presentation.sortBy[0].field == ao::rt::TrackSortField::DiscNumber &&
               viewRes->presentation.sortBy[0].ascending &&
               viewRes->presentation.sortBy[1].field == ao::rt::TrackSortField::TrackNumber &&
               viewRes->presentation.sortBy[1].ascending;
      },
      "The number column must sort disc before track within a multi-disc album");
    [browser refresh];
    AO_INVARIANT(browser.tracks.sortDescriptors.count == 1 &&
                   [browser.tracks.sortDescriptors.firstObject.key isEqualToString:@"#"] != 0 &&
                   browser.tracks.sortDescriptors.firstObject.ascending != 0,
                 "The compound disc/track sort must reflect as one native number-column descriptor");
    AO_INVARIANT(session.selection() == std::vector<ao::TrackId>{selectedId},
                 "The compound number-column sort must preserve the selected track identity");
    return selectedId;
  }

  ao::ListId exerciseBrowserDrops(ao::appkit::LibrarySession& session,
                                  AobusLibraryBrowser* browser,
                                  AobusPresentationBrowserDelegate* delegate,
                                  id<NSDraggingInfo> info,
                                  ao::TrackId const trackId)
  {
    auto& model = session.editor();
    auto const admissionRes = model.beginList();
    AO_INVARIANT(admissionRes, "The drop scenario must begin a writable List draft");
    model.editList("Drop target", "", "#appkit_drop_membership");
    auto const initialRevision = session.state().listRevision;
    model.save();
    ao::appkit::test::requireWaitUntil(
      [&] { return model.state().completed && session.state().listRevision != initialRevision; },
      "The drop target List must be saved and published");
    auto const listId = model.state().savedListId;
    AO_INVARIANT(listId != ao::kInvalidListId, "The drop target must have a durable List identity");
    model.cancel();
    [browser refresh];

    auto const writer = [browser tableView:browser.tracks pasteboardWriterForRow:browser.tracks.selectedRow];
    AO_INVARIANT(writer != nil, "The selected track must expose its native drag payload");
    auto const written = [info.draggingPasteboard writeObjects:@[writer]];
    AO_INVARIANT(written != NO, "The isolated drag pasteboard must accept the native track payload");
    auto* const item = @(listId.raw());
    auto const operation = [browser outlineView:browser.lists
                                   validateDrop:info
                                   proposedItem:item
                             proposedChildIndex:NSOutlineViewDropOnItemIndex];
    auto const accepted = [browser outlineView:browser.lists
                                    acceptDrop:info
                                          item:item
                                    childIndex:NSOutlineViewDropOnItemIndex];
    AO_INVARIANT(operation == NSDragOperationCopy && accepted != NO && delegate.membershipRequestCount == 1 &&
                   delegate.membershipTrackIds == std::vector{trackId} && delegate.membershipListId == listId,
                 "A writable List drop must request membership exactly once with the captured track and List IDs");

    auto const requireRejected = [&](char const* obligation)
    {
      auto const rejectedOperation = [browser outlineView:browser.lists
                                             validateDrop:info
                                             proposedItem:item
                                       proposedChildIndex:NSOutlineViewDropOnItemIndex];
      auto const rejected = [browser outlineView:browser.lists
                                      acceptDrop:info
                                            item:item
                                      childIndex:NSOutlineViewDropOnItemIndex];
      AO_INVARIANT(rejectedOperation == NSDragOperationNone && rejected == NO && delegate.membershipRequestCount == 1,
                   "{}",
                   obligation);
    };
    delegate.sheetBlocked = YES;
    requireRejected("A sheet-blocked browser must reject validation and acceptance without a membership request");
    delegate.sheetBlocked = NO;
    delegate.closing = YES;
    requireRejected("A closing browser must reject validation and acceptance without a membership request");
    delegate.closing = NO;

    auto const renameAdmissionRes = model.beginList(listId);
    AO_INVARIANT(renameAdmissionRes, "The drop target List must reopen for a revision change");
    model.editList("Renamed drop target", "", "#appkit_drop_membership");
    auto const listRevision = session.state().listRevision;
    model.save();
    ao::appkit::test::requireWaitUntil(
      [&] { return model.state().completed && session.state().listRevision != listRevision; },
      "The changed drop target must publish a new List revision");
    model.cancel();
    requireRejected("A stale browser must reject validation and acceptance until its List projection is refreshed");
    [browser refresh];
    auto const refreshedOperation = [browser outlineView:browser.lists
                                            validateDrop:info
                                            proposedItem:item
                                      proposedChildIndex:NSOutlineViewDropOnItemIndex];
    AO_INVARIANT(refreshedOperation == NSDragOperationCopy,
                 "Refreshing the List projection must restore admission for the same writable target");
    return listId;
  }

  void exerciseDetachedBrowser(AobusLibraryBrowser* browser,
                               AobusPresentationBrowserDelegate* delegate,
                               NSUInteger const selectionChangeCount,
                               id<NSDraggingInfo> info,
                               ao::ListId const dropListId)
  {
    auto const membershipRequestCount = delegate.membershipRequestCount;
    AO_INVARIANT(browser.tracks.delegate == nil && browser.tracks.dataSource == nil && browser.tracks.target == nil &&
                   browser.lists.delegate == nil && browser.lists.dataSource == nil,
                 "library detach must revoke native callbacks and targets");
    AO_INVARIANT(
      [browser numberOfRowsInTableView:browser.tracks] == 0, "detached library data source must report no track rows");
    AO_INVARIANT([browser outlineView:browser.lists numberOfChildrenOfItem:nil] == 0,
                 "detached library outline must report no root children");
    AO_INVARIANT([browser tableView:browser.tracks heightOfRow:0] > 0 &&
                   [browser tableView:browser.tracks shouldSelectRow:0] == 0 &&
                   [browser tableView:browser.tracks isGroupRow:0] == 0,
                 "Late table layout and selection queries must remain safe after session release");
    id const inertChild = [browser outlineView:browser.lists child:0 ofItem:nil];
    AO_INVARIANT(inertChild != nil && inertChild == NSNull.null,
                 "detached outline child callback must return an inert nonnull object");
    AO_INVARIANT([browser outlineView:browser.lists isItemExpandable:inertChild] == 0 &&
                   [browser outlineView:browser.lists isGroupItem:inertChild] == 0 &&
                   [browser outlineView:browser.lists shouldSelectItem:inertChild] == 0,
                 "detached outline callbacks must return neutral selection and expansion state");
    AO_INVARIANT([browser outlineView:browser.lists
                   viewForTableColumn:browser.lists.outlineTableColumn
                                 item:inertChild] == nil,
                 "detached outline callback must not create a cell");
    AO_INVARIANT([browser tableView:browser.tracks viewForTableColumn:browser.tracks.tableColumns.firstObject
                                  row:0] == nil,
                 "detached table callback must not create a cell");
    auto* const tableNotification = [NSNotification notificationWithName:NSTableViewSelectionDidChangeNotification
                                                                  object:browser.tracks];
    auto* const outlineNotification = [NSNotification notificationWithName:NSOutlineViewSelectionDidChangeNotification
                                                                    object:browser.lists];
    [browser tableViewSelectionDidChange:tableNotification];
    [browser outlineViewSelectionDidChange:outlineNotification];
    [browser tableView:browser.tracks sortDescriptorsDidChange:@[]];
    [browser refresh];
    [browser invalidateProjection];
    [browser playSelectedTrack];
    AO_INVARIANT(browser.activeListId == ao::kInvalidListId && browser.clickedListId == ao::kInvalidListId &&
                   browser.selectedTrackIds.empty(),
                 "detached library public state must be neutral");
    AO_INVARIANT(
      [browser prepareContextMenu:[[NSMenu alloc] init]] == 0, "detached library must reject context menu preparation");
    auto const operation = [browser outlineView:browser.lists
                                   validateDrop:info
                                   proposedItem:@(dropListId.raw())
                             proposedChildIndex:NSOutlineViewDropOnItemIndex];
    auto const accepted = [browser outlineView:browser.lists
                                    acceptDrop:info
                                          item:@(dropListId.raw())
                                    childIndex:NSOutlineViewDropOnItemIndex];
    AO_INVARIANT(operation == NSDragOperationNone && accepted == NO,
                 "A detached browser must reject a formerly valid track drop after session release");
    AO_INVARIANT(delegate.selectionChangeCount == selectionChangeCount &&
                   delegate.membershipRequestCount == membershipRequestCount,
                 "late native callbacks after session release must not reach the delegate");
  }
  ao::uimodel::ListPresentations::Snapshot readPreferences(std::filesystem::path const& musicRoot)
  {
    auto store = ao::rt::ConfigStore{ao::rt::LibraryPaths{musicRoot}.databasePath() / "appkit-workspace.yaml",
                                     ao::rt::ConfigStore::OpenMode::ReadOnly};
    auto result = ao::uimodel::ListPresentations::Snapshot{};
    auto const loadedRes = store.load(
      ao::uimodel::kListPresentationsConfigGroup, result, ao::uimodel::ListPresentationPreferenceYamlSchema{});
    AO_INVARIANT(loadedRes && *loadedRes, "Preferences must be durably readable through the shared schema");
    return result;
  }

  std::filesystem::path workspaceStorePath(std::filesystem::path const& musicRoot)
  {
    return ao::rt::LibraryPaths{musicRoot}.databasePath() / "appkit-workspace.yaml";
  }

  std::string readText(std::filesystem::path const& path)
  {
    auto input = std::ifstream{path, std::ios::binary};
    AO_INVARIANT(input.is_open(), "The AppKit workspace store must be readable");
    auto text = std::ostringstream{};
    text << input.rdbuf();
    return std::move(text).str();
  }

  // A newer writer's group: the current schema rejects its version before reading entries.
  std::string writeFuturePreferenceVersion(std::filesystem::path const& musicRoot)
  {
    auto text = readText(workspaceStorePath(musicRoot));
    auto const group = text.find("trackView.presentations:");
    AO_INVARIANT(group != std::string::npos, "The preference group must be persisted before the version probe");
    constexpr auto kCurrentVersion = std::string_view{"version: 1"};
    auto const version = text.find(kCurrentVersion, group);
    AO_INVARIANT(version != std::string::npos, "The preference group must carry its current version");
    text.replace(version, kCurrentVersion.size(), "version: 99");
    auto output = std::ofstream{workspaceStorePath(musicRoot), std::ios::binary | std::ios::trunc};
    output << text;
    AO_INVARIANT(output.good(), "The future preference version must be written");
    return text.substr(group, text.find("version: 99", group) - group);
  }

  void exercisePresentationPreferences(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    using namespace ao;
    auto firstId = kInvalidListId;
    auto secondId = kInvalidListId;
    auto deletedId = kInvalidListId;
    auto exactWorkspaceSpec = rt::TrackPresentationSpec{};
    auto custom = rt::CustomTrackPresentationPreset{
      .label = "Native custom presentation",
      .basePresetId = "songs",
      .spec = rt::builtinTrackPresentationPreset("songs")->spec,
    };
    custom.spec.id = "appkit-preference-custom";
    custom.spec.sortBy.front().ascending = false;

    {
      auto fixture = appkit::test::SessionFixture{musicRoot, stateRoot};
      auto& session = fixture.session();
      auto& runtime = session.runtime();
      auto& model = session.editor();
      auto const createList = [&](std::string const& name)
      {
        auto const admissionRes = model.beginList();
        AO_INVARIANT(admissionRes);
        model.editList(name, "", "#appkit_preference_probe");
        model.save();
        appkit::test::requireWaitUntil([&] { return model.state().completed; }, "The preference List must save");
        auto const listId = model.state().savedListId;
        model.cancel();
        return listId;
      };
      firstId = createList("First preference List");
      secondId = createList("Second preference List");
      deletedId = createList("Deleted preference List");
      session.navigate(firstId);
      auto const songsRes = session.setPresentation("songs");
      AO_INVARIANT(songsRes && readPreferences(musicRoot).at(firstId) == "songs",
                   "An accepted normal choice must durably pin only the base List");
      auto const plainViewId = runtime.workspace().snapshot().activeViewId;
      auto const sortRes = session.sort(rt::TrackSortField::Title, false);
      AO_INVARIANT(sortRes && readPreferences(musicRoot).at(firstId) == "songs",
                   "A native column sort must not overwrite the pinned presentation");
      session.navigate(secondId);
      auto const artistsRes = session.setPresentation("artists");
      AO_INVARIANT(artistsRes);
      session.navigate(firstId);
      auto reusedRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(reusedRes && reusedRes->id == plainViewId && reusedRes->presentation.id == "appkit-column-sort",
                   "Normal navigation must preserve an existing plain view's exact transient presentation");
      appkit::test::settleNativeCallbacks();
      session.filter("no matching preference probe");
      appkit::test::requireWaitUntil(
        [&]
        {
          auto const stateRes = runtime.views().findTrackListState(plainViewId);
          return stateRes && !stateRes->filterExpression.empty();
        },
        "The quick filter must be applied before navigating to a new plain view");
      session.navigate(firstId);
      auto newPlainRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(newPlainRes && newPlainRes->id != plainViewId && newPlainRes->filterExpression.empty() &&
                     newPlainRes->presentation.id == "songs",
                   "A filtered view must not replace the new plain view's saved default");
      auto const backRes = runtime.workspace().goBack();
      AO_INVARIANT(backRes);
      auto replayRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(replayRes && replayRes->presentation.id == "appkit-column-sort" &&
                     replayRes->filterExpression.empty() && readPreferences(musicRoot).at(firstId) == "songs",
                   "History must replay exact specs without pinning them");
      auto const forwardRes = runtime.workspace().goForward();
      AO_INVARIANT(forwardRes);
      auto const beforeRejected = readPreferences(musicRoot);
      auto const unknownRes = session.setPresentation("unknown-preference-probe");
      AO_INVARIANT(!unknownRes && readPreferences(musicRoot) == beforeRejected,
                   "A rejected selection must leave the preference map unchanged");
      auto const autoRes = session.setPresentation("");
      AO_INVARIANT(
        autoRes && !readPreferences(musicRoot).contains(firstId), "Auto must clear rather than pin its spec");
      auto autoViewRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(autoViewRes && autoViewRes->presentation.id == "tagging",
                   "A tag-backed List's Auto command must apply its recommendation");
      auto const customRes = runtime.workspace().addCustomPreset(custom);
      AO_INVARIANT(customRes);
      session.navigate(secondId);
      auto const selectedCustomRes = session.setPresentation(custom.spec.id);
      AO_INVARIANT(selectedCustomRes && readPreferences(musicRoot).at(secondId) == custom.spec.id,
                   "Restorable custom presets must be eligible normal choices");
      auto const deletionAdmissionRes = model.beginDeletion(deletedId);
      AO_INVARIANT(deletionAdmissionRes);
      appkit::test::requireWaitUntil([&] { return !model.state().busy && model.state().optDeletion; },
                                     "The stale preference fixture must finish deletion preview");
      model.save();
      appkit::test::requireWaitUntil([&] { return model.state().completed; }, "The stale preference List must delete");
      model.cancel();
      session.navigate(rt::kAllTracksListId);
      auto const unavailableManualRes = session.setPresentation(std::string{rt::kListOrderTrackPresentationId});
      AO_INVARIANT(!unavailableManualRes, "All Tracks must reject the shared unavailable Manual Order choice");
      auto const allSortRes = session.sort(rt::TrackSortField::Title, false);
      AO_INVARIANT(allSortRes);
      exactWorkspaceSpec =
        runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId)->presentation;
      auto const allViewId = runtime.workspace().snapshot().activeViewId;

      for (auto const viewId : runtime.workspace().snapshot().openViews)
      {
        if (viewId != allViewId)
        {
          auto const closeRes = runtime.workspace().closeView(viewId);
          AO_INVARIANT(closeRes);
        }
      }

      // Hand-authored state through the same retained writer, including an offline deletion.
      auto const seededRes =
        runtime.workspaceConfigStore().save(uimodel::kListPresentationsConfigGroup,
                                            uimodel::ListPresentations::Snapshot{{firstId, "unavailable-custom"},
                                                                                 {secondId, custom.spec.id},
                                                                                 {deletedId, "songs"},
                                                                                 {rt::kAllTracksListId, "songs"}},
                                            uimodel::ListPresentationPreferenceYamlSchema{});
      AO_INVARIANT(seededRes);
      session.checkpoint();
      AO_INVARIANT(readPreferences(musicRoot).at(firstId) == "unavailable-custom",
                   "A later workspace checkpoint must preserve the preference sibling group");
    }

    {
      auto fixture = appkit::test::SessionFixture{musicRoot, stateRoot};
      auto& session = fixture.session();
      auto& runtime = session.runtime();
      auto restoredRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(restoredRes && restoredRes->presentation == exactWorkspaceSpec,
                   "Workspace restore must retain its exact spec instead of applying a different saved default");
      session.navigate(firstId);
      appkit::test::requireWaitUntil([&] { return session.state().optListPresentationId == "unavailable-custom"; },
                                     "The new view's picker must observe its unavailable saved preference");
      auto fallbackRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(fallbackRes && fallbackRes->presentation.id == "tagging" &&
                     session.state().optListPresentationId == "unavailable-custom",
                   "A new view must fall back while retaining the unavailable opaque id for its picker");
      auto& model = session.editor();
      auto const editRes = model.beginList(firstId);
      AO_INVARIANT(editRes);
      model.editList("Renamed preference List", "", model.state().list.expression);
      model.save();
      appkit::test::requireWaitUntil([&] { return model.state().completed; }, "The picker-less List edit must save");
      model.cancel();
      AO_INVARIANT(readPreferences(musicRoot).at(firstId) == "unavailable-custom",
                   "A picker-less editor must not replace a dangling presentation choice");
      session.navigate(secondId);
      auto customViewRes = runtime.views().findTrackListState(runtime.workspace().snapshot().activeViewId);
      AO_INVARIANT(customViewRes && customViewRes->presentation == custom.spec,
                   "The restored catalog must resolve a saved custom specification exactly");
      auto const songsRes = session.setPresentation("songs");
      AO_INVARIANT(songsRes);
      auto const loadedPreferences = readPreferences(musicRoot);
      AO_INVARIANT(loadedPreferences.at(firstId) == "unavailable-custom" &&
                     loadedPreferences.at(rt::kAllTracksListId) == "songs" && !loadedPreferences.contains(deletedId),
                   "An unrelated save must retain opaque/virtual ids and omit deleted ids pruned at restore");
      auto const deletionRes = model.beginDeletion(secondId);
      AO_INVARIANT(deletionRes);
      appkit::test::requireWaitUntil([&] { return !model.state().busy && model.state().optDeletion; },
                                     "Preference deletion must finish its preview");
      model.save();
      appkit::test::requireWaitUntil([&] { return model.state().completed; }, "Preference deletion must commit");
      model.cancel();
      AO_INVARIANT(!readPreferences(musicRoot).contains(secondId),
                   "A live committed List deletion must retire and persist its preference");
      session.navigate(firstId);
      auto const autoRes = session.setPresentation("");
      AO_INVARIANT(autoRes && !readPreferences(musicRoot).contains(firstId),
                   "Explicit Auto must clear even an unavailable persisted id");
      session.navigate(rt::kAllTracksListId);
      session.checkpoint();
    }

    auto const futureGroupPrefix = writeFuturePreferenceVersion(musicRoot);

    {
      auto fixture = appkit::test::SessionFixture{musicRoot, stateRoot};
      auto& session = fixture.session();
      session.navigate(firstId);
      auto const songsRes = session.setPresentation("songs");
      AO_INVARIANT(songsRes, "A rejected preference group must still accept a presentation choice");
      appkit::test::requireWaitUntil([&] { return session.state().optListPresentationId == "songs"; },
                                     "A rejected preference group must still pin the choice in memory");
      session.checkpoint();
      auto const persisted = readText(workspaceStorePath(musicRoot));
      AO_INVARIANT(persisted.contains(futureGroupPrefix + "version: 99"),
                   "A preference group that failed to load must not be overwritten by this session");
      session.navigate(rt::kAllTracksListId);
    }
  }
} // namespace

namespace ao::appkit::test
{
  std::int32_t runPresentationScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    AO_INVARIANT([NSThread isMainThread] != 0, "AppKit presentation scenario must run on the main thread");
    AO_INVARIANT(NSApp != nil, "AppKit presentation scenario requires an initialized NSApplication");
    auto* const window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 980, 720)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
    window.title = @"Aobus AppKit presentation scenario";
    window.releasedWhenClosed = NO;
    window.preventsApplicationTerminationWhenModal = NO;
    auto* const root = [[AobusFlippedView alloc] initWithFrame:window.contentView.bounds];
    root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = root;
    auto* const activityAnchor = [NSButton buttonWithTitle:@"Activity" target:nil action:nullptr];
    activityAnchor.frame = NSMakeRect(20, 660, 120, 30);
    [root addSubview:activityAnchor];
    auto* const browserDelegate = [[AobusPresentationBrowserDelegate alloc] init];
    auto* const draggingInfo = [[AobusPresentationDraggingInfo alloc] init];
    draggingInfo.draggingPasteboard = [NSPasteboard pasteboardWithUniqueName];
    id<NSDraggingInfo> const info = static_cast<id>(draggingInfo);
    auto dropListId = kInvalidListId;
    AobusLibraryBrowser* browser = nil;

    [window center];
    [window makeKeyAndOrderFront:nil];
    settleNativeCallbacks();

    exerciseActivityExpiration();
    exerciseSoulOcclusion();

    {
      auto fixture = SessionFixture{musicRoot, stateRoot};
      auto& session = fixture.session();
      browser = [[AobusLibraryBrowser alloc] initWithSession:session delegate:browserDelegate];
      browser.listScroll.frame = NSMakeRect(20, 100, 190, 540);
      browser.trackScroll.frame = NSMakeRect(220, 100, 740, 540);
      [root addSubview:browser.listScroll];
      [root addSubview:browser.trackScroll];
      auto const selectedId = exerciseBrowser(session, browser, browserDelegate);
      dropListId = exerciseBrowserDrops(session, browser, browserDelegate, info, selectedId);
      exerciseActivity(session, window, activityAnchor, stateRoot);
      exerciseInspector(session, window, selectedId, stateRoot);
      exercisePlaybackPresentation(session, window, root, stateRoot);
      exerciseSelectedArtwork(session, window, selectedId);
      [browser detach];
    }

    auto const selectionChangeCount = browserDelegate.selectionChangeCount;
    exerciseDetachedBrowser(browser, browserDelegate, selectionChangeCount, info, dropListId);
    exercisePresentationPreferences(musicRoot, stateRoot);
    [draggingInfo.draggingPasteboard releaseGlobally];
    [window orderOut:nil];
    [window close];
    settleNativeCallbacks();
    return 0;
  }
} // namespace ao::appkit::test
