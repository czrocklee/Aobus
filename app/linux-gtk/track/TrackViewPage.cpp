// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "app/GtkStyleRuntime.h"
#include "image/ImageWidget.h"
#include "image/ImageWidgetLayout.h"
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
#include <ao/rt/Log.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackGroupHeadingPresentation.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <gdkmm/rectangle.h>
#include <glib/gtypes.h>
#include <glibmm/main.h>
#include <glibmm/object.h>
#include <glibmm/refptr.h>
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
#include <gtkmm/widget.h>
#include <pangomm/layout.h>
#include <sigc++/adaptors/track_obj.h>

#include <algorithm>
#include <array>
#include <cstdint>
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

        measureImageWidgetForSquareAllocation(_imageWidget, side);
        _imageWidget.size_allocate(Gtk::Allocation{childX, childY, side, side}, baseline);
      }

    private:
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
        auto const heading = uimodel::formatTrackGroupHeading(uimodel::PresentationTextCatalog{}, snap.heading);
        _primaryLabel.set_text(heading.primaryText);
        _secondaryLabel.set_text(heading.secondaryText);
        _tertiaryLabel.set_text(heading.tertiaryText);

        if (auto const countText = "(" + uimodel::formatTrackCount(count) + ")";
            heading.secondaryText.empty() && heading.tertiaryText.empty())
        {
          _countLabel.set_text(countText);
        }
        else
        {
          _countLabel.set_text("• " + countText);
        }

        _secondaryLabel.set_visible(!heading.secondaryText.empty());
        _tertiaryLabel.set_visible(!heading.tertiaryText.empty());
        _separatorLabel.set_visible(!heading.secondaryText.empty() && !heading.tertiaryText.empty());
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
    , _selectionModelPtr{Gtk::MultiSelection::create(_modelPtr)}
    , _viewHostPtr{std::make_unique<TrackColumnViewHost>(_modelPtr, _layoutStore, _selectionModelPtr, listId)}
  {
    _layoutStore.setActiveListId(_listId);
    _viewHostPtr->configureSelectionActivation();

    _themeRefreshConnection = GtkStyleRuntime::instance().signalRefreshed().connect(
      [this]
      {
        APP_LOG_INFO("Executing theme refresh for TrackViewPage...");
        _viewHostPtr->columnController().updateTitlePositionVariable();
        _viewHostPtr->columnView().queue_draw();
      });

    buildStatusBar();
    configureHeaderFactory();

    // 1. Configure columns and layout first (Off-tree)
    _viewHostPtr->configureColumns(
      [this](rt::TrackField field)
      {
        return buildColumnFactory(
          field,
          [this](Glib::RefPtr<TrackRowObject> const& rowPtr)
          {
            auto const trackIds = std::array{rowPtr->trackId()};
            return uimodel::TrackAuthoringSession::begin(_runtime.library(), trackIds);
          },
          std::bind_front(&TrackViewPage::commitMetadataChange, this),
          *_modelPtr);
      });

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

    applyColumnViewStyles(_viewHostPtr->columnView());

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

  TrackViewPage::~TrackViewPage()
  {
    _viewHostPtr->columnView().set_model(Glib::RefPtr<Gtk::SelectionModel>{});
    _contextPopover.unparent();
    _scrolledWindow.unset_child();
  }

  void TrackViewPage::on_map()
  {
    Gtk::Box::on_map();
    _layoutStore.setActiveListId(_listId);

    Glib::signal_idle().connect_once(sigc::track_object(
      [this]
      {
        if (auto const primaryId = _viewHostPtr->selectionController().primarySelectedTrackId();
            primaryId != kInvalidTrackId)
        {
          _viewHostPtr->selectionController().scrollToTrack(primaryId);
        }
      },
      *this));
  }

  void TrackViewPage::configureHeaderFactory()
  {
    _sectionHeaderFactoryPtr = Gtk::SignalListItemFactory::create();

    _sectionHeaderFactoryPtr->signal_setup_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& objectPtr)
      {
        auto const headerPtr = std::dynamic_pointer_cast<Gtk::ListHeader>(objectPtr);

        if (!headerPtr)
        {
          return;
        }

        auto* const widget = Gtk::make_managed<TrackSectionHeaderWidget>(_runtime.library(), _thumbnailLoader);
        headerPtr->set_child(*widget);
      });

    _sectionHeaderFactoryPtr->signal_bind_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& objectPtr)
      {
        auto const headerPtr = std::dynamic_pointer_cast<Gtk::ListHeader>(objectPtr);
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

    _viewHostPtr->columnView().set_header_factory(_sectionHeaderFactoryPtr);
  }

  void TrackViewPage::buildStatusBar()
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
    {
      return buildColumnFactory(
        field,
        [this](Glib::RefPtr<TrackRowObject> const& rowPtr)
        {
          auto const trackIds = std::array{rowPtr->trackId()};
          return uimodel::TrackAuthoringSession::begin(_runtime.library(), trackIds);
        },
        std::bind_front(&TrackViewPage::commitMetadataChange, this),
        *_modelPtr);
    };

    // 1. Detach UI from Model and Tree immediately.
    _viewHostPtr->columnView().set_model(Glib::RefPtr<Gtk::SelectionModel>{});
    _scrolledWindow.unset_child();
    _contextPopover.unparent();

    // 2. Create a new generation off-tree.
    auto& newView = _viewHostPtr->rebuild(_modelPtr, _layoutStore, _selectionModelPtr, factoryProvider, _listId);

    // 3. Configure structural properties before attaching model (Safe)
    applyColumnViewStyles(newView);

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
    Glib::signal_idle().connect_once(sigc::track_object(
      [this]
      {
        if (auto const primaryId = _viewHostPtr->selectionController().primarySelectedTrackId();
            primaryId != kInvalidTrackId)
        {
          _viewHostPtr->selectionController().scrollToTrack(primaryId);
        }
      },
      *this));

    _viewHostPtr->configureSelectionActivation();
  }

  void TrackViewPage::updateSectionHeaders()
  {
    auto* const proj = _modelPtr->projection();
    auto const groupBy = proj != nullptr ? proj->presentation().groupBy : rt::TrackGroupKey::None;

    if (groupBy == rt::TrackGroupKey::None)
    {
      _viewHostPtr->columnView().set_header_factory({});
      _viewHostPtr->columnView().set_show_row_separators(true);
      return;
    }

    _viewHostPtr->columnView().set_header_factory(_sectionHeaderFactoryPtr);
    _viewHostPtr->columnView().set_show_row_separators(false);
  }

  TrackViewPage::CreateSmartListRequestedSignal& TrackViewPage::signalCreateSmartListRequested() noexcept
  {
    return _createSmartListRequested;
  }

  void TrackViewPage::openTagPopover(TagPopover& popover, double xPosition, double yPosition)
  {
    auto const rect = Gdk::Rectangle{static_cast<std::int32_t>(xPosition), static_cast<std::int32_t>(yPosition), 1, 1};

    if (popover.get_parent() != &_viewHostPtr->columnView())
    {
      popover.set_parent(_viewHostPtr->columnView());
    }

    popover.set_pointing_to(rect);
    popover.popup();
  }

  void TrackViewPage::commitMetadataChange(Glib::RefPtr<TrackRowObject> const& rowPtr,
                                           rt::TrackField field,
                                           std::string newValue,
                                           uimodel::TrackAuthoringSession& session)
  {
    auto const* uiDef = trackFieldUiDefinition(field);

    if (uiDef == nullptr || !canInlineEdit(*uiDef))
    {
      return;
    }

    if (newValue == rowPtr->fieldText(field).raw())
    {
      return;
    }

    auto const editValueResult = uiDef->parseInlineEdit(newValue);

    if (!editValueResult)
    {
      setStatusMessage(editValueResult.error().message);
      return;
    }

    auto patch = rt::MetadataPatch{};

    if (!uimodel::writeTrackFieldPatch(patch, field, *editValueResult))
    {
      return;
    }

    auto const replyResult = session.submitMetadata(patch);

    if (!replyResult)
    {
      APP_LOG_ERROR("Metadata update failed: {}", replyResult.error().message);
      return;
    }

    switch (replyResult->status)
    {
      case rt::TrackAuthoringStatus::NoOp: return;
      case rt::TrackAuthoringStatus::Stale:
        setStatusMessage("Library changed while this edit was open. Reload the value and try again.");
        return;
      case rt::TrackAuthoringStatus::Missing: setStatusMessage("The edited track no longer exists."); return;
      case rt::TrackAuthoringStatus::Unavailable: setStatusMessage("Library editing is currently unavailable."); return;
      case rt::TrackAuthoringStatus::Applied: break;
    }

    uiDef->applyRowEditValue(*rowPtr, *editValueResult, field);
    clearStatusMessage();
  }

  void TrackViewPage::applyColumnViewStyles(Gtk::ColumnView& view)
  {
    view.set_reorderable(true);
    view.get_style_context()->add_provider(_viewHostPtr->cssProvider(), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
} // namespace ao::gtk
