// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "app/StyleManager.h"
#include "image/ImageCache.h"
#include "image/ImageWidget.h"
#include "layout/LayoutConstants.h"
#include "tag/TagPopover.h"
#include "track/TrackColumnFactoryBuilder.h"
#include "track/TrackColumnViewHost.h"
#include "track/TrackFieldUi.h"
#include "track/TrackListModel.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>
#include <ao/utility/Log.h>

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

    constexpr int kSectionCoverSize = 32;

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
  } // namespace

  TrackViewPage::TrackViewPage(ListId listId,
                               Glib::RefPtr<TrackListModel> modelPtr,
                               uimodel::track::TrackPresentationViewModel& presentationStore,
                               rt::AppRuntime& runtime,
                               ImageCache& imageCache,
                               rt::ViewId viewId)
    : Gtk::Box{Gtk::Orientation::VERTICAL}
    , _listId{listId}
    , _viewId{viewId}
    , _modelPtr{std::move(modelPtr)}

    , _presentationStore{presentationStore}
    , _runtime{runtime}
    , _imageCache{imageCache}
    , _groupModelPtr{Gtk::SortListModel::create(_modelPtr, Glib::RefPtr<Gtk::Sorter>{})}
    , _selectionModelPtr{Gtk::MultiSelection::create(_groupModelPtr)}
    , _viewHostPtr{std::make_unique<TrackColumnViewHost>(_modelPtr, _presentationStore, _selectionModelPtr, listId)}
  {
    _presentationStore.setActiveListId(_listId);
    _viewHostPtr->setupSelectionActivation();

    _themeRefreshConnection = StyleManager::instance().signalRefreshed().connect(
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
      { return buildColumnFactory(field, std::bind_front(&TrackViewPage::commitMetadataChange, this)); });

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
    _presentationStore.setActiveListId(_listId);
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

        auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        box->set_spacing(layout::kSpacingXLarge);
        box->add_css_class("ao-track-section-box");

        auto* const cover = Gtk::make_managed<ImageWidget>(_runtime.musicLibrary(), _imageCache);
        cover->setTargetSize(kSectionCoverSize);
        cover->set_size_request(kSectionCoverSize, kSectionCoverSize);
        cover->add_css_class("ao-track-section-cover");
        cover->set_valign(Gtk::Align::CENTER);
        box->append(*cover);

        auto* const label = Gtk::make_managed<Gtk::Label>("");
        label->set_halign(Gtk::Align::START);
        label->add_css_class("ao-track-section-header");
        label->set_xalign(0.0F);
        label->set_valign(Gtk::Align::CENTER);
        box->append(*label);

        headerPtr->set_child(*box);
      });

    _sectionHeaderFactoryPtr->signal_bind_obj().connect(
      [this](Glib::RefPtr<Glib::Object> const& object)
      {
        auto const headerPtr = std::dynamic_pointer_cast<Gtk::ListHeader>(object);
        auto* const box = headerPtr ? dynamic_cast<Gtk::Box*>(headerPtr->get_child()) : nullptr;

        if (headerPtr == nullptr || box == nullptr)
        {
          return;
        }

        auto* const cover = dynamic_cast<ImageWidget*>(box->get_first_child());
        auto* const label = dynamic_cast<Gtk::Label*>(box->get_last_child());

        if (cover == nullptr || label == nullptr)
        {
          return;
        }

        auto text = std::string{};
        auto coverArtId = kInvalidResourceId;

        if (auto* const proj = _modelPtr->projection(); proj != nullptr)
        {
          auto const start = headerPtr->get_start();

          if (auto const optGroupIndex = proj->groupIndexAt(start); optGroupIndex)
          {
            auto const snap = proj->groupAt(*optGroupIndex);
            text = snap.label;
            coverArtId = snap.imageId;
          }
        }

        if (!text.empty())
        {
          text += " ";
        }

        text += "(" + trackCountLabel(headerPtr->get_n_items()) + ")";

        label->set_text(text);

        if (coverArtId != kInvalidResourceId)
        {
          cover->loadImage(coverArtId);
          cover->set_visible(true);
        }
        else
        {
          cover->set_visible(false);
        }
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
    _presentationStore.setActivePresentationId(presentation.id);
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
    { return buildColumnFactory(field, std::bind_front(&TrackViewPage::commitMetadataChange, this)); };

    // 1. Detach UI from Model and Tree immediately.
    _viewHostPtr->columnView().set_model(Glib::RefPtr<Gtk::SelectionModel>{});
    _scrolledWindow.unset_child();
    _contextPopover.unparent();

    // 2. Create a new generation off-tree.
    auto& newView = _viewHostPtr->rebuild(_modelPtr, _presentationStore, _selectionModelPtr, factoryProvider, _listId);

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
    if (auto const oldValue = row->fieldText(field); newValue == oldValue)
    {
      return;
    }

    auto const* uiDef = trackFieldUiDefinition(field);

    if (uiDef == nullptr || uiDef->writePatch == nullptr || uiDef->parseInlineEdit == nullptr ||
        uiDef->readRowEditValue == nullptr || uiDef->applyRowEditValue == nullptr)
    {
      return;
    }

    auto const editValueResult = uiDef->parseInlineEdit(newValue);

    if (!editValueResult)
    {
      setStatusMessage(editValueResult.error().message);
      return;
    }

    auto const& editValue = *editValueResult;
    auto patch = rt::MetadataPatch{};
    auto const ctx = detail::TrackFieldEditContext{.patch = patch, .value = editValue};
    uiDef->writePatch(ctx);

    auto const oldEditValue = uiDef->readRowEditValue(*row, field);
    uiDef->applyRowEditValue(*row, editValue, field);

    auto const trackIds = std::array{row->trackId()};
    auto const result = _runtime.mutation().updateMetadata(trackIds, patch);

    if (!result)
    {
      APP_LOG_ERROR("Metadata update failed: {}", result.error().message);
      uiDef->applyRowEditValue(*row, oldEditValue, field);
      return;
    }

    clearStatusMessage();
  }

  void TrackViewPage::setupColumnViewStyles(Gtk::ColumnView& view)
  {
    view.set_reorderable(true);
    view.get_style_context()->add_provider(_viewHostPtr->cssProvider(), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
} // namespace ao::gtk
