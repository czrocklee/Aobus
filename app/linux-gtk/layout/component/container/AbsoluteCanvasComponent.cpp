// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/AbsoluteCanvasGeometry.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gdkmm/graphene_rect.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    class AbsoluteCanvasWidget final : public Gtk::Widget
    {
    public:
      AbsoluteCanvasWidget(bool editMode,
                           std::function<void(std::string const&, std::int32_t, std::int32_t)> onMoved,
                           bool snapToGrid = true,
                           std::int32_t gridSize = 8)
        : _editMode{editMode}, _snapToGrid{snapToGrid}, _gridSize{gridSize}, _onMoved{std::move(onMoved)}
      {
        set_hexpand(true);
        set_vexpand(true);

        if (_editMode)
        {
          _dragPtr = Gtk::GestureDrag::create();
          _dragPtr->set_button(GDK_BUTTON_PRIMARY);
          _dragPtr->signal_drag_begin().connect([this](double xPosition, double yPosition)
                                                { handleDragBegin(xPosition, yPosition); });
          _dragPtr->signal_drag_update().connect([this](double offsetX, double offsetY)
                                                 { handleDragUpdate(offsetX, offsetY); });
          _dragPtr->signal_drag_end().connect([this](double offsetX, double offsetY)
                                              { handleDragEnd(offsetX, offsetY); });
          add_controller(_dragPtr);

          auto const keyPtr = Gtk::EventControllerKey::create();
          keyPtr->signal_key_pressed().connect(
            [this](guint keyval, guint, Gdk::ModifierType) -> bool { return handleKeyPressed(keyval); }, false);
          add_controller(keyPtr);
        }
      }

      ~AbsoluteCanvasWidget() override = default;

      AbsoluteCanvasWidget(AbsoluteCanvasWidget const&) = delete;
      AbsoluteCanvasWidget& operator=(AbsoluteCanvasWidget const&) = delete;
      AbsoluteCanvasWidget(AbsoluteCanvasWidget&&) = delete;
      AbsoluteCanvasWidget& operator=(AbsoluteCanvasWidget&&) = delete;

      void unparentAll()
      {
        for (auto& child : _children)
        {
          child.widget->unparent();
        }

        _children.clear();
      }

      void addChild(std::string const& id,
                    Gtk::Widget& child,
                    std::int32_t xPosition,
                    std::int32_t yPosition,
                    std::int32_t width,
                    std::int32_t height,
                    std::int32_t zIndex)
      {
        child.set_parent(*this);
        _children.push_back({.id = id,
                             .widget = &child,
                             .x = xPosition,
                             .y = yPosition,
                             .reqWidth = width,
                             .reqHeight = height,
                             .zIndex = zIndex,
                             .insertOrder = _insertCount++,
                             .startX = xPosition,
                             .startY = yPosition,
                             .startReqWidth = width,
                             .startReqHeight = height});
      }

      void setSelectedChild(std::string const& id)
      {
        _selectedId = id;
        queue_draw();
      }

      void raiseChild(std::string const& id)
      {
        for (auto& child : _children)
        {
          if (child.id == id)
          {
            child.zIndex++;
            break;
          }
        }

        sortChildren();
      }

      void lowerChild(std::string const& id)
      {
        for (auto& child : _children)
        {
          if (child.id == id)
          {
            child.zIndex = std::max(0, child.zIndex - 1);
            break;
          }
        }

        sortChildren();
      }

      void sortChildren()
      {
        std::ranges::stable_sort(_children,
                                 [](ChildLayoutState const& childA, ChildLayoutState const& childB)
                                 {
                                   return uimodel::ordersAbsoluteCanvasBefore(
                                     childA.zIndex, childA.insertOrder, childB.zIndex, childB.insertOrder);
                                 });
      }

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::CONSTANT_SIZE; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int /*for_size*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimum = 0;
        natural = 0;
        minimumBaseline = -1;
        naturalBaseline = -1;

        for (auto const& child : _children)
        {
          std::int32_t childMin = 0;
          std::int32_t childNatural = 0;
          std::int32_t childMinBaseline = -1;
          std::int32_t childNaturalBaseline = -1;
          child.widget->measure(orientation, -1, childMin, childNatural, childMinBaseline, childNaturalBaseline);

          if (orientation == Gtk::Orientation::HORIZONTAL)
          {
            int const endPosition = child.x + (child.reqWidth > 0 ? child.reqWidth : childNatural);
            natural = std::max(natural, endPosition);
            minimum = std::max(minimum, endPosition);
          }
          else
          {
            int const endPosition = child.y + (child.reqHeight > 0 ? child.reqHeight : childNatural);
            natural = std::max(natural, endPosition);
            minimum = std::max(minimum, endPosition);
          }
        }
      }

      void size_allocate_vfunc(int /*width*/, int /*height*/, int /*baseline*/) override
      {
        for (auto const& child : _children)
        {
          auto alloc = Gtk::Allocation{};
          alloc.set_x(child.x);
          alloc.set_y(child.y);

          std::int32_t minWidth = 0;
          std::int32_t naturalWidth = 0;
          std::int32_t minBaseline = -1;
          std::int32_t naturalBaseline = -1;
          child.widget->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, naturalWidth, minBaseline, naturalBaseline);
          int const width = child.reqWidth > 0 ? child.reqWidth : naturalWidth;

          std::int32_t minHeight = 0;
          std::int32_t naturalHeight = 0;
          child.widget->measure(
            Gtk::Orientation::VERTICAL, width, minHeight, naturalHeight, minBaseline, naturalBaseline);
          int const height = child.reqHeight > 0 ? child.reqHeight : naturalHeight;

          alloc.set_width(width);
          alloc.set_height(height);

          child.widget->size_allocate(alloc, -1);
        }
      }

      void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshotPtr) override
      {
        Gtk::Widget::snapshot_vfunc(snapshotPtr);

        if (!_editMode || _selectedId.empty())
        {
          return;
        }

        for (auto const& child : _children)
        {
          if (child.id != _selectedId)
          {
            continue;
          }

          std::int32_t minWidth = 0;
          std::int32_t naturalWidth = 0;
          std::int32_t minBaseline = -1;
          std::int32_t naturalBaseline = -1;

          child.widget->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, naturalWidth, minBaseline, naturalBaseline);
          int const width = child.reqWidth > 0 ? child.reqWidth : naturalWidth;

          std::int32_t minHeight = 0;
          std::int32_t naturalHeight = 0;
          child.widget->measure(
            Gtk::Orientation::VERTICAL, width, minHeight, naturalHeight, minBaseline, naturalBaseline);
          int const height = child.reqHeight > 0 ? child.reqHeight : naturalHeight;

          auto const rect = Gdk::Graphene::Rect{static_cast<float>(child.x),
                                                static_cast<float>(child.y),
                                                static_cast<float>(width),
                                                static_cast<float>(height)};
          auto const crPtr = snapshotPtr->append_cairo(rect);

          // Adwaita blue: #3584e4
          static constexpr double kSelectionR = 0.21;
          static constexpr double kSelectionG = 0.52;
          static constexpr double kSelectionB = 0.89;
          static constexpr double kSelectionAlpha = 0.6;
          static constexpr double kHandleAlpha = 0.8;
          static constexpr double kLineWidth = 2.0;

          crPtr->set_source_rgba(kSelectionR, kSelectionG, kSelectionB, kSelectionAlpha);
          crPtr->set_line_width(kLineWidth);

          static constexpr double kDashLength = 6.0;
          static constexpr double kDashGap = 3.0;
          crPtr->set_dash(std::vector{kDashLength, kDashGap}, 0);
          crPtr->rectangle(0, 0, width, height);
          crPtr->stroke();

          // Resize handles at 4 corners
          double const handleSize = 8.0;

          auto const drawHandle = [&](std::int32_t handleX, std::int32_t handleY)
          {
            crPtr->set_dash(std::vector<double>{}, 0);

            double const halfHandleSize = handleSize / 2.0;
            crPtr->rectangle(static_cast<double>(handleX) - halfHandleSize,
                             static_cast<double>(handleY) - halfHandleSize,
                             handleSize,
                             handleSize);
            crPtr->set_source_rgba(kSelectionR, kSelectionG, kSelectionB, kHandleAlpha);
            crPtr->fill();
          };

          drawHandle(0, 0);
          drawHandle(width, 0);
          drawHandle(0, height);
          drawHandle(width, height);
          break;
        }
      }

    private:
      bool handleKeyPressed(guint keyval)
      {
        if (_selectedId.empty())
        {
          return false;
        }

        auto optDirection = std::optional<uimodel::AbsoluteCanvasNudgeDirection>{};

        if (keyval == GDK_KEY_Up)
        {
          optDirection = uimodel::AbsoluteCanvasNudgeDirection::Up;
        }
        else if (keyval == GDK_KEY_Down)
        {
          optDirection = uimodel::AbsoluteCanvasNudgeDirection::Down;
        }
        else if (keyval == GDK_KEY_Left)
        {
          optDirection = uimodel::AbsoluteCanvasNudgeDirection::Left;
        }
        else if (keyval == GDK_KEY_Right)
        {
          optDirection = uimodel::AbsoluteCanvasNudgeDirection::Right;
        }

        if (!optDirection)
        {
          return false;
        }

        for (auto& child : _children)
        {
          if (child.id != _selectedId)
          {
            continue;
          }

          auto const rect = uimodel::nudgeAbsoluteCanvasRect(
            {.x = child.x, .y = child.y, .width = child.reqWidth, .height = child.reqHeight},
            *optDirection,
            _snapToGrid,
            _gridSize);
          child.x = rect.x;
          child.y = rect.y;

          if (_onMoved && !_selectedId.empty())
          {
            _onMoved(child.id, child.x, child.y);
          }

          return true;
        }

        return false;
      }

      void handleDragBegin(double xPosition, double yPosition)
      {
        _dragChild = nullptr;
        _resizeCorner = uimodel::AbsoluteCanvasResizeCorner::None;

        auto hitItems = std::vector<uimodel::AbsoluteCanvasItem>{};
        hitItems.reserve(_children.size());

        for (auto const& child : _children)
        {
          std::int32_t minWidth = 0;
          std::int32_t naturalWidth = 0;
          std::int32_t minBaseline = -1;
          std::int32_t naturalBaseline = -1;
          child.widget->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, naturalWidth, minBaseline, naturalBaseline);
          int const width = child.reqWidth > 0 ? child.reqWidth : naturalWidth;

          std::int32_t minHeight = 0;
          std::int32_t naturalHeight = 0;
          child.widget->measure(
            Gtk::Orientation::VERTICAL, width, minHeight, naturalHeight, minBaseline, naturalBaseline);
          int const height = child.reqHeight > 0 ? child.reqHeight : naturalHeight;

          hitItems.push_back({.id = child.id,
                              .rect = {.x = child.x, .y = child.y, .width = width, .height = height},
                              .zIndex = child.zIndex,
                              .insertOrder = child.insertOrder});
        }

        auto const optHit = uimodel::hitTestAbsoluteCanvas(
          hitItems, static_cast<std::int32_t>(xPosition), static_cast<std::int32_t>(yPosition));

        if (!optHit)
        {
          return;
        }

        auto& child = _children[*optHit];
        auto const& rect = hitItems[*optHit].rect;

        _dragChild = &child;
        child.startX = child.x;
        child.startY = child.y;
        child.startReqWidth = rect.width;
        child.startReqHeight = rect.height;

        _selectedId = child.id;
        queue_draw();

        _resizeCorner =
          uimodel::detectAbsoluteCanvasResizeCorner(rect.width, rect.height, xPosition - child.x, yPosition - child.y);
      }

      void handleDragUpdate(double offsetX, double offsetY)
      {
        if (_dragChild == nullptr)
        {
          return;
        }

        if (int const offX = static_cast<std::int32_t>(offsetX), offY = static_cast<std::int32_t>(offsetY);
            _resizeCorner != uimodel::AbsoluteCanvasResizeCorner::None)
        {
          std::int32_t childNaturalWidth = 0;
          std::int32_t childNaturalHeight = 0;
          std::int32_t childMinBaseline = -1;
          std::int32_t childNaturalBaseline = -1;

          _dragChild->widget->measure(Gtk::Orientation::HORIZONTAL,
                                      -1,
                                      childNaturalWidth,
                                      childNaturalWidth,
                                      childMinBaseline,
                                      childNaturalBaseline);
          _dragChild->widget->measure(Gtk::Orientation::VERTICAL,
                                      childNaturalWidth,
                                      childNaturalHeight,
                                      childNaturalHeight,
                                      childMinBaseline,
                                      childNaturalBaseline);

          auto const rect = uimodel::updateAbsoluteCanvasResizeDrag({.x = _dragChild->startX,
                                                                     .y = _dragChild->startY,
                                                                     .width = _dragChild->startReqWidth,
                                                                     .height = _dragChild->startReqHeight},
                                                                    _resizeCorner,
                                                                    offX,
                                                                    offY,
                                                                    childNaturalWidth,
                                                                    childNaturalHeight,
                                                                    _snapToGrid,
                                                                    _gridSize);
          _dragChild->x = rect.x;
          _dragChild->y = rect.y;
          _dragChild->reqWidth = rect.width;
          _dragChild->reqHeight = rect.height;
        }
        else
        {
          auto const rect = uimodel::updateAbsoluteCanvasMoveDrag({.x = _dragChild->startX,
                                                                   .y = _dragChild->startY,
                                                                   .width = _dragChild->startReqWidth,
                                                                   .height = _dragChild->startReqHeight},
                                                                  offX,
                                                                  offY);
          _dragChild->x = rect.x;
          _dragChild->y = rect.y;
        }

        queue_allocate();
        queue_draw();
      }

      void handleDragEnd(double offsetX, double offsetY)
      {
        if (_dragChild == nullptr)
        {
          return;
        }

        if (int const offX = static_cast<std::int32_t>(offsetX), offY = static_cast<std::int32_t>(offsetY);
            _resizeCorner == uimodel::AbsoluteCanvasResizeCorner::None)
        {
          auto const rect = uimodel::commitAbsoluteCanvasMoveDrag({.x = _dragChild->startX,
                                                                   .y = _dragChild->startY,
                                                                   .width = _dragChild->startReqWidth,
                                                                   .height = _dragChild->startReqHeight},
                                                                  offX,
                                                                  offY,
                                                                  _snapToGrid,
                                                                  _gridSize);
          _dragChild->x = rect.x;
          _dragChild->y = rect.y;
        }
        else
        {
          auto const rect = uimodel::commitAbsoluteCanvasResizeDrag(
            {.x = _dragChild->x, .y = _dragChild->y, .width = _dragChild->reqWidth, .height = _dragChild->reqHeight},
            _snapToGrid,
            _gridSize);
          _dragChild->x = rect.x;
          _dragChild->y = rect.y;
          _dragChild->reqWidth = rect.width;
          _dragChild->reqHeight = rect.height;
        }

        queue_allocate();
        queue_draw();

        if (_onMoved && !_dragChild->id.empty())
        {
          _onMoved(_dragChild->id, _dragChild->x, _dragChild->y);
        }

        _dragChild = nullptr;
        _resizeCorner = uimodel::AbsoluteCanvasResizeCorner::None;
      }

      struct ChildLayoutState final
      {
        std::string id;
        Gtk::Widget* widget;
        std::int32_t x;
        std::int32_t y;
        std::int32_t reqWidth;
        std::int32_t reqHeight;
        std::int32_t zIndex;
        std::int32_t insertOrder;
        std::int32_t startX;
        std::int32_t startY;
        std::int32_t startReqWidth;
        std::int32_t startReqHeight;
      };

      std::vector<ChildLayoutState> _children;
      std::string _selectedId;
      std::int32_t _insertCount = 0;

      bool _editMode = false;
      bool _snapToGrid = true;
      std::int32_t _gridSize = 8;
      uimodel::AbsoluteCanvasResizeCorner _resizeCorner = uimodel::AbsoluteCanvasResizeCorner::None;
      std::function<void(std::string const&, std::int32_t, std::int32_t)> _onMoved;
      Glib::RefPtr<Gtk::GestureDrag> _dragPtr;
      ChildLayoutState* _dragChild = nullptr;
    };

    class AbsoluteCanvasComponent final : public LayoutComponent
    {
    public:
      AbsoluteCanvasComponent(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
        : _canvas{ctx.buildState.isEditMode(),
                  ctx.buildState.onNodeMoved(),
                  node.propertyOr<bool>("snapToGrid", true),
                  static_cast<std::int32_t>(node.propertyOr<std::int64_t>("gridSize", 8))}
      {
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);

          int const xPosition = static_cast<std::int32_t>(childNode.layoutOr<std::int64_t>("x", 0));
          int const yPosition = static_cast<std::int32_t>(childNode.layoutOr<std::int64_t>("y", 0));
          int const width = static_cast<std::int32_t>(childNode.layoutOr<std::int64_t>("width", -1));
          int const height = static_cast<std::int32_t>(childNode.layoutOr<std::int64_t>("height", -1));
          int const zIndex = static_cast<std::int32_t>(childNode.layoutOr<std::int64_t>("zIndex", 0));

          _canvas.addChild(childNode.id, childPtr->widget(), xPosition, yPosition, width, height, zIndex);
          _children.push_back(std::move(childPtr));
        }

        _canvas.sortChildren();
      }

      ~AbsoluteCanvasComponent() override { _canvas.unparentAll(); }

      AbsoluteCanvasComponent(AbsoluteCanvasComponent const&) = delete;
      AbsoluteCanvasComponent& operator=(AbsoluteCanvasComponent const&) = delete;
      AbsoluteCanvasComponent(AbsoluteCanvasComponent&&) = delete;
      AbsoluteCanvasComponent& operator=(AbsoluteCanvasComponent&&) = delete;

      Gtk::Widget& widget() override { return _canvas; }

    private:
      AbsoluteCanvasWidget _canvas;
      std::vector<std::unique_ptr<LayoutComponent>> _children;
    };

    std::unique_ptr<LayoutComponent> createAbsoluteCanvas(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
    {
      return std::make_unique<AbsoluteCanvasComponent>(ctx, node);
    }
  } // namespace

  void registerAbsoluteCanvasComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "absoluteCanvas",
                                .displayName = "Absolute Canvas",
                                .category = uimodel::LayoutComponentCategory::Container,
                                .props = {{.name = "snapToGrid",
                                           .kind = uimodel::LayoutPropertyKind::Bool,
                                           .label = "Snap To Grid",
                                           .defaultValue = uimodel::LayoutValue{true}},
                                          {.name = "gridSize",
                                           .kind = uimodel::LayoutPropertyKind::Int,
                                           .label = "Grid Size",
                                           .defaultValue = uimodel::LayoutValue{static_cast<std::int64_t>(8)}}},
                                .minChildren = 0,
                                .optMaxChildren = std::nullopt},
                               createAbsoluteCanvas);
  }
} // namespace ao::gtk::layout
