// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/UiWorkflow.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "tag/TagEditController.h"
#include "tag/TagEditor.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/property/TagEdit.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class TrackTagEditorComponent final : public LayoutComponent
    {
    public:
      TrackTagEditorComponent(rt::AppRuntime& runtime,
                              TagEditController* tagEditController,
                              i18n::MessageCatalog const& textCatalog,
                              LayoutBuildContext const& ctx,
                              LayoutNode const& /*node*/)
        : _tagEditor{textCatalog, runtime.textOrderingPolicy()}
        , _textCatalog{textCatalog}
        , _runtime{runtime}
        , _library{runtime.library()}
        , _notifications{runtime.notifications()}
        , _tagEditController{tagEditController}
      {
        if (ctx.detailScope != nullptr)
        {
          _scopeConn =
            ctx.detailScope->signalSnapshotChanged().connect([this](auto const& snap) { handleSnapshot(snap); });
          handleSnapshot(ctx.detailScope->snapshot());
        }

        _tagEditor.signalTagsChanged().connect(
          [this](auto const& toAdd, auto const& toRemove)
          {
            auto tagsToAdd = std::vector<std::string>{toAdd.begin(), toAdd.end()};
            auto tagsToRemove = std::vector<std::string>{toRemove.begin(), toRemove.end()};

            if (_tagEditController != nullptr)
            {
              auto const selection = TrackSelection{.listId = kInvalidListId, .selectedIds = _currentTrackIds};
              _tagEditController->submitTagChanges(selection, std::move(tagsToAdd), std::move(tagsToRemove));
            }
            else
            {
              submitTagsWithoutController(std::move(tagsToAdd), std::move(tagsToRemove));
            }
          });
      }

      ~TrackTagEditorComponent() override = default;
      TrackTagEditorComponent(TrackTagEditorComponent const&) = delete;
      TrackTagEditorComponent& operator=(TrackTagEditorComponent const&) = delete;
      TrackTagEditorComponent(TrackTagEditorComponent&&) = delete;
      TrackTagEditorComponent& operator=(TrackTagEditorComponent&&) = delete;

      Gtk::Widget& widget() override { return _tagEditor; }

    private:
      void submitTagsWithoutController(std::vector<std::string> tagsToAdd, std::vector<std::string> tagsToRemove)
      {
        if (_tagEditSessionPtr == nullptr || !std::ranges::equal(_tagEditSessionPtr->targetIds(), _currentTrackIds))
        {
          auto sessionRes = uimodel::TrackAuthoringSession::begin(_library, _currentTrackIds);

          if (!sessionRes)
          {
            APP_LOG_ERROR("Tag edit could not start: {}", sessionRes.error().message);
            _notifications.post(
              rt::NotificationSeverity::Error, sessionRes.error().message, rt::NotificationLifetime::history());
            return;
          }

          _tagEditSessionPtr = std::move(*sessionRes);
          ++_tagEditSessionGeneration;
        }

        auto const sessionGeneration = _tagEditSessionGeneration;
        auto submission =
          uimodel::applyTagEdit(*_tagEditSessionPtr, _textCatalog, std::move(tagsToAdd), std::move(tagsToRemove));
        spawnUiTask(_runtime.async(),
                    _tasks,
                    *this,
                    "tag edit",
                    std::move(submission),
                    [sessionGeneration](TrackTagEditorComponent* owner, Result<uimodel::TagEditResult> result)
                    { owner->handleTagEditResult(std::move(result), sessionGeneration); });
      }

      void handleTagEditResult(Result<uimodel::TagEditResult> result, std::uint64_t const sessionGeneration)
      {
        if (!result)
        {
          APP_LOG_ERROR("Tag edit failed: {}", result.error().message);
          _notifications.post(
            rt::NotificationSeverity::Error, result.error().message, rt::NotificationLifetime::history());
          return;
        }

        if (result->status == rt::AuthoringStatus::Busy)
        {
          _notifications.post(
            rt::NotificationSeverity::Warning, result->notificationText, rt::NotificationLifetime::transient());
          return;
        }

        if (result->status == rt::AuthoringStatus::Stale || result->status == rt::AuthoringStatus::Unavailable)
        {
          APP_LOG_ERROR("Tag edit failed: {}", result->notificationText);
          _notifications.post(
            rt::NotificationSeverity::Error, result->notificationText, rt::NotificationLifetime::history());

          if (_tagEditSessionGeneration == sessionGeneration)
          {
            _tagEditSessionPtr.reset();
          }

          return;
        }

        if (result->status == rt::AuthoringStatus::Applied)
        {
          _notifications.post(
            rt::NotificationSeverity::Info, result->notificationText, rt::NotificationLifetime::transient());
        }
      }

      void handleSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        if (!std::ranges::equal(_currentTrackIds, snap.trackIds))
        {
          _tagEditSessionPtr.reset();
          ++_tagEditSessionGeneration;
        }

        _currentTrackIds = snap.trackIds;
        _tagEditor.setup(_library, _currentTrackIds);
        _tagEditor.set_visible(true);
      }

      TagEditor _tagEditor;
      i18n::MessageCatalog _textCatalog;
      rt::AppRuntime& _runtime;
      rt::Library& _library;
      rt::NotificationService& _notifications;
      TagEditController* _tagEditController;
      std::unique_ptr<uimodel::TrackAuthoringSession> _tagEditSessionPtr;
      std::uint64_t _tagEditSessionGeneration = 0;
      std::vector<TrackId> _currentTrackIds;
      sigc::scoped_connection _scopeConn;
      async::LifetimeScope _tasks;
    };
  } // namespace

  void registerTrackTagEditorComponent(ComponentRegistry& registry,
                                       rt::AppRuntime& runtime,
                                       TagEditController* tagEditController,
                                       i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "track.tagEditor",
       .displayName = "Tag Editor",
       .category = ComponentCategory::Track,
       .minChildren = 0,
       .optMaxChildren = 0},
      [&runtime, tagEditController, textCatalog](LayoutBuildContext const& ctx, LayoutNode const& node)
      { return std::make_unique<TrackTagEditorComponent>(runtime, tagEditController, textCatalog, ctx, node); });
  }
} // namespace ao::gtk::layout
