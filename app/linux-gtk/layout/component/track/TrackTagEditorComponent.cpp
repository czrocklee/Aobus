// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "tag/TagEditController.h"
#include "tag/TagEditor.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/widget.h>
#include <sigc++/scoped_connection.h>

#include <memory>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    class TrackTagEditorComponent final : public ILayoutComponent
    {
    public:
      TrackTagEditorComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
        : _writer{ctx.runtime.library().writer()}, _sources{ctx.runtime.sources()}
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
              if (auto const reply = _writer.editTags(_currentTrackIds, toAdd, toRemove); !reply.mutatedIds.empty())
              {
                _sources.allTracks().notifyUpdated(reply.mutatedIds);
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
        _tagEditor.setup(ctx.runtime.library(), _currentTrackIds);
        _tagEditor.set_visible(true);
      }

      TagEditor _tagEditor;
      rt::LibraryWriter& _writer;
      rt::ListSourceStore& _sources;
      std::vector<TrackId> _currentTrackIds;
      sigc::scoped_connection _scopeConn;
    };

    std::unique_ptr<ILayoutComponent> createTrackTagEditor(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackTagEditorComponent>(ctx, node);
    }
  } // namespace

  void registerTrackTagEditorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "track.tagEditor",
                                .displayName = "Tag Editor",
                                .category = ComponentCategory::Track,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createTrackTagEditor);
  }
} // namespace ao::gtk::layout
