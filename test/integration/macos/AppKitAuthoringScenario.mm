// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/macos-appkit/AppKitText.h"
#include "app/macos-appkit/EntryCompletionAdapter.h"
#include "app/macos-appkit/LibraryBrowser.h"
#include "app/macos-appkit/LibraryEditor.h"
#include "app/macos-appkit/LibrarySession.h"
#include "app/macos-appkit/TrackInspector.h"
#include "test/integration/macos/AppKitScenarioSupport.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <variant>
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

      void retainMembershipModel(std::shared_ptr<LibraryEditorModel> modelPtr)
      {
        _membershipModels.push_back(std::move(modelPtr));
      }

      NSWindow* window() const { return _window; }

      AobusLibraryEditor* editor() const { return _editor; }

      void present(BOOL modern = YES)
      {
        AO_INVARIANT(_editor == nil, "Only one native editor may be presented by the fixture");
        AO_INVARIANT(_window.attachedSheet == nil, "The parent must be free before presenting an editor");
        AO_INVARIANT(model().state().kind != LibraryEditorKind::None,
                     "The editor model must begin a transaction before presentation");
        _editor = [[AobusLibraryEditor alloc] initWithModel:model()
                                                     parent:_window
                                                     modern:modern
                                                    artwork:nil
                                                 completion:session().runtime().completion()];
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
      // Test-owned models stay alive through SessionFixture's shutdown and final callback drain.
      std::vector<std::shared_ptr<LibraryEditorModel>> _membershipModels;
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

    void verifyRecordingDateProperty(EditorFixture& fixture, TrackId trackId)
    {
      auto& session = fixture.session();
      auto model = LibraryEditorModel{session.runtime(), session.catalog(), [] {}};

      // A full typed date through the shared Date editor parsing.
      requireAdmission(model.beginProperties({trackId}), "The recording-date property edit must begin");
      auto const dateField = std::ranges::find_if(model.state().fields,
                                                  [](LibraryEditorField const& candidate)
                                                  { return candidate.spec.field == rt::TrackField::RecordingDate; });
      AO_INVARIANT(dateField != model.state().fields.end(), "The property form must expose the recording date field");
      AO_INVARIANT(dateField->spec.editorKind == uimodel::TrackPropertiesFormEditorKind::Date,
                   "The recording date field must use the Date editor kind");
      model.editField(static_cast<std::size_t>(std::distance(model.state().fields.begin(), dateField)), "1981-05-12");
      model.save();
      requireWaitUntil([&] { return model.state().completed; }, "The recording-date submission must complete");
      {
        auto snapshot = session.runtime().library().snapshot();
        auto const optRow = snapshot.trackRow(trackId);
        AO_INVARIANT(
          optRow && optRow->recordingDate == (ao::library::RecordingDate{.year = 1981, .month = 5, .day = 12}),
          "The native property save must store the typed recording date");
      }
      model.cancel();

      // An invalid date is rejected before submission; the durable value stands.
      requireAdmission(model.beginProperties({trackId}), "The invalid recording-date edit must begin");
      auto const invalidDateField = std::ranges::find_if(
        model.state().fields,
        [](LibraryEditorField const& candidate) { return candidate.spec.field == rt::TrackField::RecordingDate; });
      AO_INVARIANT(
        invalidDateField != model.state().fields.end(), "The invalid recording-date edit requires its field");
      model.editField(
        static_cast<std::size_t>(std::distance(model.state().fields.begin(), invalidDateField)), "1981-13");
      model.save();
      AO_INVARIANT(model.state().optInvalidField == rt::TrackField::RecordingDate,
                   "The invalid recording date must mark exactly its field");
      AO_INVARIANT(!model.state().error.empty(), "The invalid recording date must publish a validation message");
      AO_INVARIANT(!model.state().completed && !model.state().busy, "An invalid recording date must not submit");
      {
        auto snapshot = session.runtime().library().snapshot();
        auto const optRow = snapshot.trackRow(trackId);
        AO_INVARIANT(
          optRow && optRow->recordingDate == (ao::library::RecordingDate{.year = 1981, .month = 5, .day = 12}),
          "A rejected recording date must leave the durable value unchanged");
      }
      model.cancel();

      // An empty Date-editor value is the explicit clear.
      requireAdmission(model.beginProperties({trackId}), "The recording-date clear must begin");
      auto const clearDateField = std::ranges::find_if(
        model.state().fields,
        [](LibraryEditorField const& candidate) { return candidate.spec.field == rt::TrackField::RecordingDate; });
      AO_INVARIANT(clearDateField != model.state().fields.end(), "The recording-date clear requires its field");
      model.editField(static_cast<std::size_t>(std::distance(model.state().fields.begin(), clearDateField)), "");
      model.save();
      requireWaitUntil([&] { return model.state().completed; }, "The recording-date clear must complete");
      {
        auto snapshot = session.runtime().library().snapshot();
        auto const optRow = snapshot.trackRow(trackId);
        AO_INVARIANT(optRow && !optRow->recordingDate.isPresent(),
                     "An empty Date-editor value must clear the stored recording date");
      }
      model.cancel();
    }

    bool isControlInSection(NSView* control, NSView* heading)
    {
      auto* view = control;

      while (view != nil)
      {
        if (view == heading.superview)
        {
          return true;
        }

        view = view.superview;
      }

      return false;
    }

    NSTextField* rowErrorLabel(NSTextField* field)
    {
      auto* const row = field.superview.superview;
      AO_INVARIANT([row isKindOfClass:NSStackView.class] != 0, "The editor field must belong to its row");
      auto* const subviews = static_cast<NSStackView*>(row).arrangedSubviews;

      for (NSUInteger index = 0; index < subviews.count; ++index)
      {
        if (auto* const subview = subviews[index]; [subview.identifier isEqualToString:@"field-error"] != NO)
        {
          AO_INVARIANT([subview isKindOfClass:NSTextField.class] != 0, "The field error must be a text label");
          return static_cast<NSTextField*>(subview);
        }
      }

      return nil;
    }

    void verifyNativeRecordingDateControl(EditorFixture& fixture, TrackId trackId)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();
      auto const typedDate = library::RecordingDate{.year = 1974, .month = 3, .day = 9};
      auto durableGenre = std::string{};
      auto requireSaveButton = [&]
      {
        auto* const save = static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"save"));

        AO_INVARIANT(save != nil && save.target == static_cast<id>(fixture.editor()) && save.action == @selector(save:),
                     "The presented editor must expose its Save action");
        return save;
      };

      // The Date control is disclosed with Work & Performance, not File Information.
      requireAdmission(model.beginProperties({trackId}), "The native recording-date edit must begin");
      fixture.present();
      auto* const performance = static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-3"));
      auto* const fileInformation =
        static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-4"));
      auto* const dateField = requireTextField(fixture, @"recording-date");
      auto* const filePath = requireTextField(fixture, @"file-path");
      AO_INVARIANT(performance != nil && fileInformation != nil,
                   "The property editor must expose its performance and file sections");
      AO_INVARIANT([performance.title
                     isEqualToString:catalogText(session.catalog(), ao::i18n::MessageId::AppKitWorkPerformance)] != 0 &&
                     performance.action == @selector(toggleSection:),
                   "Section 3 must be the Work & Performance disclosure");
      AO_INVARIANT([fileInformation.title
                     isEqualToString:catalogText(session.catalog(), ao::i18n::MessageId::AppKitFileInformation)] != 0,
                   "Section 4 must remain technical File Information");
      AO_INVARIANT(isControlInSection(dateField, performance) && !isControlInSection(dateField, fileInformation),
                   "The recording-date control must be placed in Work & Performance, not File Information");
      AO_INVARIANT(isControlInSection(filePath, fileInformation) && !isControlInSection(filePath, performance),
                   "Technical file fields must remain in File Information");
      auto const dateIndex = static_cast<std::size_t>(dateField.tag);
      AO_INVARIANT(dateIndex < model.state().fields.size() &&
                     model.state().fields[dateIndex].spec.field == rt::TrackField::RecordingDate &&
                     model.state().fields[dateIndex].spec.editorKind == uimodel::TrackPropertiesFormEditorKind::Date &&
                     dateField.delegate == static_cast<id>(fixture.editor()) && dateField.editable != 0,
                   "The recording-date control must bind the editable Date field through the editor delegate");
      AO_INVARIANT(performance.state == NSControlStateValueOff && dateField.hiddenOrHasHiddenAncestor != 0,
                   "Modern Work & Performance must start collapsed, hiding the recording-date control");
      [performance performClick:nil];
      AO_INVARIANT(performance.state == NSControlStateValueOn && dateField.hiddenOrHasHiddenAncestor == 0,
                   "The native performance disclosure must reveal the recording-date control");
      AO_INVARIANT([dateField.stringValue isEqualToString:@"1974-03-09"] == 0,
                   "The typed recording date must not already be displayed");
      auto* const save = requireSaveButton();
      AO_INVARIANT(save.enabled == 0, "Save must start disabled before the recording-date edit");
      editText(fixture.editor().window, @"recording-date", @"1974-03-09");
      AO_INVARIANT(model.state().dirty && model.state().fields.at(dateIndex).text == "1974-03-09" && save.enabled != 0,
                   "The native field editor must route the recording-date text and enable Save");
      [fixture.editor() save:save];
      requireWaitUntil([&] { return model.state().completed; }, "The native recording-date Save must complete");
      [fixture.editor() refresh];
      {
        auto snapshot = session.runtime().library().snapshot();
        auto const optRow = snapshot.trackRow(trackId);
        AO_INVARIANT(optRow && optRow->recordingDate == typedDate,
                     "The native Save action must durably store the typed recording date");
        durableGenre = optRow->genre;
      }
      fixture.finish();

      // Collapse the performance section, then require invalid Save to reveal, focus, and describe it.
      requireAdmission(model.beginProperties({trackId}), "The native invalid recording-date edit must begin");
      fixture.present();
      auto* const invalidPerformance =
        static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-3"));
      auto* const invalidFileInformation =
        static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-4"));
      AO_INVARIANT(invalidPerformance != nil && invalidFileInformation != nil,
                   "The invalid recording-date edit must expose both metadata sections");
      [invalidPerformance performClick:nil];
      editText(fixture.editor().window, @"recording-date", @"1974-13");
      editText(fixture.editor().window, @"genre", @"Rejected recording-date probe");
      [invalidPerformance performClick:nil];
      auto* const invalidDate = requireTextField(fixture, @"recording-date");
      auto const invalidIndex = static_cast<std::size_t>(invalidDate.tag);
      AO_INVARIANT(invalidPerformance.state == NSControlStateValueOff && invalidDate.hiddenOrHasHiddenAncestor != 0,
                   "The recording-date section must be collapsed before the invalid Save");
      auto* const invalidSave = requireSaveButton();
      AO_INVARIANT(invalidSave.enabled != 0, "The invalid recording-date draft must still offer Save");
      [fixture.editor() save:invalidSave];
      auto* const errorLabel = rowErrorLabel(invalidDate);
      AO_INVARIANT(!model.state().busy && model.state().dirty && !model.state().completed &&
                     model.state().optInvalidField == rt::TrackField::RecordingDate && !model.state().error.empty() &&
                     model.state().fields.at(invalidIndex).text == "1974-13",
                   "An invalid native recording date must reject Save without submitting or dropping the draft");
      AO_INVARIANT(
        invalidPerformance.state == NSControlStateValueOn && invalidFileInformation.state == NSControlStateValueOff &&
          invalidDate.hiddenOrHasHiddenAncestor == 0 && invalidDate.currentEditor != nil &&
          fixture.editor().window.firstResponder == invalidDate.currentEditor &&
          [invalidDate.accessibilityHelp isEqualToString:nativeText(model.state().error)] != 0 && errorLabel != nil &&
          errorLabel.hidden == 0 && [errorLabel.stringValue isEqualToString:nativeText(model.state().error)] != 0,
        "Invalid recording-date Save must reveal, focus, and describe the field without opening File "
        "Information");
      {
        auto snapshot = session.runtime().library().snapshot();
        auto const optRow = snapshot.trackRow(trackId);
        AO_INVARIANT(optRow && optRow->recordingDate == typedDate && optRow->genre == durableGenre,
                     "An invalid recording date must not partially replace the durable date or other fields");
      }
      fixture.finish();

      // The production clear action, not a model edit, then Save.
      requireAdmission(model.beginProperties({trackId}), "The native recording-date clear must begin");
      fixture.present();
      auto* const clearPerformance =
        static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-3"));
      AO_INVARIANT(clearPerformance != nil, "The recording-date clear must expose the performance disclosure");
      [clearPerformance performClick:nil];
      auto* const populatedDate = requireTextField(fixture, @"recording-date");
      AO_INVARIANT(
        [populatedDate.stringValue isEqualToString:@"1974-03-09"] != 0 && populatedDate.hiddenOrHasHiddenAncestor == 0,
        "The reopened recording-date control must show the durable typed value");
      auto* const line = populatedDate.superview;
      AO_INVARIANT([line isKindOfClass:NSStackView.class] != 0, "The recording-date control must own its action line");
      NSButton* actions = nil;
      auto* const subviews = static_cast<NSStackView*>(line).arrangedSubviews;

      for (NSUInteger index = 0; index < subviews.count; ++index)
      {
        if (auto* const subview = subviews[index]; [subview.identifier isEqualToString:@"field-actions"] != NO)
        {
          actions = static_cast<NSButton*>(subview);
        }
      }

      AO_INVARIANT(actions != nil && actions.enabled != 0 && actions.tag == populatedDate.tag &&
                     actions.action == @selector(fieldActions:),
                   "The recording-date row must expose its field-actions affordance");
      auto* const clearDate = [[NSMenuItem alloc] init];
      clearDate.tag = populatedDate.tag;
      [fixture.editor() clearField:clearDate];
      AO_INVARIANT(model.state().dirty &&
                     model.state().fields.at(static_cast<std::size_t>(populatedDate.tag)).text.empty() &&
                     populatedDate.stringValue.length == 0 &&
                     [populatedDate.placeholderString
                       isEqualToString:catalogText(session.catalog(), ao::i18n::MessageId::AppKitWillClear)] != 0,
                   "The native clear action must route an explicit empty recording date");
      auto* const clearSave = requireSaveButton();
      AO_INVARIANT(clearSave.enabled != 0, "The native recording-date clear must enable Save");
      [fixture.editor() save:clearSave];
      requireWaitUntil([&] { return model.state().completed; }, "The native recording-date clear must complete");
      [fixture.editor() refresh];
      {
        auto snapshot = session.runtime().library().snapshot();
        auto const optRow = snapshot.trackRow(trackId);
        AO_INVARIANT(optRow && !optRow->recordingDate.isPresent() && optRow->genre == durableGenre,
                     "The native clear Save must durably remove the recording date without changing other fields");
      }
      fixture.finish();
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

    void verifyNativeCreditPreviews(EditorFixture& fixture, TrackId trackId)
    {
      auto& model = fixture.model();
      auto& library = fixture.session().runtime().library();
      auto const storedCredits = [&]
      {
        auto const optCredits = library.snapshot().trackCredits(trackId);
        AO_INVARIANT(optCredits, "The preview fixture track must remain available");
        return *optCredits;
      };
      auto const before = library.snapshot().revision();
      auto const original = storedCredits();
      auto const button = [&](NSString* identifier)
      {
        auto* const control = findControl(fixture.editor().window.contentView, identifier);
        AO_INVARIANT([control isKindOfClass:NSButton.class], "The preview must expose its native action");
        return static_cast<NSButton*>(control);
      };
      auto const click = [&](NSString* identifier)
      {
        auto* const control = button(identifier);
        AO_INVARIANT(control.enabled, "The preview scenario requires an enabled action");
        [control performClick:nil];
      };
      auto const requireUnwritten = [&]
      {
        AO_INVARIANT(library.snapshot().revision() == before && storedCredits() == original,
                     "Opening, cancelling and accepting a preview must not write before parent Save");
      };
      auto const replacements = std::array<library::Credit, 3>{
        library::Credit{.name = "Preview conductor", .kind = library::CreditKind::Conductor, .role = "direction"},
        library::Credit{.name = "Preview ensemble", .kind = library::CreditKind::Ensemble, .role = "strings"},
        library::Credit{.name = "Preview soloist", .kind = library::CreditKind::Soloist, .role = "violin"}};

      requireAdmission(model.beginProperties({trackId}), "Category previews must begin Properties");
      fixture.present();
      click(@"section-3");
      auto* const previews =
        @[button(@"credit-preview-edit-0"), button(@"credit-preview-edit-1"), button(@"credit-preview-edit-2")];
      id const entryTarget = static_cast<NSButton*>(previews[0]).target;
      SEL const entryAction = static_cast<NSButton*>(previews[0]).action;

      for (std::size_t index = 0; index < replacements.size(); ++index)
      {
        auto* const preview = static_cast<NSButton*>(previews[index]);
        auto* const oldSummary = button(@"credits-edit-all");
        id const summaryTarget = oldSummary.target;
        SEL const summaryAction = oldSummary.action;
        AO_INVARIANT(preview.enabled && !preview.hiddenOrHasHiddenAncestor,
                     "Every category preview must remain enabled after earlier summary refreshes");
        [preview performClick:nil];
        AO_INVARIANT(model.creditsEditor().isEditing() &&
                       model.creditsEditor().scope() == uimodel::trackCreditScope(replacements[index].kind),
                     "Each native preview must open exactly its fixed category scope");
        requireUnwritten();
        click(@"credits-clear");
        click(@"credits-add");
        AO_INVARIANT(findControl(fixture.editor().window.contentView, @"credit-kind-0").hiddenOrHasHiddenAncestor,
                     "Preview editing must not expose an editable kind selector");
        editText(fixture.editor().window, @"credit-name-0", nativeText(replacements[index].name));
        editText(fixture.editor().window, @"credit-role-0", nativeText(replacements[index].role));
        click(@"credits-save");
        AO_INVARIANT(model.state().dirty && !model.creditsEditor().isEditing() &&
                       model.creditSections()[index].optValue == std::vector<library::Credit>{replacements[index]},
                     "A preview child Save must stage its full typed segment in the parent");
        requireUnwritten();
        AO_INVARIANT(oldSummary.target == nil && oldSummary.action == nullptr && !oldSummary.enabled &&
                       preview.target == entryTarget && preview.action == entryAction && preview.enabled,
                     "Summary regeneration must revoke summary controls without revoking field previews");
        auto* const responder = fixture.editor().window.firstResponder;
        [oldSummary sendAction:summaryAction to:summaryTarget];
        auto* const forgedPreview = [NSButton buttonWithTitle:preview.title target:entryTarget action:entryAction];
        forgedPreview.tag = preview.tag;
        [forgedPreview sendAction:entryAction to:entryTarget];
        AO_INVARIANT(!model.creditsEditor().isEditing() && fixture.editor().window.firstResponder == responder,
                     "Retired or copied entry senders must be rejected before focus callbacks");
        [preview performClick:nil];
        AO_INVARIANT(model.creditsEditor().isEditing() &&
                       model.creditsEditor().scope() == uimodel::trackCreditScope(replacements[index].kind) &&
                       model.creditsEditor().entries() == std::vector<library::Credit>{replacements[index]},
                     "The same preview must reopen its staged segment after summary replacement");
        click(@"credits-cancel");
        requireUnwritten();
      }

      click(@"save");
      requireWaitUntil([&] { return model.state().completed; }, "Composed preview Save must complete");
      auto expected = std::vector<library::Credit>{replacements.begin(), replacements.end()};

      for (auto const& credit : original)
      {
        if (credit.kind == library::CreditKind::Performer)
        {
          expected.push_back(credit);
        }
      }

      AO_INVARIANT(storedCredits() == expected && library.snapshot().revision() == before + 1,
                   "Parent Save must compose all three preview segments and preserve unselected performers");
      fixture.finish();
      requireAdmission(model.beginProperties({trackId}), "Preview teardown must permit a successor Properties draft");
      fixture.present();
      auto* const responder = fixture.editor().window.firstResponder;

      for (NSUInteger index = 0; index < previews.count; ++index)
      {
        auto* const preview = static_cast<NSButton*>(previews[index]);
        AO_INVARIANT(preview.target == nil && preview.action == nullptr && !preview.enabled,
                     "Parent teardown must revoke every retained preview control");
        [preview sendAction:entryAction to:entryTarget];
        AO_INVARIANT(!model.creditsEditor().isEditing() && !model.state().dirty && storedCredits() == expected &&
                       library.snapshot().revision() == before + 1 &&
                       fixture.editor().window.firstResponder == responder,
                     "Retained preview callbacks must not reopen or disturb a successor draft");
      }

      fixture.finish();
    }

    void verifyNativeCreditsControls(EditorFixture& fixture,
                                     std::vector<TrackId> const& tracks,
                                     std::filesystem::path const& stateRoot)
    {
      auto& model = fixture.model();
      auto& library = fixture.session().runtime().library();
      auto const revision = [&] { return library.snapshot().revision(); };
      auto const storedCredits = [&](TrackId id)
      {
        auto optCredits = library.snapshot().trackCredits(id);
        AO_INVARIANT(optCredits, "Credits fixture must retain its track");
        return *optCredits;
      };
      auto const button = [&](NSString* identifier)
      {
        auto* const control = findControl(fixture.editor().window.contentView, identifier);
        AO_INVARIANT([control isKindOfClass:NSButton.class], "Credits must expose its semantic native button");
        return static_cast<NSButton*>(control);
      };
      auto const click = [&](NSString* identifier)
      {
        auto* const control = button(identifier);
        AO_INVARIANT(control.enabled, "Credits scenario requires an enabled native action");
        [control performClick:nil];
      };
      auto const pressKey = [&](NSString* identifier, NSString* characters, std::uint16_t keyCode)
      {
        auto* const event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:NSProcessInfo.processInfo.systemUptime
                                         windowNumber:fixture.editor().window.windowNumber
                                              context:nil
                                           characters:characters
                          charactersIgnoringModifiers:characters
                                            isARepeat:NO
                                              keyCode:keyCode];
        auto const handled = [button(identifier) performKeyEquivalent:event];
        AO_INVARIANT(handled, "Credits must handle its native key equivalent");
      };
      auto const chooseKind = [&](NSString* identifier, library::CreditKind kind)
      {
        auto* const control = static_cast<NSPopUpButton*>(findControl(fixture.editor().window.contentView, identifier));
        AO_INVARIANT(
          [control isKindOfClass:NSPopUpButton.class] && control.enabled && !control.hiddenOrHasHiddenAncestor,
          "All-kind Credits must expose an editable kind popup");
        [control selectItemAtIndex:static_cast<NSInteger>(kind)];
        [control sendAction:control.action to:control.target];
      };

      auto const retainCreditsCallbacks = [&]
      {
        auto* const controls = @[
          button(@"credit-delete-0"),
          button(@"credit-up-1"),
          button(@"credit-down-0"),
          findControl(fixture.editor().window.contentView, @"credit-kind-0"),
          button(@"credits-add"),
          button(@"credits-clear"),
          button(@"credits-replace"),
          button(@"credits-save"),
          button(@"credits-cancel")
        ];
        auto actions = std::vector<SEL>{};
        id const target = static_cast<NSControl*>(controls[0]).target;

        for (NSUInteger index = 0; index < controls.count; ++index)
        {
          auto* const control = static_cast<NSControl*>(controls[index]);
          AO_INVARIANT(control.target == target && control.action != nullptr,
                       "The retained controls must come from the actual current Credits child");
          actions.push_back(control.action);
        }

        auto* const field = requireTextField(fixture, @"credit-name-0");
        id<NSTextFieldDelegate> const delegate = field.delegate;
        auto* const textView = static_cast<NSTextView*>([fixture.editor().window fieldEditor:YES forObject:field]);
        AO_INVARIANT(textView != nil, "The Credits callback probe requires the native field editor");
        return [&, controls, actions, target, field, delegate, textView]
        {
          auto const candidate = model.creditsEditor().entries();
          auto const scope = model.creditsEditor().scope();
          auto const editing = model.creditsEditor().isEditing();
          auto const optFocusedRow = model.creditsEditor().focusedRow();
          auto const durable = storedCredits(tracks[0]);
          auto const beforeCallbacks = revision();
          auto* const responder = fixture.editor().window.firstResponder;
          auto const requireUnchanged = [&]
          {
            AO_INVARIANT(
              model.creditsEditor().entries() == candidate && model.creditsEditor().scope() == scope &&
                model.creditsEditor().isEditing() == editing && model.creditsEditor().focusedRow() == optFocusedRow &&
                storedCredits(tracks[0]) == durable && revision() == beforeCallbacks &&
                fixture.editor().window.firstResponder == responder,
              "Retired Credits callbacks must preserve complete draft order/kinds/roles, scope, native focus and "
              "database");
          };

          for (NSUInteger index = 0; index < controls.count; ++index)
          {
            NSControl* const control = controls[index];
            AO_INVARIANT(control.target == nil && control.action == nullptr && !control.enabled,
                         "Retiring a Credits control must revoke its target and action");

            if ([control isKindOfClass:NSPopUpButton.class])
            {
              [static_cast<NSPopUpButton*>(control)
                selectItemAtIndex:static_cast<NSInteger>(library::CreditKind::Ensemble)];
            }

            // Deliver the captured target/action, as an event already queued before retirement would.
            [control sendAction:actions[index] to:target];
            requireUnchanged();
          }

          AO_INVARIANT(field.delegate == nil, "Retired credit fields must detach their delegate");
          field.stringValue = @"Obsolete name callback";
          [delegate
            controlTextDidBeginEditing:[NSNotification notificationWithName:NSControlTextDidBeginEditingNotification
                                                                     object:field]];
          requireUnchanged();
          [delegate controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification
                                                                       object:field]];
          requireUnchanged();
          auto const consumed = [delegate control:field textView:textView doCommandBySelector:@selector(moveDown:)];
          AO_INVARIANT(!consumed, "A retired field must not route commands to a successor completion adapter");
          [delegate controlTextDidEndEditing:[NSNotification notificationWithName:NSControlTextDidEndEditingNotification
                                                                           object:field]];
          requireUnchanged();
        };
      };

      // The actual inspector action opens Properties; its child edits the same draft.
      auto* const fixtureBorrow = &fixture;
      auto const trackId = tracks[0];
      auto* const inspector = [[AobusTrackInspector alloc] initWithCatalog:fixture.session().catalog()
        revealHandler:^{}
        propertiesHandler:^{
          requireAdmission(fixtureBorrow->model().beginProperties({trackId}), "Inspector Properties must begin");
          fixtureBorrow->present();
        }
        dismissHandler:^{}];
      auto const optInspectorRow = library.snapshot().trackRow(trackId);
      [inspector renderSelectionCount:1 row:optInspectorRow credits:{} canReveal:NO];
      auto* const properties = static_cast<NSButton*>(findControl(inspector.view, @"inspector-properties"));
      AO_INVARIANT(properties != nil && properties.enabled, "Inspector must expose its working Properties route");
      [properties performClick:nil];
      [inspector detach];
      auto const before = revision();
      click(@"credits-edit-all");
      AO_INVARIANT(model.creditsEditor().isEditing() && model.creditsEditor().entries().empty(),
                   "An empty Credits list must open the same list editor without a scalar fallback");
      click(@"credits-add");
      auto* const invalidName = requireTextField(fixture, @"credit-name-0");
      auto* const error = requireTextField(fixture, @"credit-error-0");
      AO_INVARIANT(!button(@"credits-save").enabled && !error.hidden && error.stringValue.length > 0 &&
                     invalidName.currentEditor != nil && invalidName.accessibilityHelp.length > 0,
                   "A blank native credit must expose its error, retain focus, and prevent child Save");
      editText(fixture.editor().window, @"credit-name-0", @"Aa");
      editText(fixture.editor().window, @"credit-role-0", @"pano");
      auto* const typingRole = requireTextField(fixture, @"credit-role-0");
      auto const typedEntries = std::array<library::Credit, 2>{
        library::Credit{.name = "Ada", .role = "pano"}, library::Credit{.name = "Ada", .role = "piano"}};
      auto const interruptedEntries = std::array<library::Credit, 2>{
        library::Credit{.name = "Ad!a", .role = "pano"}, library::Credit{.name = "Ada", .role = "pi!ano"}};

      // Programmatic native text insertion, not physical keyboard or accessibility evidence.
      // Select each field once; consecutive interior edits must not replace it or move the caret.
      for (std::size_t column = 0; column < typedEntries.size(); ++column)
      {
        auto* const field = column == 0 ? invalidName : typingRole;
        id const delegate = field.delegate;
        [field selectText:nil];
        auto* const textEditor = static_cast<NSTextView*>(field.currentEditor);
        AO_INVARIANT(textEditor != nil, "Consecutive credit edits require the actual native field editor");
        auto const requireTyping = [&](library::Credit const& expectedEntry, NSUInteger caret)
        {
          auto const requireStable = [&]
          {
            AO_INVARIANT(requireTextField(fixture, @"credit-name-0") == invalidName &&
                           requireTextField(fixture, @"credit-role-0") == typingRole && field.delegate == delegate &&
                           field.currentEditor == textEditor && fixture.editor().window.firstResponder == textEditor &&
                           NSEqualRanges(textEditor.selectedRange, NSMakeRange(caret, 0)) != NO &&
                           model.creditsEditor().focusedRow() == std::size_t{0},
                         "Consecutive Name/Role edits must retain field identity, delegate, native focus and caret");
            AO_INVARIANT(model.creditsEditor().entries() == std::vector<library::Credit>{expectedEntry} &&
                           model.creditsEditor().isEditing() && model.creditsEditor().validationErrors().empty() &&
                           error.hidden != NO && button(@"credits-save").enabled != NO &&
                           button(@"save").enabled == NO && revision() == before && storedCredits(trackId).empty(),
                         "Typing must retain every unedited credit attribute, clear child validation and keep parent "
                         "Save guarded without writing");
          };
          requireStable();
          settleNativeCallbacks();
          requireStable();
        };
        textEditor.selectedRange = NSMakeRange(1, 0);
        [textEditor insertText:column == 0 ? @"d" : @"i" replacementRange:textEditor.selectedRange];
        requireTyping(typedEntries[column], 2);
        [textEditor insertText:@"!" replacementRange:textEditor.selectedRange];
        requireTyping(interruptedEntries[column], 3);
        [textEditor insertText:@"" replacementRange:NSMakeRange(2, 1)];
        requireTyping(typedEntries[column], 2);
      }

      [fixture.editor() save:nil];
      AO_INVARIANT(model.creditsEditor().isEditing() && !model.state().busy && revision() == before &&
                     model.creditsEditor().entries() == (std::vector<library::Credit>{typedEntries[1]}),
                   "Programmatic parent Save must not submit or discard the consecutive typing candidate");
      click(@"credits-add");
      editText(fixture.editor().window, @"credit-name-1", @"Ada");
      editText(fixture.editor().window, @"credit-role-1", @"piano");
      click(@"credits-add");
      editText(fixture.editor().window, @"credit-name-2", @"Leader");
      editText(fixture.editor().window, @"credit-role-2", @"director");
      click(@"credit-up-2");
      AO_INVARIANT(
        model.creditsEditor().entries()[1].name == "Leader" && model.creditsEditor().entries()[1].role == "director",
        "Native within-kind reorder must preserve the moved row's role");
      chooseKind(@"credit-kind-1", library::CreditKind::Conductor);
      AO_INVARIANT(model.creditsEditor().entries()[0] ==
                     (library::Credit{.name = "Leader", .kind = library::CreditKind::Conductor, .role = "director"}),
                   "Native reclassification must preserve name and role and regroup the row");
      addSharedTag(fixture, @"native_credits_probe");
      AO_INVARIANT(!button(@"save").enabled, "A tags-only dirty parent must not bypass an active Credits child");
      [fixture.editor() save:nil];
      AO_INVARIANT(!model.state().busy && revision() == before, "An active child must block programmatic parent Save");
      [fixture.editor().window setContentSize:NSMakeSize(580, 500)];
      [fixture.editor() refresh];
      auto* const name = requireTextField(fixture, @"credit-name-0");
      [name scrollRectToVisible:name.bounds];
      [fixture.editor().window.contentView layoutSubtreeIfNeeded];
      AO_INVARIANT(name.frame.size.width > 100 && name.frame.size.width < 580 &&
                     button(@"credits-save").keyEquivalent.length == 1 &&
                     button(@"credits-cancel").keyEquivalent.length == 1,
                   "Constrained Credits must retain usable field width and native Save/Cancel key equivalents");
      captureView(fixture.editor().window.contentView, stateRoot / "properties-credits-narrow.png");
      [fixture.editor() requestCloseWithCompletion:nil];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "Closing a Credits draft must present the native discard decision");
      auto* const childSave = button(@"credits-save");
      [childSave sendAction:childSave.action to:childSave.target];
      AO_INVARIANT(model.creditsEditor().isEditing() && revision() == before,
                   "A programmatic child Save beneath discard confirmation must not accept or submit the draft");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertFirstButtonReturn];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet == nil; },
                       "Keep Editing must return to the existing Credits child");
      pressKey(@"credits-save", @"\r", 36);
      AO_INVARIANT(!model.creditsEditor().isEditing() && model.state().dirty && revision() == before,
                   "Accepting Credits must stage the parent draft without a database write");
      editText(fixture.editor().window, @"genre", @"Credits composed save");
      click(@"save");
      requireWaitUntil([&] { return model.state().completed; }, "Composed native Credits Save must complete");
      auto const expected =
        std::vector<library::Credit>{{.name = "Leader", .kind = library::CreditKind::Conductor, .role = "director"},
                                     {.name = "Ada", .role = "piano"},
                                     {.name = "Ada", .role = "piano"}};
      AO_INVARIANT(
        storedCredits(trackId) == expected && revision() == before + 1 &&
          library.snapshot().trackRow(trackId)->genre == "Credits composed save" &&
          std::ranges::contains(library.snapshot().selectionTags(std::array{trackId}), "native_credits_probe"),
        "One composed commit must retain duplicate credits and save ordinary metadata and tags atomically");
      fixture.finish();

      // Retained native senders must not acquire the meaning of a reused row tag.
      requireAdmission(model.beginProperties({trackId}), "Retired Credits controls must begin");
      fixture.present();
      auto* const retiredEntry = button(@"credits-edit-all");
      id const retiredEntryTarget = retiredEntry.target;
      auto* const retiredEntryAction = retiredEntry.action;
      click(@"credits-edit-all");
      click(@"credits-clear");

      for (auto const index : {0, 1, 2})
      {
        click(@"credits-add");
        auto* const nameIdentifier = [NSString stringWithFormat:@"credit-name-%d", index];
        auto* const roleIdentifier = [NSString stringWithFormat:@"credit-role-%d", index];
        editText(fixture.editor().window, nameIdentifier, [NSString stringWithFormat:@"Retained row %d", index]);
        editText(fixture.editor().window, roleIdentifier, [NSString stringWithFormat:@"Retained role %d", index]);
      }

      auto const afterAdd = retainCreditsCallbacks();
      click(@"credits-add");
      afterAdd();
      auto* const addedName = requireTextField(fixture, @"credit-name-3");
      AO_INVARIANT(model.creditsEditor().focusedRow() == std::size_t{3} && addedName.currentEditor != nil &&
                     fixture.editor().window.firstResponder == addedName.currentEditor,
                   "Native Add must retire the old row controls and focus the new blank row");
      editText(fixture.editor().window, @"credit-name-3", @"Retained row 3");
      editText(fixture.editor().window, @"credit-role-3", @"Retained role 3");
      auto const afterMove = retainCreditsCallbacks();
      click(@"credit-up-3");
      afterMove();
      auto* const movedName = requireTextField(fixture, @"credit-name-2");
      AO_INVARIANT(model.creditsEditor().entries() ==
                       (std::vector<library::Credit>{{.name = "Retained row 0", .role = "Retained role 0"},
                                                     {.name = "Retained row 1", .role = "Retained role 1"},
                                                     {.name = "Retained row 3", .role = "Retained role 3"},
                                                     {.name = "Retained row 2", .role = "Retained role 2"}}) &&
                     model.creditsEditor().focusedRow() == std::size_t{2} && movedName.currentEditor != nil &&
                     fixture.editor().window.firstResponder == movedName.currentEditor,
                   "Native Move must preserve the complete row attributes and focus the moved row");
      auto const afterReclassification = retainCreditsCallbacks();
      chooseKind(@"credit-kind-2", library::CreditKind::Conductor);
      afterReclassification();
      auto const afterDestinationReclassification = retainCreditsCallbacks();
      chooseKind(@"credit-kind-2", library::CreditKind::Conductor);
      afterDestinationReclassification();
      auto* const reclassifiedName = requireTextField(fixture, @"credit-name-1");
      AO_INVARIANT(model.creditsEditor().entries() ==
                       (std::vector<library::Credit>{
                         {.name = "Retained row 3", .kind = library::CreditKind::Conductor, .role = "Retained role 3"},
                         {.name = "Retained row 1", .kind = library::CreditKind::Conductor, .role = "Retained role 1"},
                         {.name = "Retained row 0", .role = "Retained role 0"},
                         {.name = "Retained row 2", .role = "Retained role 2"}}) &&
                     model.creditsEditor().focusedRow() == std::size_t{1} && reclassifiedName.currentEditor != nil &&
                     fixture.editor().window.firstResponder == reclassifiedName.currentEditor,
                   "Native Kind change must append to the destination segment with name/role and focus intact");
      auto const afterDeletion = retainCreditsCallbacks();
      click(@"credit-delete-0");
      afterDeletion();
      auto const afterCancel = retainCreditsCallbacks();
      click(@"credits-cancel");
      afterCancel();
      AO_INVARIANT(retiredEntry.target == nil && retiredEntry.action == nullptr && !retiredEntry.enabled,
                   "Rebuilding the Credits summary must revoke its retired entry buttons");
      [retiredEntry sendAction:retiredEntryAction to:retiredEntryTarget];
      AO_INVARIANT(!model.creditsEditor().isEditing() && !model.state().dirty && storedCredits(trackId) == expected &&
                     revision() == before + 1,
                   "A queued retired summary action must not reopen an obsolete Credits scope");
      click(@"credits-edit-0");
      afterAdd();
      afterMove();
      afterReclassification();
      afterDestinationReclassification();
      afterDeletion();
      afterCancel();
      AO_INVARIANT(model.creditsEditor().entries() == (std::vector<library::Credit>{expected[0]}) &&
                     storedCredits(trackId) == expected && revision() == before + 1,
                   "Retired controls must leave the reopened scope and complete durable Credits unchanged");
      click(@"credits-cancel");
      fixture.finish();

      requireAdmission(model.beginProperties({trackId}), "Scoped native Credits must reopen");
      fixture.present();
      click(@"credits-edit-0");
      AO_INVARIANT(model.creditsEditor().scope() == uimodel::trackCreditScope(library::CreditKind::Conductor) &&
                     model.creditsEditor().entries().size() == 1 &&
                     findControl(fixture.editor().window.contentView, @"credit-kind-0").hiddenOrHasHiddenAncestor,
                   "A category editor must lock kind and load its complete scoped list");
      editText(fixture.editor().window, @"credit-name-0", @"Revised leader");
      click(@"credits-save");
      click(@"credits-edit-3");
      click(@"credit-delete-0");
      pressKey(@"credits-cancel", @"\033", 53);
      click(@"credits-edit-all");
      AO_INVARIANT(
        model.creditsEditor().entries().size() == 3 && model.creditsEditor().entries()[0].name == "Revised leader" &&
          model.creditsEditor().entries()[0].role == "director" && model.creditsEditor().entries()[1] == expected[1] &&
          model.creditsEditor().entries()[2] == expected[2],
        "Reopening must overlay accepted scopes and preserve cancelled scopes with roles and duplicates");
      click(@"credits-cancel");
      click(@"save");
      requireWaitUntil([&] { return model.state().completed; }, "Scoped native name edit must complete");
      auto scopedExpected = expected;
      scopedExpected[0].name = "Revised leader";
      AO_INVARIANT(storedCredits(trackId) == scopedExpected,
                   "A category name edit must preserve role and every unselected segment");
      fixture.finish();

      requireAdmission(model.beginProperties({trackId, tracks[1]}), "Mixed Credits must begin");
      fixture.present();
      auto const mixedBefore = revision();
      click(@"credits-edit-0");
      AO_INVARIANT(model.creditsEditor().isMixedReplacement() && model.creditsEditor().entries().empty() &&
                     !button(@"credits-add").enabled && !button(@"credits-save").enabled,
                   "Mixed Credits must neither seed a first-target/union draft nor permit implicit replacement");
      click(@"credits-replace");
      AO_INVARIANT(button(@"credits-add").enabled && !button(@"credits-save").enabled,
                   "An untouched empty mixed replacement must not become an implicit clear");
      click(@"credits-clear");
      click(@"credits-save");
      AO_INVARIANT(revision() == mixedBefore, "Explicit mixed clear must remain a staged parent edit");
      click(@"save");
      requireWaitUntil([&] { return model.state().completed; }, "Mixed scoped clear must complete");
      AO_INVARIANT(storedCredits(trackId) == (std::vector<library::Credit>{expected[1], expected[2]}) &&
                     storedCredits(tracks[1]).empty(),
                   "Explicit mixed clear must preserve each target's unselected credits independently");
      fixture.finish();

      requireAdmission(model.beginProperties({trackId}), "Stale child Credits must begin");
      fixture.present();
      click(@"credits-edit-3");
      editText(fixture.editor().window, @"credit-role-0", @"Retained stale role");
      auto concurrent = LibraryEditorModel{fixture.session().runtime(), fixture.session().catalog(), [] {}};
      requireAdmission(concurrent.beginProperties({tracks[1]}), "Independent mutation must begin");
      auto const field = std::ranges::find(
        concurrent.state().fields, rt::TrackField::Genre, [](auto const& candidate) { return candidate.spec.field; });
      AO_INVARIANT(field != concurrent.state().fields.end(), "Independent mutation requires Genre");
      concurrent.editField(static_cast<std::size_t>(field - concurrent.state().fields.begin()), "Stale Credits probe");
      concurrent.save();
      requireWaitUntil([&] { return concurrent.state().completed && model.state().stale; },
                       "Independent revision must stale the active Credits draft");
      concurrent.cancel();
      [fixture.editor() refresh];
      AO_INVARIANT(model.creditsEditor().entries()[0].role == "Retained stale role" &&
                     !requireTextField(fixture, @"credit-role-0").editable && !button(@"credits-save").enabled,
                   "Stale native Credits must freeze and retain the candidate without rebinding");
      [fixture.editor() requestCloseWithCompletion:nil];
      requireWaitUntil([&] { return fixture.editor().window.attachedSheet != nil; },
                       "An active Credits child alone must require a discard decision");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertFirstButtonReturn];
      settleNativeCallbacks();
      AO_INVARIANT(
        model.creditsEditor().isEditing() && model.creditsEditor().entries()[0].role == "Retained stale role",
        "Keep Open must preserve the stale Credits child");
      [fixture.editor() requestCloseWithCompletion:nil];
      requireWaitUntil(
        [&] { return fixture.editor().window.attachedSheet != nil; }, "Discard must reopen confirmation");
      [fixture.editor().window endSheet:fixture.editor().window.attachedSheet returnCode:NSAlertSecondButtonReturn];
      requireWaitUntil(
        [&] { return fixture.editor().window == nil; }, "Confirmed discard must detach the child safely");
      fixture.detachClosed();
      AO_INVARIANT(storedCredits(trackId) == (std::vector<library::Credit>{expected[1], expected[2]}),
                   "Discarding a stale child must not write its candidate");
    }

    void verifyPropertyAuthoring(EditorFixture& fixture,
                                 std::vector<TrackId> const& tracks,
                                 std::filesystem::path const& stateRoot)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();
      auto const [optFirstRow, optThirdRow] = [&]
      {
        auto snapshot = session.runtime().library().snapshot();
        return std::pair{snapshot.trackRow(tracks[0]), snapshot.trackRow(tracks[2])};
      }();
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
      auto* const expandedDate = requireTextField(fixture, @"recording-date");
      AO_INVARIANT(expandedDate.hiddenOrHasHiddenAncestor == 0 && isControlInSection(expandedDate, disclosure),
                   "Modern disclosure must reveal RecordingDate in Work & Performance");
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
      auto* const classicPerformance =
        static_cast<NSButton*>(findControl(fixture.editor().window.contentView, @"section-3"));
      auto* const classicDate = requireTextField(fixture, @"recording-date");
      AO_INVARIANT(classicPerformance != nil && classicPerformance.state == NSControlStateValueOn &&
                     classicDate.hiddenOrHasHiddenAncestor == 0 && classicDate.editable != 0 &&
                     classicDate.controlSize == NSControlSizeRegular &&
                     isControlInSection(classicDate, classicPerformance),
                   "Classic must expose the editable RecordingDate control in its expanded Work & Performance section");
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

    NSView* findIdentifiedCompletionView(NSView* root)
    {
      if ([root.identifier isEqualToString:@"completion-list"] != NO ||
          [root.accessibilityIdentifier isEqualToString:@"completion-list"] != NO)
      {
        return root;
      }

      auto* const children = root.subviews;

      for (NSUInteger index = 0; index < children.count; ++index)
      {
        auto* const child = children[index];

        if (auto* const found = findIdentifiedCompletionView(child); found != nil)
        {
          return found;
        }
      }

      return nil;
    }

    // The completion popover is its own native window outside every editor
    // panel tree, so discovery walks the application's shown windows.
    NSTableView* findShownCompletionTable()
    {
      auto* const windows = NSApp.windows;

      for (NSUInteger index = 0; index < windows.count; ++index)
      {
        auto* const window = windows[index];

        if (window.visible == NO)
        {
          continue;
        }

        if (auto* const view = findIdentifiedCompletionView(window.contentView); view != nil)
        {
          AO_INVARIANT([view isKindOfClass:NSTableView.class] != 0, "The completion list must be a table view");
          return static_cast<NSTableView*>(view);
        }
      }

      return nil;
    }

    void verifyCompletionRanges(EditorFixture& fixture)
    {
      auto* const field = [NSTextField textFieldWithString:@"😀$ti tail"];
      field.frame = NSMakeRect(16, 16, 320, 28);
      [fixture.window().contentView addSubview:field];
      AO_INVARIANT([fixture.window() makeFirstResponder:field] != NO, "The range probe must own native editing");
      auto* const editor = static_cast<NSTextView*>(field.currentEditor);
      AO_INVARIANT(editor != nil);
      editor.selectedRange = NSMakeRange(5, 0);
      std::size_t providerCalls = 0;
      std::size_t observedCursor = 0;
      bool invalidSpan = false;
      auto completion = EntryCompletionAdapter{
        field,
        fixture.session().catalog(),
        [&](std::string_view text, std::size_t cursor) -> std::optional<rt::CompletionResult>
        {
          ++providerCalls;
          observedCursor = cursor;
          AO_INVARIANT(text == "😀$ti tail", "The provider must receive exact UTF-8 source bytes");
          return rt::CompletionResult{
            .replaceBegin = invalidSpan ? 1U : 4U,
            .replaceEnd = 7,
            .items = {{.displayText = "Title field", .insertText = "$title"}},
          };
        }};
      completion.update();
      AO_INVARIANT(providerCalls == 1 && observedCursor == 7 && findShownCompletionTable() != nil,
                   "A non-BMP prefix must map the native caret to its exact UTF-8 byte offset");
      AO_INVARIANT(completion.tryHandleCommand(@selector(insertTab:)));
      AO_INVARIANT(
        [editor.string isEqual:@"😀$title tail"] != NO && NSEqualRanges(editor.selectedRange, NSMakeRange(8, 0)),
        "Acceptance must preserve both sides of the advertised byte span and use UTF-16 caret units");
      AO_INVARIANT([field.stringValue isEqual:editor.string] != NO,
                   "Native acceptance must publish insertText rather than the candidate display label");
      [editor insertText:@"😀$ti tail" replacementRange:NSMakeRange(0, editor.string.length)];
      editor.selectedRange = NSMakeRange(5, 0);
      invalidSpan = true;
      completion.update();
      AO_INVARIANT(providerCalls == 2 && findShownCompletionTable() == nil,
                   "A provider span inside a UTF-8 scalar must not become a native replacement");
      invalidSpan = false;
      completion.update();
      AO_INVARIANT(findShownCompletionTable() != nil);
      // A pointer click moves the caret without a command or text change.
      editor.selectedRange = NSMakeRange(2, 0);
      AO_INVARIANT(findShownCompletionTable() == nil && !completion.tryHandleCommand(@selector(insertTab:)),
                   "A caret moved away from the source must dismiss and leave Tab to the field");
      editor.selectedRange = NSMakeRange(5, 0);
      completion.update();
      AO_INVARIANT(providerCalls == 4 && findShownCompletionTable() != nil);
      // Text the host has not forwarded yet leaves the popover to the next update.
      [editor insertText:@"x" replacementRange:NSMakeRange(editor.string.length, 0)];
      AO_INVARIANT(findShownCompletionTable() != nil, "A text edit must not dismiss through the caret observer");
      AO_INVARIANT(!completion.tryHandleCommand(@selector(insertTab:)) && findShownCompletionTable() == nil &&
                     [editor.string isEqual:@"😀$ti tailx"] != NO,
                   "A stale source must dismiss without consuming Tab or replacing text");
      [editor insertText:@"😀$ti tail" replacementRange:NSMakeRange(0, editor.string.length)];
      editor.selectedRange = NSMakeRange(5, 0);
      completion.update();
      AO_INVARIANT(providerCalls == 5 && findShownCompletionTable() != nil);
      editor.selectedRange = NSMakeRange(5, 1);
      AO_INVARIANT(!completion.tryApplySelected() && [editor.string isEqual:@"😀$ti tail"] != NO &&
                     findShownCompletionTable() == nil,
                   "A live selection must invalidate a pending replacement even when its start did not move");
      editor.selectedRange = NSMakeRange(5, 0);
      completion.update();
      AO_INVARIANT(providerCalls == 6 && findShownCompletionTable() != nil);
      [NSNotificationCenter.defaultCenter postNotificationName:NSApplicationDidResignActiveNotification object:NSApp];
      AO_INVARIANT(findShownCompletionTable() == nil, "Leaving the application must dismiss completion");
      // A lone surrogate has no UTF-8 offset to hand the provider.
      [editor insertText:[NSString stringWithCharacters:std::to_array<unichar>({0xD83D, u'$'}).data() length:2]
        replacementRange:NSMakeRange(0, editor.string.length)];
      editor.selectedRange = NSMakeRange(2, 0);
      completion.update();
      AO_INVARIANT(providerCalls == 6 && findShownCompletionTable() == nil,
                   "Text that cannot convert to UTF-8 must not reach the provider");
      completion.detach();
      completion.update();
      AO_INVARIANT(providerCalls == 6 && !completion.tryHandleCommand(@selector(insertTab:)),
                   "Detached completion must not query providers or consume commands");
      [fixture.window() makeFirstResponder:nil];
      [field removeFromSuperview];
    }

    void verifyEntryCompletion(EditorFixture& fixture, std::vector<TrackId> const& tracks)
    {
      auto& model = fixture.model();

      // Metadata dictionary completion runs through the artist field's real
      // fixture vocabulary; the inserted text is expected from the scanned
      // fixture metadata, not from any shared completion algorithm.
      requireAdmission(model.beginProperties({tracks[0]}), "The completion properties draft must begin");
      fixture.present();
      auto* const artist = requireTextField(fixture, @"artist");
      editText(fixture.editor().window, @"artist", @"Aobus");
      settleNativeCallbacks();
      auto* const table = findShownCompletionTable();
      AO_INVARIANT(
        table != nil && table.numberOfRows >= 1, "An artist dictionary prefix must open the completion list");
      AO_INVARIANT([table.identifier isEqualToString:@"completion-list"] != 0 &&
                     table.window != fixture.editor().window && table.window != fixture.window(),
                   "The completion list must live in its own shown window outside the editor tree");
      auto* const artistEditor = static_cast<NSTextView*>(artist.currentEditor);
      AO_INVARIANT(artistEditor != nil, "Artist completion must run through the native field editor");
      [artistEditor doCommandBySelector:@selector(insertTab:)];
      settleNativeCallbacks();
      AO_INVARIANT([artistEditor.string isEqualToString:@"Aobus Fixture Artist"] != 0,
                   "Tab acceptance must insert the fixture's real artist value");
      auto const artistField = model.state().fields.at(static_cast<std::size_t>(artist.tag));
      AO_INVARIANT(
        artistEditor.selectedRange.location == artistEditor.string.length && artistEditor.selectedRange.length == 0,
        "Acceptance must leave the caret at the end of the inserted value");
      AO_INVARIANT(artistField.text == "Aobus Fixture Artist" && artistField.changed && model.state().dirty,
                   "Acceptance must publish the edited artist value into the model");
      AO_INVARIANT(findShownCompletionTable() == nil, "Acceptance must dismiss the completion popover");
      fixture.finish();

      // Query expression completion for a system field prefix.
      requireAdmission(model.beginList(), "The expression completion draft must begin");
      fixture.present();
      auto* const expression = requireTextField(fixture, @"Expression");
      editText(fixture.editor().window, @"Expression", @"$ti");
      settleNativeCallbacks();
      AO_INVARIANT(findShownCompletionTable() != nil, "A system field prefix must complete query expressions");
      auto* const expressionEditor = static_cast<NSTextView*>(expression.currentEditor);
      AO_INVARIANT(expressionEditor != nil, "Expression completion must run through the native field editor");
      [expressionEditor doCommandBySelector:@selector(insertTab:)];
      settleNativeCallbacks();
      AO_INVARIANT(
        [expressionEditor.string isEqualToString:@"$title"] != 0, "Tab must insert the advertised system field name");
      AO_INVARIANT(expressionEditor.selectedRange.location == static_cast<NSUInteger>(6) &&
                     expressionEditor.selectedRange.length == 0,
                   "The caret must follow the inserted field name");
      AO_INVARIANT(model.state().list.expression == "$title" && model.state().dirty,
                   "Expression acceptance must publish the edited draft value");
      AO_INVARIANT(findShownCompletionTable() == nil, "Expression acceptance must dismiss completion");

      // A visible popover must not survive its editor's teardown.
      editText(fixture.editor().window, @"Name", @"Completion teardown probe");
      editText(fixture.editor().window, @"Expression", @"$");
      settleNativeCallbacks();
      AO_INVARIANT(findShownCompletionTable() != nil, "The teardown probe must open completion first");
      fixture.finish();
      settleNativeCallbacks();
      AO_INVARIANT(fixture.editor() == nil && findShownCompletionTable() == nil,
                   "Finishing with a visible popover must tear the completion down with its editor");
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

    void verifyMembershipNotifications(EditorFixture& fixture, std::vector<TrackId> const& tracks)
    {
      auto& session = fixture.session();
      auto& model = fixture.model();

      requireAdmission(model.beginList(), "The notification probe list must begin");
      model.editList("Notification probe list", "", "#aobus_notification_probe");
      model.save();
      requireWaitUntil([&] { return model.state().completed; }, "The notification probe list must save");
      auto const probeListId = model.state().savedListId;
      AO_INVARIANT(probeListId != kInvalidListId, "The notification probe list must publish its id");
      model.cancel();

      struct PostedObservation final
      {
        std::vector<rt::NotificationEntry> posted{};
        std::vector<rt::NotificationId> known{};
      };
      auto observationPtr = std::make_shared<PostedObservation>();

      for (auto const& entry : session.runtime().notifications().feed().entries)
      {
        observationPtr->known.push_back(entry.id);
      }

      auto feedSub = session.runtime().notifications().onFeedUpdated(
        [observationPtr](rt::NotificationFeedUpdate const& update)
        {
          if (update.feedPtr == nullptr)
          {
            return;
          }

          for (auto const& entry : update.feedPtr->entries)
          {
            if (!std::ranges::contains(observationPtr->known, entry.id))
            {
              observationPtr->known.push_back(entry.id);
              observationPtr->posted.push_back(entry);
            }
          }
        });

      auto const expectedNotification =
        [&](uimodel::ListMembershipOperation operation, rt::AuthoringStatus status, std::size_t changedTrackCount)
      {
        return uimodel::listMembershipEditNotification(
          session.catalog(),
          uimodel::ListMembershipEditResult{
            .status = status,
            .listId = probeListId,
            .operation = operation,
            .listName = "Notification probe list",
            .tag = "aobus_notification_probe",
            .targetTrackCount = changedTrackCount,
            .changedTrackCount = status == rt::AuthoringStatus::NoOp ? 0 : changedTrackCount,
            .forgottenPositionCount = 0,
          });
      };

      auto const trackHasProbeTag = [&](TrackId trackId)
      {
        return std::ranges::contains(
          session.runtime().library().snapshot().selectionTags(std::vector{trackId}), "aobus_notification_probe");
      };

      auto const requirePosted = [&](std::size_t expectedCount)
      {
        requireWaitUntil([&] { return observationPtr->posted.size() >= expectedCount; },
                         "The membership notification feed must observe its posted update");
        AO_INVARIANT(observationPtr->posted.size() == expectedCount,
                     "Each live successful membership reply must append exactly one feed entry");
      };

      // The severity and lifetime policy is the shared uimodel mapping; its unit
      // test owns the Busy, Stale, and Unavailable cases, which contend for the
      // live write lane nondeterministically here.
      auto const requireLastPosted = [&](uimodel::ListMembershipEditNotification const& expected)
      {
        auto const& entry = observationPtr->posted.back();
        AO_INVARIANT(entry.severity == expected.severity && entry.lifetime == expected.lifetime &&
                       std::holds_alternative<std::string>(entry.message) &&
                       std::get<std::string>(entry.message) == expected.text,
                     "A completed membership edit must post exactly the shared feed notification: expected {} {} '{}', "
                     "observed {} {} '{}'",
                     static_cast<int>(expected.severity),
                     static_cast<int>(expected.lifetime.kind()),
                     expected.text,
                     static_cast<int>(entry.severity),
                     static_cast<int>(entry.lifetime.kind()),
                     std::holds_alternative<std::string>(entry.message) ? std::get<std::string>(entry.message)
                                                                        : std::string{"<non-text>"});
      };

      // Applied add: the busy contract settles into completed with one exact post.
      requireAdmission(
        model.beginMembership({tracks[0], tracks[1]}, probeListId, false), "The applied add membership must begin");
      AO_INVARIANT(model.state().busy, "A membership edit must enter the busy state on admission");
      requireWaitUntil([&] { return model.state().completed; }, "The applied add must complete");
      requirePosted(1);
      requireLastPosted(expectedNotification(uimodel::ListMembershipOperation::Add, rt::AuthoringStatus::Applied, 2));
      AO_INVARIANT(!model.state().busy && trackHasProbeTag(tracks[0]) && trackHasProbeTag(tracks[1]),
                   "The applied add must settle idle with both durable probe tags");
      model.cancel();

      // Applied remove of one captured track.
      requireAdmission(
        model.beginMembership({tracks[0]}, probeListId, true), "The applied remove membership must begin");
      requireWaitUntil([&] { return model.state().completed; }, "The applied remove must complete");
      requirePosted(2);
      requireLastPosted(
        expectedNotification(uimodel::ListMembershipOperation::Remove, rt::AuthoringStatus::Applied, 1));
      AO_INVARIANT(!trackHasProbeTag(tracks[0]) && trackHasProbeTag(tracks[1]),
                   "The applied remove must drop only the captured track's durable probe tag");
      model.cancel();

      // A genuine no-op re-add still completes and posts its shared no-op text.
      requireAdmission(
        model.beginMembership({tracks[1]}, probeListId, false), "The no-op re-add membership must begin");
      requireWaitUntil([&] { return model.state().completed; }, "The no-op re-add must complete");
      requirePosted(3);
      requireLastPosted(expectedNotification(uimodel::ListMembershipOperation::Add, rt::AuthoringStatus::NoOp, 1));
      model.cancel();

      // A failed authoring result publishes its editor error without any feed post.
      requireAdmission(model.beginMembership({tracks[0]}, ListId{9999}, false), "The failed membership must begin");
      requireWaitUntil([&] { return !model.state().busy && !model.state().error.empty(); },
                       "The failed membership must return its authoring error");
      AO_INVARIANT(
        observationPtr->posted.size() == 3, "A failed membership edit must not post a completion notification");
      model.cancel();

      // Live positive control on a separately owned model borrowing the same runtime.
      auto controlPtr = std::make_shared<LibraryEditorModel>(session.runtime(), session.catalog(), [] {});
      requireAdmission(
        controlPtr->beginMembership({tracks[0]}, probeListId, false), "The control membership must begin");
      requireWaitUntil([&] { return controlPtr->state().completed; }, "The control membership must complete");
      requirePosted(4);
      requireLastPosted(expectedNotification(uimodel::ListMembershipOperation::Add, rt::AuthoringStatus::Applied, 1));
      controlPtr->cancel();
      controlPtr->shutdown();
      fixture.retainMembershipModel(controlPtr);

      // A shutdown before the callback lands retires the completed edit: no post.
      auto retiredPtr = std::make_shared<LibraryEditorModel>(session.runtime(), session.catalog(), [] {});
      requireAdmission(
        retiredPtr->beginMembership({tracks[1]}, probeListId, true), "The retired membership must begin");
      retiredPtr->shutdown();
      fixture.retainMembershipModel(retiredPtr);
      requireWaitUntil(
        [&] { return !trackHasProbeTag(tracks[1]); }, "The retired membership must still commit its durable removal");
      settleNativeCallbacks();
      AO_INVARIANT(
        observationPtr->posted.size() == 4, "A shutdown before the completion callback must retire the notification");

      // A reentrant shutdown from the completed change callback also suppresses the post.
      auto const reentrantBorrowPtr = std::make_shared<std::weak_ptr<LibraryEditorModel>>();
      auto const shutdownObservedPtr = std::make_shared<bool>(false);
      auto reentrantPtr = std::make_shared<LibraryEditorModel>(session.runtime(),
                                                               session.catalog(),
                                                               [reentrantBorrowPtr, shutdownObservedPtr]
                                                               {
                                                                 if (auto const modelPtr = reentrantBorrowPtr->lock();
                                                                     modelPtr && modelPtr->state().completed)
                                                                 {
                                                                   modelPtr->shutdown();
                                                                   *shutdownObservedPtr = true;
                                                                 }
                                                               });
      *reentrantBorrowPtr = reentrantPtr;
      fixture.retainMembershipModel(reentrantPtr);
      requireAdmission(
        reentrantPtr->beginMembership({tracks[1]}, probeListId, false), "The reentrant membership must begin");
      requireWaitUntil([&] { return trackHasProbeTag(tracks[1]) && *shutdownObservedPtr; },
                       "The committed membership must reach its reentrant shutdown callback");
      settleNativeCallbacks();
      AO_INVARIANT(
        observationPtr->posted.size() == 4, "A reentrant shutdown must suppress the completed edit's notification");
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
    verifyRecordingDateProperty(fixture, tracks[0]);
    verifyNativeRecordingDateControl(fixture, tracks[0]);
    verifyPropertyAuthoring(fixture, tracks, stateRoot);
    auto const listId = verifyStaleDraft(fixture, tracks, stateRoot);
    verifyStaleCloseContract(fixture, tracks, listId);
    verifyListAuthoring(fixture, tracks, listId, stateRoot);
    verifyCompletionRanges(fixture);
    verifyEntryCompletion(fixture, tracks);
    verifyMembershipNotifications(fixture, tracks);
    return 0;
  }

  std::int32_t runCreditsControlsScenario(std::filesystem::path const& musicRoot,
                                          std::filesystem::path const& stateRoot)
  {
    auto fixture = EditorFixture{musicRoot, stateRoot};
    auto const tracks = firstThreeTracks(fixture.session());
    verifyNativeCreditsControls(fixture, tracks, stateRoot);
    return 0;
  }

  std::int32_t runCreditsPreviewsScenario(std::filesystem::path const& musicRoot,
                                          std::filesystem::path const& stateRoot)
  {
    auto fixture = EditorFixture{musicRoot, stateRoot};
    auto const tracks = firstThreeTracks(fixture.session());
    auto const expected =
      std::vector<library::Credit>{{.name = "Ada", .role = "piano"}, {.name = "Ada", .role = "piano"}};
    auto& model = fixture.model();
    requireAdmission(model.beginProperties({tracks[0]}), "Preview preserved-segment arrangement must begin");
    requireAdmission(model.beginCreditsEdit(uimodel::allTrackCreditKinds()), "Preview Credits arrangement must begin");

    for (std::size_t index = 0; index < expected.size(); ++index)
    {
      model.creditsEditor().addEntry(expected[index].kind);
      model.creditsEditor().updateName(index, expected[index].name);
      model.creditsEditor().updateRole(index, expected[index].role);
    }

    requireAdmission(model.acceptCreditsEdit(), "Preview preserved-segment arrangement must stage");
    model.save();
    requireWaitUntil([&] { return model.state().completed; }, "Preview preserved-segment arrangement must commit");
    model.cancel();
    auto const optCredits = fixture.session().runtime().library().snapshot().trackCredits(tracks[0]);
    AO_INVARIANT(optCredits && *optCredits == expected,
                 "Preview isolation must preserve the original nonempty duplicate-Performer oracle");
    verifyNativeCreditPreviews(fixture, tracks[0]);
    return 0;
  }
} // namespace ao::appkit::test
