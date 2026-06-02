// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/components/TrackEditorComponents.h"

#include "layout/components/TrackDetailComponents.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "tag/TagEditController.h"
#include "tag/TagEditor.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/TrackDetailProjection.h>
#include <ao/rt/TrackSource.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <memory>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    class TrackTagEditorComponent final : public ILayoutComponent
    {
    public:
      TrackTagEditorComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _mutation{ctx.runtime.mutation()}, _sources{ctx.runtime.sources()}
      {
        if (ctx.track.detailScope != nullptr)
        {
          _scopeConn = ctx.track.detailScope->signalSnapshotChanged().connect([this, &ctx](auto const& snap)
                                                                              { onSnapshot(ctx, snap); });
          onSnapshot(ctx, ctx.track.detailScope->snapshot());
        }

        _tagEditor.signalTagsChanged().connect(
          [this, &ctx](auto const& toAdd, auto const& toRemove)
          {
            if (ctx.tag.editController != nullptr)
            {
              ctx.tag.editController->submitTagChanges(
                TrackSelectionContext{.listId = kInvalidListId, .selectedIds = _currentTrackIds}, toAdd, toRemove);
            }
            else
            {
              // Fallback if controller is missing
              if (auto const result = _mutation.editTags(_currentTrackIds, toAdd, toRemove); result)
              {
                _sources.allTracks().notifyUpdated(_currentTrackIds);
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
      void onSnapshot(LayoutContext& ctx, rt::TrackDetailSnapshot const& snap)
      {
        _currentTrackIds = snap.trackIds;

        if (_currentTrackIds.empty())
        {
          _tagEditor.set_visible(false);
          return;
        }

        _tagEditor.setup(ctx.runtime.musicLibrary(), _currentTrackIds);
        _tagEditor.set_visible(true);
      }

      TagEditor _tagEditor;
      rt::LibraryMutationService& _mutation;
      rt::ListSourceStore& _sources;
      std::vector<TrackId> _currentTrackIds;
      sigc::scoped_connection _scopeConn;
    };
  } // namespace

  void registerTrackEditorComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.tagEditor",
                                .displayName = "Tag Editor",
                                .category = "Tracks",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TrackTagEditorComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
