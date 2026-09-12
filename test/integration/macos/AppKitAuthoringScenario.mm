// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/macos-appkit/AppKitText.h"
#include "app/macos-appkit/LibraryBrowser.h"
#include "app/macos-appkit/LibraryEditor.h"
#include "app/macos-appkit/LibrarySession.h"
#include "test/integration/macos/AppKitScenarioSupport.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

@interface AobusAuthoringBrowserDelegate : NSObject<AobusLibraryBrowserDelegate>
@end
@implementation AobusAuthoringBrowserDelegate
- (BOOL)isLibraryBrowserClosing:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  return NO;
}
- (BOOL)isLibraryBrowserSheetBlocked:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
  return NO;
}
- (void)libraryBrowserSelectionDidChange:(AobusLibraryBrowser*) [[maybe_unused]] browser
{
}
- (BOOL)libraryBrowser:(AobusLibraryBrowser*) [[maybe_unused]] browser
  requestMembershipForTracks:(std::vector<ao::TrackId> const&) [[maybe_unused]] trackIds
                      listId:(ao::ListId) [[maybe_unused]] listId
{
  return NO;
}
@end

namespace ao::appkit::test
{
  namespace
  {
    constexpr auto kWindowWidth = 960.0;
    constexpr auto kWindowHeight = 760.0;

    template<typename Admission>
    void requireAdmission(Admission const& admission, char const* obligation)
    {
      AO_INVARIANT(admission, "{}", obligation);
    }

    class EditorFixture final
    {
    public:
      EditorFixture(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
        : _sessionFixture{musicRoot, stateRoot}
        , _window{[[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, kWindowWidth, kWindowHeight)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO]}
      {
        _window.title = @"Aobus authoring scenario";
        _window.releasedWhenClosed = NO;
        [_window makeKeyAndOrderFront:nil];
        settleNativeCallbacks();
        AO_INVARIANT(_window.visible != 0, "The authoring scenario parent window must be visible");
      }

      ~EditorFixture()
      {
        AO_INVARIANT(_editor == nil, "The native editor must detach before the session shuts down");
        AO_INVARIANT(_window.attachedSheet == nil, "The parent must not retain an editor sheet at teardown");
        [_window orderOut:nil];
        [_window close];
      }

      EditorFixture(EditorFixture const&) = delete;
      EditorFixture& operator=(EditorFixture const&) = delete;
      EditorFixture(EditorFixture&&) = delete;
      EditorFixture& operator=(EditorFixture&&) = delete;

      LibrarySession& session() { return _sessionFixture.session(); }

      LibraryEditorModel& model() { return session().editor(); }

      NSWindow* window() const { return _window; }

      AobusLibraryEditor* editor() const { return _editor; }

      void present(BOOL modern = YES)
      {
        AO_INVARIANT(_editor == nil, "Only one native editor may be presented by the fixture");
        AO_INVARIANT(_window.attachedSheet == nil, "The parent must be free before presenting an editor");
        AO_INVARIANT(model().state().kind != LibraryEditorKind::None,
                     "The editor model must begin a transaction before presentation");
        _editor = [[AobusLibraryEditor alloc] initWithModel:model() parent:_window modern:modern artwork:nil];
        [_editor present];
        settleNativeCallbacks();
        AO_INVARIANT(_editor.window != nil, "Presenting an editor must create its panel");
        AO_INVARIANT(_window.attachedSheet == _editor.window, "The editor panel must attach to its parent window");
      }

      void finish()
      {
        AO_INVARIANT(_editor != nil, "Finishing requires a presented editor");
        AO_INVARIANT(!model().state().busy, "An editor cannot finish while authoring work is busy");
        [_editor finish];
        settleNativeCallbacks();
        AO_INVARIANT(_editor.window == nil, "Finishing must release the editor panel");
        AO_INVARIANT(_window.attachedSheet == nil, "Finishing must detach the editor sheet");
        AO_INVARIANT(model().state().kind == LibraryEditorKind::None,
                     "Finishing must cancel the completed or abandoned model transaction");
        _editor = nil;
      }

      void detachClosed()
      {
        AO_INVARIANT(_editor != nil, "Detaching requires an editor owner");
        settleNativeCallbacks();
        AO_INVARIANT(_editor.window == nil, "A completed close transaction must release the panel");
        AO_INVARIANT(_window.attachedSheet == nil, "A completed close transaction must detach its sheet");
        AO_INVARIANT(
          model().state().kind == LibraryEditorKind::None, "A completed close transaction must reset the model");
        _editor = nil;
      }

    private:
      SessionFixture _sessionFixture;
      NSWindow* _window = nil;
      AobusLibraryEditor* _editor = nil;
    };

    NSTextField* requireTextField(EditorFixture const& fixture, NSString* identifier)
    {
      auto* const control = findControl(fixture.editor().window.contentView, identifier);
      AO_INVARIANT(control != nil, "The editor must expose the requested semantic control");
      AO_INVARIANT(
        [control isKindOfClass:NSTextField.class] != 0, "The requested semantic control must be a text field");
      return static_cast<NSTextField*>(control);
    }

    std::vector<TrackId> firstThreeTracks(LibrarySession& session)
    {
      auto ids = std::vector<TrackId>{};

      for (std::size_t index = 0; index < session.displayIndex().displayCount() && ids.size() < 3; ++index)
      {
        if (auto const* row = session.rowAt(index); row != nullptr)
        {
          ids.push_back(row->id);
        }
      }

      AO_INVARIANT(ids.size() == 3, "The authoring scenario requires three scanned tracks");
      return ids;
    }

    struct CloseObservation final
    {
      std::int32_t count = 0;
      bool closed = true;
    };

    void verifyEditorCloseContract(EditorFixture& fixture)
    {
      auto& model = fixture.model();
      requireAdmission(model.beginList(), "A clean list editor must begin");
      fixture.present();
      __block std::int32_t cleanCallbackCount = 0;
      __block BOOL cleanClosed = NO;
      [fixture.editor() requestCloseWithCompletion:^(BOOL closed) {
        ++cleanCallbackCount;
        cleanClosed = closed;
      }];
      settleNativeCallbacks();
      AO_INVARIANT(
        cleanCallbackCount == 1 && cleanClosed != 0, "A clean close request must complete exactly once with success");
      fixture.detachClosed();

      requireAdmission(model.beginList(), "A dirty list editor must begin");
      fixture.present();
      editText(fixture.editor().window, @"Name", @"Discard confirmation regression");
      AO_INVARIANT(model.state().dirty, "Editing a list name must dirty the editor model");

      auto const dirtyClosePtr = std::make_shared<CloseObservation>();
      [fixture.editor() requestCloseWithCompletion:^(BOOL closed) {
        ++dirtyClosePtr->count;
        dirtyClosePtr->closed = closed != NO;
      }];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "A dirty close request must present discard confirmation");
      AO_INVARIANT(dirtyClosePtr->count == 0, "Discard confirmation must resolve before close completion");
      auto* const dirtyConfirmation = fixture.editor().window.attachedSheet;
      AO_INVARIANT([dirtyConfirmation.defaultButtonCell.title isEqualToString:@"Keep Editing"] != 0,
                   "An ordinary dirty draft must retain the Keep Editing choice");
      [fixture.editor().window endSheet:dirtyConfirmation returnCode:NSAlertFirstButtonReturn];
      requireWaitUntil([&] { return dirtyClosePtr->count == 1 && fixture.editor().window.attachedSheet == nil; },
                       "Keep Editing must resolve the first close request");
      settleNativeCallbacks();
      AO_INVARIANT(
        dirtyClosePtr->count == 1 && !dirtyClosePtr->closed, "Keep Editing must complete exactly once without closing");
      AO_INVARIANT(fixture.editor().window != nil && fixture.window().attachedSheet == fixture.editor().window &&
                     model.state().dirty,
                   "Keep Editing must retain the dirty editor transaction");

      [fixture.editor() cancel:nil];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "The editor must open its own discard confirmation before lifecycle admission");
      auto* const existingConfirmation = fixture.editor().window.attachedSheet;
      [fixture.editor() requestCloseWithCompletion:^(BOOL closed) {
        ++dirtyClosePtr->count;
        dirtyClosePtr->closed = closed != NO;
      }];
      AO_INVARIANT(dirtyClosePtr->count == 1 && fixture.editor().window.attachedSheet == existingConfirmation,
                   "A lifecycle request must join the existing confirmation without reporting cancellation");
      [fixture.editor() finish];
      [fixture.editor() finish];
      requireWaitUntil([&] { return dirtyClosePtr->count == 2 && fixture.editor().window == nil; },
                       "External repeated finish must resolve the pending close once");
      settleNativeCallbacks();
      AO_INVARIANT(dirtyClosePtr->count == 2 && dirtyClosePtr->closed,
                   "External repeated finish must not duplicate close completion");
      fixture.detachClosed();
    }

    void verifyDiscardRejectsSave(EditorFixture& fixture)
    {
      auto& model = fixture.model();
      requireAdmission(model.beginList(), "The discard/save regression draft must begin");
      fixture.present();
      editText(fixture.editor().window, @"Name", @"Discard without saving");
      auto const closePtr = std::make_shared<CloseObservation>();
      [fixture.editor() requestCloseWithCompletion:^(BOOL closed) {
        ++closePtr->count;
        closePtr->closed = closed != NO;
      }];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "The close request must present discard confirmation");

      // Programmatic actions must respect the same modal admission as native input.
      [fixture.editor() save:nil];
      AO_INVARIANT(!model.state().busy && model.state().dirty && !model.state().completed && closePtr->count == 0,
                   "Save during confirmation must not submit work or cancel the close request");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertSecondButtonReturn];
      requireWaitUntil([&] { return closePtr->count == 1 && closePtr->closed; },
                       "Discard must resolve the lifecycle close successfully");
      fixture.detachClosed();
      AO_INVARIANT(closePtr->count == 1, "Discard must complete the close exactly once");
    }

    void verifyDeferredCloseAfterSave(EditorFixture& fixture)
    {
      auto& model = fixture.model();
      requireAdmission(model.beginList(), "The deferred close save draft must begin");
      fixture.present();
      editText(fixture.editor().window, @"Name", @"Deferred close saved list");
      [fixture.editor() save:nil];
      AO_INVARIANT(model.state().busy, "Saving must enter the pending authoring state before close admission");
      auto const savedClosePtr = std::make_shared<CloseObservation>();
      [fixture.editor() requestCloseWithCompletion:^(BOOL closed) {
        ++savedClosePtr->count;
        savedClosePtr->closed = closed != NO;
      }];
      AO_INVARIANT(savedClosePtr->count == 0 && fixture.editor().window != nil,
                   "Saving must defer close completion without abandoning the panel");
      requireWaitUntil([&] { return model.state().completed; }, "The admitted save must finish");
      auto const savedId = model.state().savedListId;
      auto const optSavedList = fixture.session().runtime().library().snapshot().listNode(savedId);
      AO_INVARIANT(optSavedList && optSavedList->name == "Deferred close saved list",
                   "Deferred close must retain the completed durable save");
      [fixture.editor() refresh];
      requireWaitUntil([&] { return savedClosePtr->count == 1 && savedClosePtr->closed; },
                       "Authoring completion must resume the close exactly once");
      fixture.detachClosed();

      requireAdmission(model.beginDeletion(savedId), "The disposable saved list must admit cleanup");
      requireWaitUntil([&] { return model.state().optDeletion && !model.state().busy; },
                       "The disposable saved list must finish deletion preview");
      model.save();
      requireWaitUntil([&] { return model.state().completed; }, "Disposable list cleanup must finish");
      model.cancel();

      requireAdmission(model.beginList(), "The failing deferred save draft must begin");
      fixture.present();
      editText(fixture.editor().window, @"Name", @"Keep failed close draft");
      editText(fixture.editor().window, @"Expression", @"(");
      [fixture.editor() save:nil];
      AO_INVARIANT(model.state().busy, "The invalid expression must be submitted before close admission");
      auto const failedClosePtr = std::make_shared<CloseObservation>();
      [fixture.editor() requestCloseWithCompletion:^(BOOL closed) {
        ++failedClosePtr->count;
        failedClosePtr->closed = closed != NO;
      }];
      AO_INVARIANT(failedClosePtr->count == 0, "A pending failed save must not be mistaken for user cancellation");
      requireWaitUntil([&] { return !model.state().busy && !model.state().error.empty(); },
                       "The invalid expression must return an authoring error");
      [fixture.editor() refresh];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "A failed save must resume close through a discard decision");
      AO_INVARIANT(failedClosePtr->count == 0 && model.state().dirty,
                   "The failed draft must survive until the actual user decision");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertFirstButtonReturn];
      requireWaitUntil([&] { return failedClosePtr->count == 1 && !failedClosePtr->closed; },
                       "Only Keep Editing may cancel the deferred close");
      AO_INVARIANT(fixture.editor().window != nil && model.state().list.name == "Keep failed close draft" &&
                     model.state().list.expression == "(",
                   "Cancelling deferred close must preserve the failed candidate");
      fixture.finish();
    }

    void verifyPendingTagClose(EditorFixture& fixture, TrackId trackId)
    {
      auto& model = fixture.model();
      requireAdmission(model.beginProperties({trackId}), "The token-only close draft must begin");
      fixture.present();
      auto const originalTags = model.state().tags;
      AO_INVARIANT(!model.state().dirty, "The token-only close regression must start with a clean draft");
      editText(fixture.editor().window, @"shared-tags", @"pending_close_tag");
      auto* const tags = requireTextField(fixture, @"shared-tags");
      AO_INVARIANT(tags.currentEditor != nil && model.state().tags == originalTags && model.state().dirty,
                   "An uncommitted token alone must dirty the draft without replacing committed tags");
      auto const closePtr = std::make_shared<CloseObservation>();
      auto const completion = ^(BOOL closed) {
        ++closePtr->count;
        closePtr->closed = closed != NO;
      };
      [fixture.editor() requestCloseWithCompletion:completion];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "Closing with only an uncommitted token must ask before discarding it");
      AO_INVARIANT(closePtr->count == 0, "An uncommitted token must prevent automatic close completion");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertFirstButtonReturn];
      requireWaitUntil([&] { return closePtr->count == 1; }, "Keep Editing must settle the token-only close request");
      AO_INVARIANT(!closePtr->closed && fixture.editor().window != nil && model.state().dirty &&
                     [tags.stringValue containsString:@"pending_close_tag"] != NO,
                   "Keep Editing must retain the token candidate and its dirty draft");
      [fixture.editor() requestCloseWithCompletion:completion];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "The retained token draft must still require a discard decision");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertSecondButtonReturn];
      requireWaitUntil([&] { return closePtr->count == 2 && closePtr->closed; },
                       "Discard must close the token-only draft exactly once");
      fixture.detachClosed();
      AO_INVARIANT(closePtr->count == 2 && fixture.session().runtime().library().snapshot().selectionTags(
                                             std::vector{trackId}) == originalTags,
                   "Discarding the native token candidate must leave durable tags unchanged");
    }

    void verifyCallbackExecutorSubmission(EditorFixture& fixture, TrackId trackId)
    {
      std::int32_t callbackCount = 0;
      bool callbacksOnMainThread = true;
      auto& session = fixture.session();
      auto model = LibraryEditorModel{session.runtime(),
                                      session.catalog(),
                                      [&]
                                      {
                                        callbacksOnMainThread = callbacksOnMainThread && [NSThread isMainThread] != 0;
                                        ++callbackCount;
                                      }};
      AO_INVARIANT(session.runtime().textOrderingPolicy() != nullptr,
                   "The AppKit runtime must borrow its locale ordering policy from the session");
      auto const beganPropertiesRes = model.beginProperties({trackId});
      AO_INVARIANT(beganPropertiesRes, "The callback-affinity property edit must begin");
      auto const field = std::ranges::find_if(model.state().fields,
                                              [](LibraryEditorField const& candidate)
                                              { return candidate.spec.field == rt::TrackField::Genre; });
      AO_INVARIANT(field != model.state().fields.end(), "The callback-affinity edit requires the Genre field");
      model.editField(
        static_cast<std::size_t>(std::distance(model.state().fields.begin(), field)), "Callback executor verified");
      auto const listRevision = session.state().listRevision;
      model.save();
      requireWaitUntil(
        [&] { return model.state().completed; }, "The callback-first TrackAuthoringSession submission must complete");
      AO_INVARIANT(session.state().listRevision == listRevision,
                   "A track mutation must not invalidate the independent List projection cache");
      auto const optRow = session.runtime().library().snapshot().trackRow(trackId);
      AO_INVARIANT(
        optRow && optRow->genre == "Callback executor verified" && callbackCount >= 4 && callbacksOnMainThread,
        "The callback-first submission must publish its durable edit and main-executor callbacks");
      model.cancel();
    }

    void addSharedTag(EditorFixture const& fixture, NSString* tag)
    {
      auto* const tags = requireTextField(fixture, @"shared-tags");
      NSArray* const existingTags = tags.objectValue;
      auto* const values = existingTags != nil ? [NSMutableArray arrayWithArray:existingTags] : [NSMutableArray array];

      if ([values containsObject:tag] == NO)
      {
        [values addObject:tag];
      }

      tags.objectValue = values;
      [fixture.editor()
        controlTextDidEndEditing:[NSNotification notificationWithName:NSControlTextDidEndEditingNotification
                                                               object:tags]];
    }

    void verifyPropertyAuthoring(EditorFixture& fixture,
                                 std::vector<TrackId> const& tracks,
                                 std::filesystem::path const& stateRoot)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();
      auto initialSnapshot = session.runtime().library().snapshot();
      auto const optFirstRow = initialSnapshot.trackRow(tracks[0]);
      auto const optThirdRow = initialSnapshot.trackRow(tracks[2]);
      AO_INVARIANT(optFirstRow && optThirdRow, "Scanned authoring tracks must remain readable");
      auto const originalFirstGenre = optFirstRow->genre;
      auto const originalThirdGenre = optThirdRow->genre;

      session.select({tracks[0], tracks[1]});
      requireAdmission(model.beginProperties({tracks[0], tracks[1]}), "Mixed properties must begin editing");
      fixture.present();
      auto* const disclosure = static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-3"));
      auto* const composer = requireTextField(fixture, @"composer");
      AO_INVARIANT(
        disclosure != nil && composer.hiddenOrHasHiddenAncestor != 0, "Secondary metadata must start collapsed");
      [disclosure performClick:nil];
      AO_INVARIANT(
        composer.hiddenOrHasHiddenAncestor == 0, "The native disclosure button must reveal secondary metadata");
      captureView(fixture.editor().window.contentView, stateRoot / "properties-expanded.png");
      [disclosure performClick:nil];
      AO_INVARIANT(
        composer.hiddenOrHasHiddenAncestor != 0, "The native disclosure button must collapse secondary metadata again");
      auto* const title = requireTextField(fixture, @"title");
      auto const titleIndex = static_cast<std::size_t>(title.tag);
      AO_INVARIANT(
        model.state().fields.at(titleIndex).mixed, "The first two fixture tracks must expose a mixed title value");
      NSString* const mixedPlaceholder = [title.placeholderString copy];
      AO_INVARIANT(mixedPlaceholder.length > 0, "A mixed field must have a visible placeholder");
      auto* const clearTitle = [[NSMenuItem alloc] init];
      clearTitle.tag = title.tag;
      [fixture.editor() clearField:clearTitle];
      AO_INVARIANT(model.state().fields.at(titleIndex).changed && model.state().fields.at(titleIndex).text.empty(),
                   "Clearing a mixed field must record an explicit empty edit");
      AO_INVARIANT(title.stringValue.length == 0 && [title.placeholderString isEqualToString:mixedPlaceholder] == 0,
                   "The explicit clear marker must replace the mixed-value placeholder");
      fixture.finish();

      requireAdmission(model.beginProperties({tracks[0], tracks[1]}), "Classic properties must begin editing");
      fixture.present(NO);
      AO_INVARIANT(fixture.editor().window.frame.size.width <= fixture.window().contentView.bounds.size.width,
                   "The Classic editor must fit its parent window");
      AO_INVARIANT(requireTextField(fixture, @"genre").editable != 0 &&
                     requireTextField(fixture, @"genre").controlSize == NSControlSizeRegular &&
                     requireTextField(fixture, @"composer").hiddenOrHasHiddenAncestor == 0,
                   "Classic presentation must retain compact controls and expanded secondary metadata");
      captureView(fixture.editor().window.contentView, stateRoot / "properties-classic.png");
      fixture.finish();

      requireAdmission(model.beginProperties({tracks[0], tracks[1]}), "Bulk properties must reopen after cancellation");
      fixture.present();
      editText(fixture.editor().window, @"genre", @"Native authoring verified");
      editText(fixture.editor().window, @"year", @"70000");
      addSharedTag(fixture, @"native_properties_probe");
      auto* const classification =
        static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-1"));
      AO_INVARIANT(classification != nil, "The year field must belong to a visible disclosure section");
      [classification performClick:nil];
      AO_INVARIANT(requireTextField(fixture, @"year").hiddenOrHasHiddenAncestor != 0,
                   "The year section must be collapsed before validating its field");
      [fixture.editor() save:nil];
      AO_INVARIANT(!model.state().busy && model.state().dirty && !model.state().error.empty(),
                   "An out-of-range year must fail synchronously without losing the draft");
      AO_INVARIANT(
        model.state().optInvalidField == rt::TrackField::Year, "Numeric validation must identify the year field");
      auto* const invalidYear = requireTextField(fixture, @"year");
      AO_INVARIANT(invalidYear.hiddenOrHasHiddenAncestor == 0 && invalidYear.currentEditor != nil &&
                     fixture.editor().window.firstResponder == invalidYear.currentEditor &&
                     [invalidYear.accessibilityHelp isEqualToString:nativeText(model.state().error)] != 0,
                   "Year validation must reveal, focus, and describe the specific invalid field");
      auto validationSnapshot = session.runtime().library().snapshot();
      auto const optValidationRow = validationSnapshot.trackRow(tracks[0]);
      AO_INVARIANT(optValidationRow && optValidationRow->genre == originalFirstGenre,
                   "Numeric validation failure must prevent partial property mutation");
      captureView(fixture.editor().window.contentView, stateRoot / "properties-validation.png");

      editText(fixture.editor().window, @"year", @"2026");
      AO_INVARIANT(
        model.state().error.empty() && !model.state().optInvalidField && invalidYear.accessibilityHelp == nil,
        "Correcting the invalid numeric field must clear its error and accessibility help");
      session.select({tracks[2]});
      AO_INVARIANT(session.selection() == std::vector{tracks[2]},
                   "The visible selection must change independently of the captured edit targets");
      [fixture.editor() save:nil];
      requireWaitUntil([&] { return model.state().completed; }, "The captured bulk property edit must complete");
      [fixture.editor() refresh];

      auto savedSnapshot = session.runtime().library().snapshot();
      auto const optSavedFirstRow = savedSnapshot.trackRow(tracks[0]);
      auto const optSavedSecondRow = savedSnapshot.trackRow(tracks[1]);
      auto const optSavedThirdRow = savedSnapshot.trackRow(tracks[2]);
      AO_INVARIANT(optSavedFirstRow && optSavedSecondRow && optSavedFirstRow->genre == "Native authoring verified" &&
                     optSavedSecondRow->genre == "Native authoring verified",
                   "The bulk edit must update both captured tracks");
      AO_INVARIANT(optSavedThirdRow && optSavedThirdRow->genre == originalThirdGenre,
                   "A later selection must not join the captured bulk edit");
      AO_INVARIANT(std::ranges::contains(
                     savedSnapshot.selectionTags(std::vector{tracks[0], tracks[1]}), "native_properties_probe"),
                   "The shared tag must be added to both captured tracks");
      AO_INVARIANT(
        !std::ranges::contains(savedSnapshot.selectionTags(std::vector{tracks[2]}), "native_properties_probe"),
        "The independent selection must not receive the captured shared tag");
      fixture.finish();
    }

    ListId verifyStaleDraft(EditorFixture& fixture,
                            std::vector<TrackId> const& tracks,
                            std::filesystem::path const& stateRoot)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();
      session.select({tracks[0], tracks[1]});
      requireAdmission(model.beginProperties({tracks[0], tracks[1]}), "The stale properties draft must begin");
      fixture.present();
      editText(fixture.editor().window, @"genre", @"Keep this stale candidate");
      editText(fixture.editor().window, @"shared-tags", @"pending_stale_tag");
      auto* const activeTags = requireTextField(fixture, @"shared-tags");
      AO_INVARIANT(activeTags.currentEditor != nil, "The stale regression requires an uncommitted token candidate");

      auto concurrentModelPtr = std::make_unique<LibraryEditorModel>(session.runtime(), session.catalog(), [] {});
      requireAdmission(concurrentModelPtr->beginList(), "The concurrent list mutation must begin");
      concurrentModelPtr->editList("Authoring probe", "", "#aobus_authoring_probe");
      auto const listRevision = session.state().listRevision;
      concurrentModelPtr->save();
      requireWaitUntil([&] { return concurrentModelPtr->state().completed && model.state().stale; },
                       "The concurrent list mutation must invalidate the open properties draft");
      AO_INVARIANT(session.state().listRevision > listRevision,
                   "A saved List mutation must advance the independent List revision");
      auto const listId = concurrentModelPtr->state().savedListId;
      AO_INVARIANT(listId != kInvalidListId, "The concurrent list mutation must publish its saved id");
      concurrentModelPtr->cancel();
      concurrentModelPtr.reset();

      AO_INVARIANT(model.state().dirty && !model.state().busy && !model.state().error.empty(),
                   "An invalidated draft must remain dirty, idle, and visibly stale");
      [fixture.editor() save:nil];
      AO_INVARIANT(!model.state().busy, "A stale draft must reject save admission");
      [fixture.editor() refresh];
      auto* const genre = requireTextField(fixture, @"genre");
      AO_INVARIANT([genre.stringValue isEqualToString:@"Keep this stale candidate"] != 0,
                   "Stale rendering must preserve the user's visible genre candidate");
      auto const savedTags = model.state().tags;
      model.editField(static_cast<std::size_t>(genre.tag), "Rejected stale input");
      model.editTags({"rejected_stale_tag"});
      AO_INVARIANT(model.state().fields.at(static_cast<std::size_t>(genre.tag)).text == "Keep this stale candidate" &&
                     model.state().tags == savedTags,
                   "A stale model must reject subsequent field and tag edits");
      auto* const frozenTags = requireTextField(fixture, @"shared-tags");
      AO_INVARIANT(
        frozenTags.editable == 0 && frozenTags.selectable != 0, "A stale token field must be read-only but selectable");
      AO_INVARIANT([static_cast<NSArray*>(frozenTags.objectValue) containsObject:@"pending_stale_tag"] != 0,
                   "A stale token field must retain its pending native candidate");
      AO_INVARIANT(!std::ranges::contains(model.state().tags, "pending_stale_tag"),
                   "The uncommitted native token candidate must not enter model state");
      auto* const clearGenre = [[NSMenuItem alloc] init];
      clearGenre.tag = genre.tag;
      [fixture.editor() clearField:clearGenre];
      AO_INVARIANT(genre.editable == 0 && genre.selectable != 0 &&
                     [genre.stringValue isEqualToString:@"Keep this stale candidate"] != 0,
                   "A stale field clear gesture must preserve the frozen candidate");
      captureView(fixture.editor().window.contentView, stateRoot / "properties-stale.png");
      fixture.finish();
      session.navigate(listId);
      return listId;
    }

    void verifyStaleCloseContract(EditorFixture& fixture, std::vector<TrackId> const& tracks, ListId listId)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();
      session.select({tracks[0]});
      requireAdmission(model.beginProperties({tracks[0]}), "The stale close properties draft must begin");
      fixture.present();
      editText(fixture.editor().window, @"genre", @"Keep this transitioning draft");
      auto* const genre = requireTextField(fixture, @"genre");
      auto* const editorWindow = fixture.editor().window;
      [fixture.editor() cancel:nil];
      requireWaitUntil([&] { return editorWindow.attachedSheet != nil; },
                       "Cancelling an ordinary dirty draft must present discard confirmation");
      auto* const confirmation = editorWindow.attachedSheet;
      AO_INVARIANT([confirmation.defaultButtonCell.title isEqualToString:@"Keep Editing"] != 0,
                   "A fresh dirty draft must initially offer Keep Editing");

      auto concurrentModelPtr = std::make_unique<LibraryEditorModel>(session.runtime(), session.catalog(), [] {});
      requireAdmission(concurrentModelPtr->beginList(listId), "The stale close mutation must begin");
      concurrentModelPtr->editList("Authoring probe", "Stale close mutation", "#aobus_authoring_probe");
      concurrentModelPtr->save();
      requireWaitUntil([&] { return concurrentModelPtr->state().completed && model.state().stale; },
                       "The independent mutation must stale the draft beneath its open confirmation");
      concurrentModelPtr->cancel();
      concurrentModelPtr.reset();
      [fixture.editor() refresh];
      settleNativeCallbacks();
      AO_INVARIANT(editorWindow.attachedSheet == confirmation &&
                     [confirmation.defaultButtonCell.title isEqualToString:@"Keep Open"] != 0,
                   "An open dirty confirmation must become Keep Open when its draft turns stale");

      [editorWindow endSheet:confirmation returnCode:NSAlertFirstButtonReturn];
      requireWaitUntil(
        [&] { return editorWindow.attachedSheet == nil; }, "Keep Open must settle the stale discard confirmation");
      settleNativeCallbacks();
      AO_INVARIANT(fixture.editor().window == editorWindow && fixture.window().attachedSheet == editorWindow &&
                     model.state().dirty && model.state().stale && genre.editable == 0 && genre.selectable != 0 &&
                     [genre.stringValue isEqualToString:@"Keep this transitioning draft"] != 0,
                   "Keep Open must retain the stale read-only draft and its field candidate");

      [fixture.editor() cancel:nil];
      requireWaitUntil([&] { return editorWindow.attachedSheet != nil; },
                       "Cancelling the retained stale draft must present discard confirmation again");
      auto* const discardConfirmation = editorWindow.attachedSheet;
      AO_INVARIANT([discardConfirmation.defaultButtonCell.title isEqualToString:@"Keep Open"] != 0,
                   "A retained stale draft must continue to offer Keep Open");
      [editorWindow endSheet:discardConfirmation returnCode:NSAlertSecondButtonReturn];
      requireWaitUntil([&] { return fixture.editor().window == nil; }, "Discard must close the retained stale draft");
      fixture.detachClosed();
    }

    void finishCompletedEditor(EditorFixture& fixture)
    {
      AO_INVARIANT(fixture.model().state().completed, "Only a completed editor may use completion cleanup");
      [fixture.editor() refresh];
      fixture.finish();
    }

    void verifySavedListNavigation(EditorFixture& fixture, ListId parentId, ListId childId)
    {
      auto& session = fixture.session();
      auto* const delegate = [[AobusAuthoringBrowserDelegate alloc] init];
      auto* const browser = [[AobusLibraryBrowser alloc] initWithSession:session delegate:delegate];
      auto* const outline = browser.lists;
      browser.listScroll.frame = NSMakeRect(0, 0, 240, 600);
      [fixture.window().contentView addSubview:browser.listScroll];
      [browser refresh];
      auto rowForList = [&](ListId listId)
      {
        for (NSInteger row = 0; row < outline.numberOfRows; ++row)
        {
          if (id const item = [outline itemAtRow:row]; [item isKindOfClass:NSNumber.class] != 0 &&
                                                       [static_cast<NSNumber*>(item) unsignedIntValue] == listId.raw())
          {
            return row;
          }
        }

        return NSInteger{-1};
      };
      auto const parentRow = rowForList(parentId);
      AO_INVARIANT(parentRow >= 0, "The saved parent must appear in the native outline");
      id const retainedParent = [outline itemAtRow:parentRow];
      auto const listRevision = session.state().listRevision;
      auto const tableRevision = session.state().tableRevision;
      auto const sortBeganRes = session.sort(rt::TrackSortField::Title, false);
      AO_INVARIANT(sortBeganRes, "The List-cache regression sort must begin");
      requireWaitUntil(
        [&] { return session.state().tableRevision != tableRevision; }, "Sorting tracks must publish a table revision");
      [browser refresh];
      AO_INVARIANT(
        session.state().listRevision == listRevision && [outline itemAtRow:rowForList(parentId)] == retainedParent,
        "A table-only revision must retain the List tree and its native item identity");
      [outline expandItem:[outline itemAtRow:parentRow]];
      auto const childRow = rowForList(childId);
      AO_INVARIANT(childRow > parentRow && [outline levelForItem:[outline itemAtRow:childRow]] >
                                             [outline levelForItem:[outline itemAtRow:parentRow]],
                   "A saved child must appear nested under its parent");
      [outline selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(parentRow)]
           byExtendingSelection:NO];
      requireWaitUntil(
        [&] { return browser.activeListId == parentId; }, "Native parent selection must navigate the shared session");
      [outline selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(childRow)]
           byExtendingSelection:NO];
      requireWaitUntil(
        [&] { return browser.activeListId == childId; }, "Native child selection must navigate the shared session");
      [browser refresh];
      id const expandedParent = [outline itemAtRow:rowForList(parentId)];
      id const selectedChild = [outline itemAtRow:outline.selectedRow];
      auto renameParent = [&](std::string name)
      {
        auto& model = fixture.model();
        requireAdmission(model.beginList(parentId), "The List-cache regression must reopen its parent");
        model.editList(std::move(name), model.state().list.description, model.state().list.expression);
        auto const revision = session.state().listRevision;
        model.save();
        requireWaitUntil([&] { return model.state().completed && session.state().listRevision != revision; },
                         "A durable List upsert must advance the independent List revision");
        model.cancel();
        [browser refresh];
      };
      renameParent("Renamed expanded parent");
      AO_INVARIANT([outline isItemExpanded:expandedParent] != 0 &&
                     [outline itemAtRow:outline.selectedRow] == selectedChild && rowForList(childId) >= 0,
                   "Reloading the list projection must preserve expanded item identity and active child selection");
      [outline selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(rowForList(parentId))]
           byExtendingSelection:NO];
      requireWaitUntil([&] { return browser.activeListId == parentId; },
                       "Returning from child to parent must preserve native selection routing");
      [browser refresh];
      [outline collapseItem:expandedParent];
      renameParent("Renamed collapsed parent");
      AO_INVARIANT([outline isItemExpanded:expandedParent] == 0 && rowForList(childId) < 0,
                   "Reloading must also preserve an intentional user collapse");
      [browser detach];
      [browser.listScroll removeFromSuperview];
    }

    void verifyListAuthoring(EditorFixture& fixture,
                             std::vector<TrackId> const& tracks,
                             ListId listId,
                             std::filesystem::path const& stateRoot)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();
      requireAdmission(model.beginList(listId), "The concurrently created list must reopen for editing");
      fixture.present();
      editText(fixture.editor().window, @"Name", @"Native Authoring List");
      editText(fixture.editor().window, @"Expression", @"#invalid-list-expression");
      auto* const listSection = static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-0"));
      AO_INVARIANT(listSection != nil, "The expression field must belong to a visible disclosure section");
      [listSection performClick:nil];
      AO_INVARIANT(requireTextField(fixture, @"Expression").hiddenOrHasHiddenAncestor != 0,
                   "The expression section must be collapsed before validating its field");
      [fixture.editor() save:nil];
      requireWaitUntil([&] { return !model.state().busy && !model.state().error.empty(); },
                       "An invalid list expression must return an authoring error");
      [fixture.editor() refresh];
      AO_INVARIANT(model.state().dirty && !model.state().completed && model.state().listExpressionError,
                   "Invalid expression state must preserve the editable list draft");
      auto* const invalidExpression = requireTextField(fixture, @"Expression");
      AO_INVARIANT(invalidExpression.hiddenOrHasHiddenAncestor == 0 && invalidExpression.currentEditor != nil &&
                     fixture.editor().window.firstResponder == invalidExpression.currentEditor &&
                     [invalidExpression.accessibilityHelp isEqualToString:nativeText(model.state().error)] != 0,
                   "Expression validation must reveal, focus, and describe the specific invalid field");
      auto const expressionError = model.state().error;
      editText(fixture.editor().window, @"Name", @"Native Authoring List");
      editText(fixture.editor().window, @"Description", @"Native authoring regression");
      AO_INVARIANT(model.state().error == expressionError && model.state().listExpressionError,
                   "Unrelated list edits must preserve the expression-specific error");
      captureView(fixture.editor().window.contentView, stateRoot / "list-validation.png");
      editText(fixture.editor().window, @"Expression", @"#aobus_authoring_membership");
      AO_INVARIANT(
        model.state().error.empty() && !model.state().listExpressionError && invalidExpression.accessibilityHelp == nil,
        "Correcting the list expression must clear its field error and accessibility help");
      [fixture.editor() save:nil];
      requireWaitUntil([&] { return model.state().completed; }, "The repaired list edit must complete");
      AO_INVARIANT(model.state().savedListId == listId, "Renaming a list must preserve its identity");
      [fixture.editor() refresh];
      captureView(fixture.editor().window.contentView, stateRoot / "list-editor.png");
      finishCompletedEditor(fixture);
      session.navigate(listId);

      auto const optList = session.runtime().library().snapshot().listNode(listId);
      AO_INVARIANT(optList && optList->name == "Native Authoring List" &&
                     optList->description == "Native authoring regression" &&
                     optList->expression == "#aobus_authoring_membership",
                   "The repaired list draft must persist its name, description, and expression");

      requireAdmission(model.beginMembership({tracks[0], tracks[1]}, listId, false),
                       "Adding captured tracks to the saved list must begin");
      fixture.present();
      requireWaitUntil([&] { return model.state().completed; }, "Adding list membership must complete");
      finishCompletedEditor(fixture);
      AO_INVARIANT(
        std::ranges::contains(session.runtime().library().snapshot().selectionTags(std::vector{tracks[0], tracks[1]}),
                              "aobus_authoring_membership"),
        "Both captured tracks must gain the saved-list membership tag");

      requireAdmission(
        model.beginMembership({tracks[0]}, listId, true), "Removing one captured track from the saved list must begin");
      fixture.present();
      requireWaitUntil([&] { return model.state().completed; }, "Removing list membership must complete");
      finishCompletedEditor(fixture);
      auto membershipSnapshot = session.runtime().library().snapshot();
      AO_INVARIANT(
        !std::ranges::contains(
          membershipSnapshot.selectionTags(std::vector{tracks[0]}), "aobus_authoring_membership") &&
          std::ranges::contains(membershipSnapshot.selectionTags(std::vector{tracks[1]}), "aobus_authoring_membership"),
        "Membership removal must affect only the captured track");

      auto const parentId = listId;
      requireAdmission(model.beginList(kInvalidListId, parentId), "Creating a child list must begin under its parent");
      fixture.present();
      editText(fixture.editor().window, @"Name", @"Child probe");
      editText(fixture.editor().window, @"Expression", @"#aobus_authoring_membership");
      [fixture.editor() save:nil];
      requireWaitUntil([&] { return model.state().completed; }, "The child list save must complete");
      auto const childId = model.state().savedListId;
      AO_INVARIANT(childId != kInvalidListId && childId != listId, "The child list must receive a distinct id");
      finishCompletedEditor(fixture);
      session.navigate(childId);
      auto const optChild = session.runtime().library().snapshot().listNode(childId);
      AO_INVARIANT(optChild && optChild->parentId == listId && optChild->name == "Child probe",
                   "The saved child must retain its parent relationship");

      verifySavedListNavigation(fixture, listId, childId);
      requireAdmission(model.beginDeletion(listId), "Deleting the active parent list must begin");
      fixture.present();
      requireWaitUntil([&] { return model.state().optDeletion && !model.state().busy; },
                       "List deletion must first produce a subtree preview");
      [fixture.editor() refresh];
      auto const& preview = *model.state().optDeletion;
      AO_INVARIANT(preview.rootListId == listId && preview.deletedLists.size() == 2,
                   "The deletion preview must include the parent and its child");
      AO_INVARIANT(std::ranges::contains(preview.deletedLists, listId, &rt::DeleteListReply::listId) &&
                     std::ranges::contains(preview.deletedLists, childId, &rt::DeleteListReply::listId),
                   "The deletion preview must identify both subtree list ids");
      captureView(fixture.editor().window.contentView, stateRoot / "list-deletion.png");
      [fixture.editor() save:nil];
      requireWaitUntil([&] { return model.state().completed; }, "Subtree deletion must complete");
      finishCompletedEditor(fixture);
      session.navigate(rt::kAllTracksListId);

      auto finalSnapshot = session.runtime().library().snapshot();
      AO_INVARIANT(!finalSnapshot.listNode(listId) && !finalSnapshot.listNode(childId),
                   "Subtree deletion must remove both saved lists");
      AO_INVARIANT(finalSnapshot.containsTrack(tracks[0]) && finalSnapshot.containsTrack(tracks[1]),
                   "Deleting saved lists must retain their referenced tracks");
      AO_INVARIANT(
        std::ranges::contains(finalSnapshot.selectionTags(std::vector{tracks[1]}), "aobus_authoring_membership"),
        "Deleting the list definition must retain the surviving track tag");
      auto const activeViewId = session.runtime().workspace().snapshot().activeViewId;
      auto activeStateRes = session.runtime().views().findTrackListState(activeViewId);
      AO_INVARIANT(activeStateRes && activeStateRes->listId == rt::kAllTracksListId,
                   "The component caller must be able to return to All Tracks after deletion");
    }
  } // namespace

  std::int32_t runAuthoringScenario(std::filesystem::path const& musicRoot, std::filesystem::path const& stateRoot)
  {
    auto fixture = EditorFixture{musicRoot, stateRoot};
    verifyEditorCloseContract(fixture);
    verifyDiscardRejectsSave(fixture);
    verifyDeferredCloseAfterSave(fixture);
    auto const tracks = firstThreeTracks(fixture.session());
    verifyPendingTagClose(fixture, tracks[0]);
    verifyCallbackExecutorSubmission(fixture, tracks[0]);
    verifyPropertyAuthoring(fixture, tracks, stateRoot);
    auto const listId = verifyStaleDraft(fixture, tracks, stateRoot);
    verifyStaleCloseContract(fixture, tracks, listId);
    verifyListAuthoring(fixture, tracks, listId, stateRoot);
    return 0;
  }
} // namespace ao::appkit::test
