// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "app/GtkStyleRuntime.h"
#include "image/ImageWidget.h"
#include "image/ResourceImageController.h"
#include "image/ThumbnailLoader.h"
#include "layout/LayoutConstants.h"
#include "tag/TagPopover.h"
#include "track/TrackColumnFactoryBuilder.h"
#include "track/TrackColumnViewHost.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/field/TrackFieldEditCodec.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/field/TrackInlineEditWorkflow.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <gdkmm/rectangle.h>
#include <glib/gtypes.h>
#include <glibmm/main.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/wrap.h>
#include <gobject/gobject.h>
#include <gtk/gtkstyleprovider.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listheader.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/object.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/selectionmodel.h>
#include <gtkmm/signallistitemfactory.h>
#include <gtkmm/sorter.h>
#include <gtkmm/sortlistmodel.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    std::string trackCountLabel(::guint count)
    {
      return std::format("{} {}", count, count == 1 ? "track" : "tracks");
    }

    TrackRowObject* trackRowFromSorterItem(gpointer item)
    {
      auto* const object = static_cast<GObject*>(item);
      return object != nullptr ? dynamic_cast<TrackRowObject*>(Glib::wrap_auto(object, false)) : nullptr;
    }

    class ProjectionGroupSectionSorter final : public Gtk::Sorter
    {
    public:
      static Glib::RefPtr<ProjectionGroupSectionSorter> create(Glib::RefPtr<TrackListModel> modelPtr)
      {
        return Glib::make_refptr_for_instance<ProjectionGroupSectionSorter>(
          new ProjectionGroupSectionSorter{std::move(modelPtr)});
      }

    protected:
      explicit ProjectionGroupSectionSorter(Glib::RefPtr<TrackListModel> modelPtr)
        : Glib::ObjectBase{typeid(ProjectionGroupSectionSorter)}, Gtk::Sorter{}, _modelPtr{std::move(modelPtr)}
      {
      }

      Gtk::Ordering compare_vfunc(::gpointer item1, ::gpointer item2) override
      {
        auto const* const lhs = trackRowFromSorterItem(item1);
        auto const* const rhs = trackRowFromSorterItem(item2);

        if (lhs == nullptr || rhs == nullptr)
        {
          return Gtk::Ordering::EQUAL;
        }

        auto const optLhsGroup = _modelPtr->groupIndexForTrack(lhs->trackId());
        auto const optRhsGroup = _modelPtr->groupIndexForTrack(rhs->trackId());

        if (!optLhsGroup || !optRhsGroup || *optLhsGroup == *optRhsGroup)
        {
          return Gtk::Ordering::EQUAL;
        }

        return *optLhsGroup < *optRhsGroup ? Gtk::Ordering::SMALLER : Gtk::Ordering::LARGER;
      }

      Order get_order_vfunc() override { return Order::PARTIAL; }

    private:
      Glib::RefPtr<TrackListModel> _modelPtr;
    };

    void configureSectionLabel(Gtk::Label& label)
    {
      label.set_halign(Gtk::Align::START);
      label.set_ellipsize(Pango::EllipsizeMode::END);
      label.set_single_line_mode(true);
      label.set_lines(1);
      label.set_xalign(0.0F);
    }

    class TrackSectionCoverSlot final : public Gtk::Widget
    {
    public:
      TrackSectionCoverSlot(ImageWidget& imageWidget, std::int32_t size)
        : _imageWidget{imageWidget}, _size{std::max(0, size)}
      {
        set_overflow(Gtk::Overflow::HIDDEN);
        _imageWidget.setTargetSize(_size);
        _imageWidget.setMaxRenderSize(_size, _size);
        _imageWidget.setForceSquareTarget(true);
        _imageWidget.set_halign(Gtk::Align::CENTER);
        _imageWidget.set_valign(Gtk::Align::CENTER);
        _imageWidget.set_expand(false);
        _imageWidget.set_overflow(Gtk::Overflow::HIDDEN);
        _imageWidget.set_parent(*this);
      }

      ~TrackSectionCoverSlot() override { _imageWidget.unparent(); }

      TrackSectionCoverSlot(TrackSectionCoverSlot const&) = delete;
      TrackSectionCoverSlot& operator=(TrackSectionCoverSlot const&) = delete;
      TrackSectionCoverSlot(TrackSectionCoverSlot&&) = delete;
      TrackSectionCoverSlot& operator=(TrackSectionCoverSlot&&) = delete;

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::CONSTANT_SIZE; }

      void measure_vfunc(Gtk::Orientation /*orientation*/,
                         int /*forSize*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimum = _size;
        natural = minimum;
        minimumBaseline = -1;
        naturalBaseline = -1;
      }

      void size_allocate_vfunc(int width, int height, int baseline) override
      {
        auto const side = std::min({width, height, _size});
        auto const childX = std::max(0, (width - side) / 2);
        auto const childY = std::max(0, (height - side) / 2);

        measureImageForAllocation(side);
        _imageWidget.size_allocate(Gtk::Allocation{childX, childY, side, side}, baseline);
      }

    private:
      void measureImageForAllocation(std::int32_t side) const
      {
        std::int32_t minimum = 0;
        std::int32_t natural = 0;
        std::int32_t minimumBaseline = -1;
        std::int32_t naturalBaseline = -1;
        _imageWidget.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
        _imageWidget.measure(Gtk::Orientation::VERTICAL, side, minimum, natural, minimumBaseline, naturalBaseline);
      }

      ImageWidget& _imageWidget;
      std::int32_t _size = 0;
    };

    class TrackSectionHeaderWidget final : public Gtk::Box
    {
    public:
      TrackSectionHeaderWidget(rt::Library const& reads, ThumbnailLoader& thumbnailLoader)
        : Gtk::Box{Gtk::Orientation::HORIZONTAL}
        , _coverArtSlot{_coverArt, layout::kSectionCoverLogicalSize}
        , _coverArtController{_coverArt, reads, thumbnailLoader.cache()}
      {
        set_spacing(layout::kSpacingXLarge);
        add_css_class("ao-track-section-box");

        _coverArtController.enableThumbnailMode(thumbnailLoader, layout::kSectionCoverLogicalSize);
        _coverArtSlot.add_css_class("ao-track-section-cover");
        _coverArtSlot.set_valign(Gtk::Align::CENTER);
        append(_coverArtSlot);

        auto* const vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        vbox->set_hexpand(true);
        vbox->set_valign(Gtk::Align::CENTER);
        vbox->set_spacing(layout::kSpacingSmall);
        append(*vbox);

        configureSectionLabel(_primaryLabel);
        _primaryLabel.add_css_class("ao-track-section-title");
        _primaryLabel.add_css_class("title-3");
        vbox->append(_primaryLabel);

        auto* const subtitleBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        subtitleBox->set_hexpand(true);
        subtitleBox->set_spacing(layout::kSpacingMedium);
        subtitleBox->add_css_class("ao-track-section-subtitle");
        vbox->append(*subtitleBox);

        configureSectionLabel(_secondaryLabel);
        _secondaryLabel.add_css_class("dim-label");
        subtitleBox->append(_secondaryLabel);

        _separatorLabel.set_text("•");
        _separatorLabel.add_css_class("dim-label");
        _separatorLabel.set_halign(Gtk::Align::CENTER);
        subtitleBox->append(_separatorLabel);

        configureSectionLabel(_tertiaryLabel);
        _tertiaryLabel.add_css_class("dim-label");
        subtitleBox->append(_tertiaryLabel);

        configureSectionLabel(_countLabel);
        _countLabel.add_css_class("dim-label");
        subtitleBox->append(_countLabel);
      }

      void bind(rt::TrackGroupSectionSnapshot const& snap, ::guint count, bool reserveCoverSlot)
      {
        _primaryLabel.set_text(snap.primaryText);
        _secondaryLabel.set_text(snap.secondaryText);
        _tertiaryLabel.set_text(snap.tertiaryText);

        if (auto const countText = "(" + trackCountLabel(count) + ")";
            snap.secondaryText.empty() && snap.tertiaryText.empty())
        {
          _countLabel.set_text(countText);
        }
        else
        {
          _countLabel.set_text("• " + countText);
        }

        _secondaryLabel.set_visible(!snap.secondaryText.empty());
        _tertiaryLabel.set_visible(!snap.tertiaryText.empty());
        _separatorLabel.set_visible(!snap.secondaryText.empty() && !snap.tertiaryText.empty());
        _coverArtSlot.set_visible(reserveCoverSlot);

        if (reserveCoverSlot && snap.imageId != kInvalidResourceId)
        {
          _coverArtController.load(snap.imageId);
        }
        else
        {
          _coverArtController.clear();
        }
      }

    private:
      ImageWidget _coverArt;
      TrackSectionCoverSlot _coverArtSlot;
      ResourceImageController _coverArtController;
      Gtk::Label _primaryLabel;
      Gtk::Label _secondaryLabel;
      Gtk::Label _separatorLabel;
      Gtk::Label _tertiaryLabel;
      Gtk::Label _countLabel;
    };
  } // namespace

  TrackViewPage::TrackViewPage(ListId listId,
                               Glib::RefPtr<TrackListModel> modelPtr,
                               uimodel::TrackColumnLayoutStore& layoutStore,
                               rt::AppRuntime& runtime,
                               ThumbnailLoader& thumbnailLoader,
                               rt::ViewId viewId)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _viewId{viewId}
    , _modelPtr{std::move(modelPtr)}

    , _layoutStore{layoutStore}
    , _runtime{runtime}
    , _thumbnailLoader{thumbnailLoader}
    , _groupModelPtr{Gtk::SortListModel::create(_modelPtr, Glib::RefPtr<Gtk::Sorter>{})}
    , _selectionModelPtr{Gtk::MultiSelection::create(_groupModelPtr)}
    , _viewHostPtr{std::make_unique<TrackColumnViewHost>(_modelPtr, _layoutStore, _selectionModelPtr, listId)}
  {
    _layoutStore.setActiveListId(_listId);
    _viewHostPtr->setupSelectionActivation();

    _themeRefreshConnection = GtkStyleRuntime::instance().signalRefreshed().connect(
      [this]
      {
        APP_LOG_INFO("Executing theme refresh for TrackViewPage...");
        _viewHostPtr->columnController().updateTitlePositionVariable();
        _viewHostPtr->columnView().queue_draw();
      });

    setupStatusBar();
    setupHeaderFactory();

    // 1. Configure columns and layout first (Off-tree)
    _viewHostPtr->setupColumns(
      [this](rt::TrackField field)
      { return buildColumnFactory(field, std::bind_front(&TrackViewPage::commitMetadataChange, this), *_modelPtr); });

    if (_viewId != rt::kInvalidViewId)
    {
      auto const& presState = _runtime.views().trackListState(_viewId).presentation;
      _viewHostPtr->columnController().setLayoutAndApply(presState.visibleFields);
    }
    else
    {
      _viewHostPtr->columnController().syncLayout(rt::defaultTrackPresentationSpec().visibleFields);
    }

    // 2. Configure decorators and styles
    updateSectionHeaders();

    setupColumnViewStyles(_viewHostPtr->columnView());

    _contextPopover.set_has_arrow(false);

    // 3. Finally attach model and add to scrolled window
    _viewHostPtr->columnView().set_model(_selectionModelPtr);

    _scrolledWindow.set_child(_viewHostPtr->columnView());
    _scrolledWindow.set_vexpand(true);
    _scrolledWindow.set_hexpand(true);
    _contextPopover.set_parent(_viewHostPtr->columnView());

    append(_statusLabel);
    append(_scrolledWindow);
  }

  TrackViewPage::~TrackViewPage() = default;

  void TrackViewPage::on_map()
  {
    Gtk::Box::on_map();
    _layoutStore.setActiveListId(_listId);
  }

  void TrackViewPage::setupHeaderFactory()
  {
    _sectionHeaderFactoryPtr = Gtk::SignalListItemFactory::create();

    _sectionHeaderFactoryPtr->signal_setup_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& object)
      {
        auto const headerPtr = std::dynamic_pointer_cast<Gtk::ListHeader>(object);

        if (!headerPtr)
        {
          return;
        }

        auto* const widget = Gtk::make_managed<TrackSectionHeaderWidget>(_runtime.library(), _thumbnailLoader);
        headerPtr->set_child(*widget);
      });

    _sectionHeaderFactoryPtr->signal_bind_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& object)
      {
        auto const headerPtr = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
        auto* const widget = headerPtr ? dynamic_cast<TrackSectionHeaderWidget*>(headerPtr->get_child()) : nullptr;

        if (headerPtr == nullptr || widget == nullptr)
        {
          return;
        }

        auto snap = rt::TrackGroupSectionSnapshot{};
        bool reserveCoverSlot = false;

        if (auto* const proj = _modelPtr->projection(); proj != nullptr)
        {
          reserveCoverSlot = proj->presentation().groupBy == rt::TrackGroupKey::Album;
          auto const start = headerPtr->get_start();

          if (auto const optGroupIndex = proj->groupIndexAt(start); optGroupIndex)
          {
            snap = proj->groupAt(*optGroupIndex);

            // Warm the next section's cover so its header binds from cache as the
            // user scrolls down. Decode budget mirrors the section thumbnail size.
            if (auto const nextGroupIndex = *optGroupIndex + 1; nextGroupIndex < proj->groupCount())
            {
              auto const nextImageId = proj->groupAt(nextGroupIndex).imageId;
              auto const scale = std::max(1, _viewHostPtr->columnView().get_scale_factor());
              _thumbnailLoader.prefetch(nextImageId, layout::kSectionCoverLogicalSize * scale);
            }
          }
        }

        widget->bind(snap, headerPtr->get_n_items(), reserveCoverSlot);
      });

    _groupModelPtr->set_section_sorter(ProjectionGroupSectionSorter::create(_modelPtr));
    _viewHostPtr->columnView().set_header_factory(_sectionHeaderFactoryPtr);
  }

  void TrackViewPage::setupStatusBar()
  {
    _statusLabel.set_visible(false);
    _statusLabel.set_halign(Gtk::Align::START);
    _statusLabel.set_valign(Gtk::Align::CENTER);
    _statusLabel.add_css_class("ao-track-status-message");

    auto const contextPtr = _statusLabel.get_style_context();
    contextPtr->add_class("dim-label");
  }

  void TrackViewPage::setStatusMessage(std::string_view message)
  {
    _statusLabel.set_text(std::string{message});

    if (message.empty())
    {
      clearStatusMessage();
      return;
    }

    _statusLabel.set_visible(true);
  }

  void TrackViewPage::clearStatusMessage()
  {
    _statusLabel.set_text("");
    _statusLabel.set_visible(false);
  }

  void TrackViewPage::applyPresentation(rt::TrackPresentationSpec const& presentation)
  {
    rebuildColumnView(presentation.visibleFields);
  }

  void TrackViewPage::setPlayingTrackId(TrackId trackId)
  {
    _playingTrackId = trackId;
    _viewHostPtr->selectionController().setPlayingTrackId(trackId);
  }

  void TrackViewPage::rebuildColumnView(std::span<rt::TrackField const> visibleFields)
  {
    auto const factoryProvider = [this](rt::TrackField field)
    { return buildColumnFactory(field, std::bind_front(&TrackViewPage::commitMetadataChange, this), *_modelPtr); };

    // 1. Detach UI from Model and Tree immediately.
    _viewHostPtr->columnView().set_model(Glib::RefPtr<Gtk::SelectionModel>{});
    _scrolledWindow.unset_child();
    _contextPopover.unparent();

    // 2. Create a new generation off-tree.
    auto& newView = _viewHostPtr->rebuild(_modelPtr, _layoutStore, _selectionModelPtr, factoryProvider, _listId);

    // 3. Configure structural properties before attaching model (Safe)
    setupColumnViewStyles(newView);

    _viewHostPtr->columnController().setLayoutAndApply(visibleFields);
    _viewHostPtr->columnController().updateTitlePositionVariable();

    // 4. Apply decorations (Section Headers)
    updateSectionHeaders();

    // 5. Attach the model first so items_changed has a listener
    newView.set_model(_selectionModelPtr);

    // 6. Restore playing state after model is attached
    if (_playingTrackId != kInvalidTrackId)
    {
      _viewHostPtr->selectionController().setPlayingTrackId(_playingTrackId);
    }

    // 7. Swap the child in the live UI tree
    _scrolledWindow.set_child(newView);
    _contextPopover.set_parent(newView);

    // 8. Restore scroll position to selection if possible (Deferred to idle for stability)
    Glib::signal_idle().connect_once(
      [this]
      {
        if (auto const primaryId = _viewHostPtr->selectionController().primarySelectedTrackId();
            primaryId != kInvalidTrackId)
        {
          _viewHostPtr->selectionController().scrollToTrack(primaryId);
        }
      });

    _viewHostPtr->setupSelectionActivation();
  }

  void TrackViewPage::updateSectionHeaders()
  {
    auto* const proj = _modelPtr->projection();
    auto const groupBy = proj != nullptr ? proj->presentation().groupBy : rt::TrackGroupKey::None;

    if (groupBy == rt::TrackGroupKey::None)
    {
      _groupModelPtr->set_section_sorter({});
      _viewHostPtr->columnView().set_header_factory({});
      _viewHostPtr->columnView().set_show_row_separators(true);
      return;
    }

    _groupModelPtr->set_section_sorter(ProjectionGroupSectionSorter::create(_modelPtr));
    _viewHostPtr->columnView().set_header_factory(_sectionHeaderFactoryPtr);
    _viewHostPtr->columnView().set_show_row_separators(false);
  }

  TrackViewPage::CreateSmartListRequestedSignal& TrackViewPage::signalCreateSmartListRequested() noexcept
  {
    return _createSmartListRequested;
  }

  void TrackViewPage::showTagPopover(TagPopover& popover, double posX, double posY)
  {
    auto const rect = Gdk::Rectangle{static_cast<std::int32_t>(posX), static_cast<std::int32_t>(posY), 1, 1};

    if (popover.get_parent() != &_viewHostPtr->columnView())
    {
      popover.set_parent(_viewHostPtr->columnView());
    }

    popover.set_pointing_to(rect);
    popover.popup();
  }

  void TrackViewPage::commitMetadataChange(Glib::RefPtr<TrackRowObject> const& row,
                                           rt::TrackField field,
                                           std::string newValue)
  {
    auto const* uiDef = trackFieldUiDefinition(field);

    if (uiDef == nullptr || !canInlineEdit(*uiDef))
    {
      return;
    }

    auto const result = uimodel::TrackInlineEditWorkflow::apply(
      uimodel::TrackInlineEditRequest{
        .field = field, .oldText = row->fieldText(field).raw(), .newText = std::move(newValue)},
      uimodel::TrackInlineEditHooks{
        .parse = [uiDef](std::string_view text) -> Result<uimodel::TrackFieldEditValue>
        { return uiDef->parseInlineEdit(text); },
        .readCurrentValue = [row, field, uiDef] -> uimodel::TrackFieldEditValue
        { return uiDef->readRowEditValue(*row, field); },
        .applyValue = [row, field, uiDef](uimodel::TrackFieldEditValue const& value)
        { uiDef->applyRowEditValue(*row, value, field); },
        .writePatch = [field](rt::MetadataPatch& patch, uimodel::TrackFieldEditValue const& value)
        { uimodel::writeTrackFieldPatch(patch, field, value); },
        .commitPatch = [this, row](rt::MetadataPatch const& patch) -> rt::UpdateTrackMetadataReply
        {
          auto const trackIds = std::array{row->trackId()};
          return _runtime.library().writer().updateMetadata(trackIds, patch);
        },
      });

    switch (result.outcome)
    {
      case uimodel::TrackInlineEditOutcome::NoChange:
      case uimodel::TrackInlineEditOutcome::NotEditable: return;
      case uimodel::TrackInlineEditOutcome::ParseRejected: setStatusMessage(result.statusMessage); return;
      case uimodel::TrackInlineEditOutcome::MutationRejected:
        APP_LOG_ERROR("Metadata update failed: {}", result.statusMessage);
        return;
      case uimodel::TrackInlineEditOutcome::Applied: break;
    }

    clearStatusMessage();
  }

  void TrackViewPage::setupColumnViewStyles(Gtk::ColumnView& view)
  {
    view.set_reorderable(true);
    view.get_style_context()->add_provider(_viewHostPtr->cssProvider(), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
} // namespace ao::gtk
