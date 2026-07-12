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
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <memory>
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
        , _writer{_library.writer()}
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
              // Fallback if controller is missing
              auto const replyResult = _writer.editTags(_currentTrackIds, toAdd, toRemove);

              if (!replyResult)
              {
                APP_LOG_ERROR("Tag edit failed: {}", replyResult.error().message);
                return;
              }
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
      void handleSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        _currentTrackIds = snap.trackIds;
        _tagEditor.setup(_library, _currentTrackIds);
        _tagEditor.set_visible(true);
      }

      TagEditor _tagEditor;
      rt::Library& _library;
      rt::LibraryWriter& _writer;
      TagEditController* _tagEditController;
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
