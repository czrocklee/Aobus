// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "tag/TagEditController.h"
#include "tag/TagEditor.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/property/TagEditWorkflow.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <memory>
#include <span>
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
      TrackTagEditorComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _library{ctx.runtime.library()}
        , _notifications{ctx.runtime.notifications()}
        , _tagEditController{ctx.dependencies.tagEditController}
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
            if (_tagEditController != nullptr)
            {
              _tagEditController->submitTagChanges(
                TrackSelection{.listId = kInvalidListId, .selectedIds = _currentTrackIds}, toAdd, toRemove);
            }
            else
            {
              submitTagsWithoutController(toAdd, toRemove);
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
      void submitTagsWithoutController(std::span<std::string const> tagsToAdd,
                                       std::span<std::string const> tagsToRemove)
      {
        if (_tagEditSessionPtr == nullptr || !std::ranges::equal(_tagEditSessionPtr->targetIds(), _currentTrackIds))
        {
          auto sessionResult = uimodel::TrackAuthoringSession::begin(_library, _currentTrackIds);

          if (!sessionResult)
          {
            APP_LOG_ERROR("Tag edit could not start: {}", sessionResult.error().message);
            _notifications.post(rt::NotificationSeverity::Error, sessionResult.error().message);
            return;
          }

          _tagEditSessionPtr = std::move(*sessionResult);
        }

        auto request = uimodel::TagEditRequest{.selectedIds = _currentTrackIds};
        request.tagsToAdd.assign(tagsToAdd.begin(), tagsToAdd.end());
        request.tagsToRemove.assign(tagsToRemove.begin(), tagsToRemove.end());

        auto workflow = uimodel::TagEditWorkflow{*_tagEditSessionPtr};
        auto const result = workflow.apply(request);

        if (result.rejected || result.stale)
        {
          APP_LOG_ERROR("Tag edit failed: {}", result.notificationText);
          _notifications.post(rt::NotificationSeverity::Error, result.notificationText);

          if (result.stale)
          {
            _tagEditSessionPtr.reset();
          }

          return;
        }

        if (result.applied)
        {
          _notifications.post(rt::NotificationSeverity::Info, result.notificationText);
        }
      }

      void handleSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        if (!std::ranges::equal(_currentTrackIds, snap.trackIds))
        {
          _tagEditSessionPtr.reset();
        }

        _currentTrackIds = snap.trackIds;
        _tagEditor.setup(_library, _currentTrackIds);
        _tagEditor.set_visible(true);
      }

      TagEditor _tagEditor;
      rt::Library& _library;
      rt::NotificationService& _notifications;
      TagEditController* _tagEditController;
      std::unique_ptr<uimodel::TrackAuthoringSession> _tagEditSessionPtr;
      std::vector<TrackId> _currentTrackIds;
      sigc::scoped_connection _scopeConn;
    };

    std::unique_ptr<LayoutComponent> createTrackTagEditor(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackTagEditorComponent>(ctx, node);
    }
  } // namespace

  void registerTrackTagEditorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.tagEditor",
                                .displayName = "Tag Editor",
                                .category = LayoutComponentCategory::Track,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackTagEditor);
  }
} // namespace ao::gtk::layout
