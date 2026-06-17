// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

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
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
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
          _dragPtr->signal_drag_begin().connect([this](double posX, double posY) { onDragBegin(posX, posY); });
          _dragPtr->signal_drag_update().connect([this](double offsetX, double offsetY)
                                                 { onDragUpdate(offsetX, offsetY); });
          _dragPtr->signal_drag_end().connect([this](double offsetX, double offsetY) { onDragEnd(offsetX, offsetY); });
          add_controller(_dragPtr);

          auto const keyPtr = Gtk::EventControllerKey::create();
          keyPtr->signal_key_pressed().connect(
            [this](guint keyval, guint, Gdk::ModifierType) -> bool { return onKeyPressed(keyval); }, false);
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
                    std::int32_t posX,
                    std::int32_t posY,
                    std::int32_t width,
                    std::int32_t height,
                    std::int32_t zIndex)
      {
        child.set_parent(*this);
        _children.push_back({id, &child, posX, posY, width, height, zIndex, _insertCount++, posX, posY, width, height});
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
                                 [](ChildData const& childA, ChildData const& childB)
                                 {
                                   if (childA.zIndex != childB.zIndex)
                                   {
                                     return childA.zIndex < childB.zIndex;
                                   }

                                   return childA.insertOrder < childB.insertOrder;
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
            int const endPosition = child.posX + (child.reqWidth > 0 ? child.reqWidth : childNatural);
            natural = std::max(natural, endPosition);
            minimum = std::max(minimum, endPosition);
          }
          else
          {
            int const endPosition = child.posY + (child.reqHeight > 0 ? child.reqHeight : childNatural);
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
          alloc.set_x(child.posX);
          alloc.set_y(child.posY);

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

      void snapshot_vfunc(Glib::RefPtr<Gtk::Snapshot> const& snapshot) override
      {
        Gtk::Widget::snapshot_vfunc(snapshot);

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

          auto const rect = Gdk::Graphene::Rect{static_cast<float>(child.posX),
                                                static_cast<float>(child.posY),
                                                static_cast<float>(width),
                                                static_cast<float>(height)};
          auto const crPtr = snapshot->append_cairo(rect);

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
      enum class ResizeCorner : std::uint8_t
      {
        None = 0,
        TopLeft = 1,
        TopRight = 2,
        BottomLeft = 3,
        BottomRight = 4
      };

      static constexpr int kCornerHitRadius = 10;

      bool hitCorner(std::int32_t cornerX, std::int32_t cornerY, double mouseX, double mouseY) const
      {
        return std::abs(mouseX - static_cast<double>(cornerX)) <= kCornerHitRadius &&
               std::abs(mouseY - static_cast<double>(cornerY)) <= kCornerHitRadius;
      }

      ResizeCorner detectResizeCorner(std::int32_t width, std::int32_t height, double relX, double relY) const
      {
        if (hitCorner(0, 0, relX, relY))
        {
          return ResizeCorner::TopLeft;
        }

        if (hitCorner(width, 0, relX, relY))
        {
          return ResizeCorner::TopRight;
        }

        if (hitCorner(0, height, relX, relY))
        {
          return ResizeCorner::BottomLeft;
        }

        if (hitCorner(width, height, relX, relY))
        {
          return ResizeCorner::BottomRight;
        }

        return ResizeCorner::None;
      }

      bool onKeyPressed(guint keyval)
      {
        if (_selectedId.empty())
        {
          return false;
        }

        int const step = _snapToGrid ? _gridSize : 1;
        bool moved = false;

        for (auto& child : _children)
        {
          if (child.id != _selectedId)
          {
            continue;
          }

          if (keyval == GDK_KEY_Up)
          {
            child.posY -= step;
            moved = true;
          }
          else if (keyval == GDK_KEY_Down)
          {
            child.posY += step;
            moved = true;
          }
          else if (keyval == GDK_KEY_Left)
          {
            child.posX -= step;
            moved = true;
          }
          else if (keyval == GDK_KEY_Right)
          {
            child.posX += step;
            moved = true;
          }

          if (moved)
          {
            break;
          }
        }

        if (moved)
        {
          if (_onMoved && !_selectedId.empty())
          {
            for (auto const& child : _children)
            {
              if (child.id == _selectedId)
              {
                _onMoved(child.id, child.posX, child.posY);
                break;
              }
            }
          }
        }

        return moved;
      }

      void onDragBegin(double posX, double posY)
      {
        _dragChild = nullptr;
        _resizeCorner = ResizeCorner::None;

        for (auto& child : std::ranges::reverse_view{_children})
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

          if (posX >= child.posX && posX <= (child.posX + width) && posY >= child.posY && posY <= (child.posY + height))
          {
            _dragChild = &child;
            child.startX = child.posX;
            child.startY = child.posY;
            child.startReqWidth = child.reqWidth;
            child.startReqHeight = child.reqHeight;

            _selectedId = child.id;
            queue_draw();

            // Detect corner for resize
            _resizeCorner = detectResizeCorner(width, height, posX - child.posX, posY - child.posY);

            break;
          }
        }
      }

      void onDragUpdate(double offsetX, double offsetY)
      {
        if (_dragChild == nullptr)
        {
          return;
        }

        if (int const offX = static_cast<std::int32_t>(offsetX), offY = static_cast<std::int32_t>(offsetY);
            _resizeCorner != ResizeCorner::None)
        {
          auto const snap = [this](std::int32_t value)
          { return _snapToGrid ? ((value + _gridSize / 2) / _gridSize) * _gridSize : value; };

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

          switch (_resizeCorner)
          {
            case ResizeCorner::TopLeft:
              _dragChild->posX = snap(_dragChild->startX + offX);
              _dragChild->posY = snap(_dragChild->startY + offY);
              _dragChild->reqWidth = std::max(childNaturalWidth, _dragChild->startReqWidth - offX);
              _dragChild->reqHeight = std::max(childNaturalHeight, _dragChild->startReqHeight - offY);
              break;

            case ResizeCorner::TopRight:
              _dragChild->posY = snap(_dragChild->startY + offY);
              _dragChild->reqWidth = std::max(childNaturalWidth, _dragChild->startReqWidth + offX);
              _dragChild->reqHeight = std::max(childNaturalHeight, _dragChild->startReqHeight - offY);
              break;

            case ResizeCorner::BottomLeft:
              _dragChild->posX = snap(_dragChild->startX + offX);
              _dragChild->reqWidth = std::max(childNaturalWidth, _dragChild->startReqWidth - offX);
              _dragChild->reqHeight = std::max(childNaturalHeight, _dragChild->startReqHeight + offY);
              break;

            case ResizeCorner::BottomRight:
              _dragChild->reqWidth = std::max(childNaturalWidth, _dragChild->startReqWidth + offX);
              _dragChild->reqHeight = std::max(childNaturalHeight, _dragChild->startReqHeight + offY);
              break;

            case ResizeCorner::None:
            default: break;
          }
        }
        else
        {
          _dragChild->posX = _dragChild->startX + offX;
          _dragChild->posY = _dragChild->startY + offY;
        }

        queue_allocate();
        queue_draw();
      }

      void onDragEnd(double offsetX, double offsetY)
      {
        if (_dragChild == nullptr)
        {
          return;
        }

        if (int const offX = static_cast<std::int32_t>(offsetX), offY = static_cast<std::int32_t>(offsetY);
            _resizeCorner == ResizeCorner::None)
        {
          _dragChild->posX = _dragChild->startX + offX;
          _dragChild->posY = _dragChild->startY + offY;

          if (_snapToGrid)
          {
            _dragChild->posX = ((_dragChild->posX + _gridSize / 2) / _gridSize) * _gridSize;
            _dragChild->posY = ((_dragChild->posY + _gridSize / 2) / _gridSize) * _gridSize;
          }
        }
        else
        {
          if (_snapToGrid)
          {
            _dragChild->posX = ((_dragChild->posX + _gridSize / 2) / _gridSize) * _gridSize;
            _dragChild->posY = ((_dragChild->posY + _gridSize / 2) / _gridSize) * _gridSize;
            _dragChild->reqWidth = ((_dragChild->reqWidth + _gridSize / 2) / _gridSize) * _gridSize;
            _dragChild->reqHeight = ((_dragChild->reqHeight + _gridSize / 2) / _gridSize) * _gridSize;
          }
        }

        queue_allocate();
        queue_draw();

        if (_onMoved && !_dragChild->id.empty())
        {
          _onMoved(_dragChild->id, _dragChild->posX, _dragChild->posY);
        }

        _dragChild = nullptr;
        _resizeCorner = ResizeCorner::None;
      }

      struct ChildData final
      {
        std::string id;
        Gtk::Widget* widget;
        std::int32_t posX;
        std::int32_t posY;
        std::int32_t reqWidth;
        std::int32_t reqHeight;
        std::int32_t zIndex;
        std::int32_t insertOrder;
        std::int32_t startX;
        std::int32_t startY;
        std::int32_t startReqWidth;
        std::int32_t startReqHeight;
      };

      std::vector<ChildData> _children;
      std::string _selectedId;
      std::int32_t _insertCount = 0;

      bool _editMode = false;
      bool _snapToGrid = true;
      std::int32_t _gridSize = 8;
      ResizeCorner _resizeCorner = ResizeCorner::None;
      std::function<void(std::string const&, std::int32_t, std::int32_t)> _onMoved;
      Glib::RefPtr<Gtk::GestureDrag> _dragPtr;
      ChildData* _dragChild = nullptr;
    };

    class AbsoluteCanvasComponent final : public ILayoutComponent
    {
    public:
      AbsoluteCanvasComponent(LayoutContext& ctx, LayoutNode const& node)
        : _canvas{ctx.editMode,
                  ctx.onNodeMoved,
                  node.getProp<bool>("snapToGrid", true),
                  static_cast<std::int32_t>(node.getProp<std::int64_t>("gridSize", 8))}
      {
        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);

          int const posX = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("x", 0));
          int const posY = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("y", 0));
          int const width = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("width", -1));
          int const height = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("height", -1));
          int const zIndex = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("zIndex", 0));

          _canvas.addChild(childNode.id, childPtr->widget(), posX, posY, width, height, zIndex);
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
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
    };

    std::unique_ptr<ILayoutComponent> createAbsoluteCanvas(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<AbsoluteCanvasComponent>(ctx, node);
    }
  } // namespace

  void registerAbsoluteCanvasComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "absoluteCanvas",
                                .displayName = "Absolute Canvas",
                                .category = ComponentCategory::Container,
                                .props = {{.name = "snapToGrid",
                                           .kind = PropertyKind::Bool,
                                           .label = "Snap To Grid",
                                           .defaultValue = LayoutValue{true}},
                                          {.name = "gridSize",
                                           .kind = PropertyKind::Int,
                                           .label = "Grid Size",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(8)}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = std::nullopt},
                               createAbsoluteCanvas);
  }
} // namespace ao::gtk::layout
