// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Containers.h"

#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gdkmm/graphene_rect.h>
#include <glib.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/stack.h>
#include <gtkmm/stackswitcher.h>
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
  /**
   * @brief Apply common layout properties to a GTK widget.
   */
  void applyCommonProps(Gtk::Widget& widget, LayoutNode const& node)
  {
    auto const& layout = node.layout;

    if (auto const it = layout.find("hexpand"); it != layout.end())
    {
      widget.set_hexpand(it->second.asBool());
    }

    if (auto const it = layout.find("vexpand"); it != layout.end())
    {
      widget.set_vexpand(it->second.asBool());
    }

    if (auto const it = layout.find("halign"); it != layout.end())
    {
      if (auto const alignment = it->second.asString(); alignment == "fill")
      {
        widget.set_halign(Gtk::Align::FILL);
      }
      else if (alignment == "start")
      {
        widget.set_halign(Gtk::Align::START);
      }
      else if (alignment == "end")
      {
        widget.set_halign(Gtk::Align::END);
      }
      else if (alignment == "center")
      {
        widget.set_halign(Gtk::Align::CENTER);
      }
    }

    if (auto const it = layout.find("valign"); it != layout.end())
    {
      if (auto const alignment = it->second.asString(); alignment == "fill")
      {
        widget.set_valign(Gtk::Align::FILL);
      }
      else if (alignment == "start")
      {
        widget.set_valign(Gtk::Align::START);
      }
      else if (alignment == "end")
      {
        widget.set_valign(Gtk::Align::END);
      }
      else if (alignment == "center")
      {
        widget.set_valign(Gtk::Align::CENTER);
      }
    }

    std::int32_t width = -1;
    std::int32_t height = -1;
    bool sizeChanged = false;

    if (auto const it = layout.find("minWidth"); it != layout.end())
    {
      width = static_cast<std::int32_t>(it->second.asInt());
      sizeChanged = true;
    }

    if (auto const it = layout.find("minHeight"); it != layout.end())
    {
      height = static_cast<std::int32_t>(it->second.asInt());
      sizeChanged = true;
    }

    if (sizeChanged)
    {
      widget.set_size_request(width, height);
    }

    if (auto const it = layout.find("visible"); it != layout.end())
    {
      widget.set_visible(it->second.asBool());
    }

    if (auto const it = layout.find("cssClasses"); it != layout.end())
    {
      if (auto const* classes = it->second.getIf<std::vector<std::string>>())
      {
        for (auto const& className : *classes)
        {
          widget.add_css_class(className);
        }
      }
      else if (auto const className = it->second.asString(); !className.empty())
      {
        widget.add_css_class(className);
      }
    }
  }

  namespace
  {
    /**
     * @brief A box container component.
     */
    class BoxComponent final : public ILayoutComponent
    {
    public:
      BoxComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::VERTICAL;

        if (node.getProp<std::string>("orientation", "") == "horizontal")
        {
          orientation = Gtk::Orientation::HORIZONTAL;
        }

        _box.set_orientation(orientation);
        _box.set_spacing(static_cast<std::int32_t>(node.getProp<std::int64_t>("spacing", 0)));
        _box.set_homogeneous(node.getProp<bool>("homogeneous", false));
        applyCommonProps(_box, node);

        for (auto const& childNode : node.children)
        {
          auto child = ctx.registry.create(ctx, childNode);
          applyCommonProps(child->widget(), childNode);
          _box.append(child->widget());
          _children.push_back(std::move(child));
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
    };

    /**
     * @brief A center box container component (Gtk::CenterBox).
     */
    class CenterBoxComponent final : public ILayoutComponent
    {
    public:
      CenterBoxComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::HORIZONTAL;

        if (node.getProp<std::string>("orientation", "") == "vertical")
        {
          orientation = Gtk::Orientation::VERTICAL;
        }

        _centerBox.set_orientation(orientation);
        applyCommonProps(_centerBox, node);

        for (auto const& childNode : node.children)
        {
          auto child = ctx.registry.create(ctx, childNode);
          applyCommonProps(child->widget(), childNode);

          auto const slot = childNode.getLayout<std::string>("slot", "");

          if (slot == "start")
          {
            _centerBox.set_start_widget(child->widget());
            _startChild = std::move(child);
          }
          else if (slot == "center")
          {
            _centerBox.set_center_widget(child->widget());
            _centerChild = std::move(child);
          }
          else if (slot == "end")
          {
            _centerBox.set_end_widget(child->widget());
            _endChild = std::move(child);
          }
          else
          {
            _overflowChildren.push_back(std::move(child));
          }
        }
      }

      Gtk::Widget& widget() override { return _centerBox; }

    private:
      Gtk::CenterBox _centerBox;
      std::unique_ptr<ILayoutComponent> _startChild;
      std::unique_ptr<ILayoutComponent> _centerChild;
      std::unique_ptr<ILayoutComponent> _endChild;
      std::vector<std::unique_ptr<ILayoutComponent>> _overflowChildren;
    };

    /**
     * @brief A split container component (Gtk::Paned).
     */
    class SplitComponent final : public ILayoutComponent
    {
    public:
      SplitComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 2)
        {
          _error = std::make_unique<Gtk::Label>();
          _error->set_markup("<span foreground='red'><b>[Layout Error]</b> split requires exactly 2 children</span>");
          _error->add_css_class("ao-layout-error");
          return;
        }

        auto orientation = Gtk::Orientation::VERTICAL;

        if (node.getProp<std::string>("orientation", "") == "horizontal")
        {
          orientation = Gtk::Orientation::HORIZONTAL;
        }

        _paned.set_orientation(orientation);

        _startChild = ctx.registry.create(ctx, node.children[0]);
        applyCommonProps(_startChild->widget(), node.children[0]);
        _paned.set_start_child(_startChild->widget());

        _endChild = ctx.registry.create(ctx, node.children[1]);
        applyCommonProps(_endChild->widget(), node.children[1]);
        _paned.set_end_child(_endChild->widget());

        _paned.set_resize_start_child(node.getProp<bool>("resizeStart", true));
        _paned.set_shrink_start_child(node.getProp<bool>("shrinkStart", false));
        _paned.set_resize_end_child(node.getProp<bool>("resizeEnd", true));
        _paned.set_shrink_end_child(node.getProp<bool>("shrinkEnd", false));

        if (auto const it = node.props.find("position"); it != node.props.end())
        {
          _paned.set_position(static_cast<std::int32_t>(it->second.asInt()));
        }
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(_paned);
      }

    private:
      Gtk::Paned _paned;
      std::unique_ptr<Gtk::Label> _error;
      std::unique_ptr<ILayoutComponent> _startChild;
      std::unique_ptr<ILayoutComponent> _endChild;
    };

    /**
     * @brief A scrollable container component (Gtk::ScrolledWindow).
     */
    class ScrollComponent final : public ILayoutComponent
    {
    public:
      ScrollComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 1)
        {
          _error = std::make_unique<Gtk::Label>();
          _error->set_markup("<span foreground='red'><b>[Layout Error]</b> scroll requires exactly 1 child</span>");
          _error->add_css_class("ao-layout-error");
          return;
        }

        _child = ctx.registry.create(ctx, node.children[0]);
        applyCommonProps(_child->widget(), node.children[0]);
        _sw.set_child(_child->widget());

        auto hpolicy = Gtk::PolicyType::AUTOMATIC;
        auto const hscrollPolicy = node.getProp<std::string>("hscrollPolicy", "");

        if (hscrollPolicy == "never")
        {
          hpolicy = Gtk::PolicyType::NEVER;
        }
        else if (hscrollPolicy == "always")
        {
          hpolicy = Gtk::PolicyType::ALWAYS;
        }

        auto vpolicy = Gtk::PolicyType::AUTOMATIC;
        auto const vscrollPolicy = node.getProp<std::string>("vscrollPolicy", "");

        if (vscrollPolicy == "never")
        {
          vpolicy = Gtk::PolicyType::NEVER;
        }
        else if (vscrollPolicy == "always")
        {
          vpolicy = Gtk::PolicyType::ALWAYS;
        }

        _sw.set_policy(hpolicy, vpolicy);

        _sw.set_min_content_width(static_cast<std::int32_t>(node.getProp<std::int64_t>("minContentWidth", -1)));
        _sw.set_min_content_height(static_cast<std::int32_t>(node.getProp<std::int64_t>("minContentHeight", -1)));

        _sw.set_propagate_natural_width(node.getProp<bool>("propagateNaturalWidth", false));
        _sw.set_propagate_natural_height(node.getProp<bool>("propagateNaturalHeight", false));
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(_sw);
      }

    private:
      Gtk::ScrolledWindow _sw;
      std::unique_ptr<Gtk::Label> _error;
      std::unique_ptr<ILayoutComponent> _child;
    };

    /**
     * @brief A simple spacer component.
     */
    class SpacerComponent final : public ILayoutComponent
    {
    public:
      SpacerComponent(LayoutContext& /*ctx*/, LayoutNode const& /*node*/) {}

      Gtk::Widget& widget() override { return _box; }

    private:
      Gtk::Box _box;
    };

    /**
     * @brief A visual separator component (Gtk::Separator).
     */
    class SeparatorComponent final : public ILayoutComponent
    {
    public:
      SeparatorComponent(LayoutContext& /*ctx*/, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::HORIZONTAL;

        if (node.getProp<std::string>("orientation", "") == "vertical")
        {
          orientation = Gtk::Orientation::VERTICAL;
        }

        _separator.set_orientation(orientation);
      }

      Gtk::Widget& widget() override { return _separator; }

    private:
      Gtk::Separator _separator;
    };

    /**
     * @brief A stack of tabs (Gtk::Stack).
     */
    class TabsComponent final : public ILayoutComponent
    {
    public:
      TabsComponent(LayoutContext& ctx, LayoutNode const& node)
        : _box{Gtk::Orientation::VERTICAL}
      {
        if (node.children.empty())
        {
          _error = std::make_unique<Gtk::Label>();
          _error->set_markup("<span foreground='red'><b>[Layout Error]</b> tabs require at least 1 child</span>");
          _error->add_css_class("ao-layout-error");
          return;
        }

        _switcher.set_stack(_stack);
        _box.append(_switcher);
        _box.append(_stack);
        _stack.set_vexpand(true);

        for (auto const& childNode : node.children)
        {
          auto child = ctx.registry.create(ctx, childNode);
          applyCommonProps(child->widget(), childNode);

          auto const title =
            childNode.getLayout<std::string>("title", !childNode.id.empty() ? childNode.id : "[Untitled]");
          auto const name = !childNode.id.empty() ? childNode.id : childNode.type;

          auto const page = _stack.add(child->widget(), name, title);

          if (auto const it = childNode.layout.find("icon"); it != childNode.layout.end())
          {
            page->set_icon_name(it->second.asString());
          }

          _children.push_back(std::move(child));
        }
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(_box);
      }

    private:
      Gtk::Box _box;
      Gtk::StackSwitcher _switcher;
      Gtk::Stack _stack;
      std::unique_ptr<Gtk::Label> _error;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
    };

    std::unique_ptr<ILayoutComponent> createBox(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<BoxComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createCenterBox(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<CenterBoxComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSplit(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SplitComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createScroll(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ScrollComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSpacer(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SpacerComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSeparator(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SeparatorComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createTabs(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TabsComponent>(ctx, node);
    }

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
          _drag = Gtk::GestureDrag::create();
          _drag->set_button(GDK_BUTTON_PRIMARY);
          _drag->signal_drag_begin().connect([this](double posX, double posY) { onDragBegin(posX, posY); });
          _drag->signal_drag_update().connect([this](double offsetX, double offsetY)
                                              { onDragUpdate(offsetX, offsetY); });
          _drag->signal_drag_end().connect([this](double offsetX, double offsetY) { onDragEnd(offsetX, offsetY); });
          add_controller(_drag);

          auto const key = Gtk::EventControllerKey::create();
          key->signal_key_pressed().connect(
            [this](guint keyval, guint, Gdk::ModifierType) -> bool { return onKeyPressed(keyval); }, false);
          add_controller(key);
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
          auto const cr = snapshot->append_cairo(rect);

          // Adwaita blue: #3584e4
          static constexpr double kSelectionR = 0.21;
          static constexpr double kSelectionG = 0.52;
          static constexpr double kSelectionB = 0.89;
          static constexpr double kSelectionAlpha = 0.6;
          static constexpr double kHandleAlpha = 0.8;
          static constexpr double kLineWidth = 2.0;

          cr->set_source_rgba(kSelectionR, kSelectionG, kSelectionB, kSelectionAlpha);
          cr->set_line_width(kLineWidth);

          static constexpr double kDashLength = 6.0;
          static constexpr double kDashGap = 3.0;
          cr->set_dash(std::vector<double>{kDashLength, kDashGap}, 0);
          cr->rectangle(0, 0, width, height);
          cr->stroke();

          // Resize handles at 4 corners
          double const handleSize = 8.0;

          auto const drawHandle = [&](std::int32_t handleX, std::int32_t handleY)
          {
            cr->set_dash(std::vector<double>{}, 0);

            double const halfHandleSize = handleSize / 2.0;
            cr->rectangle(static_cast<double>(handleX) - halfHandleSize,
                          static_cast<double>(handleY) - halfHandleSize,
                          handleSize,
                          handleSize);
            cr->set_source_rgba(kSelectionR, kSelectionG, kSelectionB, kHandleAlpha);
            cr->fill();
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
            if (hitCorner(0, 0, posX - child.posX, posY - child.posY))
            {
              _resizeCorner = ResizeCorner::TopLeft;
            }
            else if (hitCorner(width, 0, posX - child.posX, posY - child.posY))
            {
              _resizeCorner = ResizeCorner::TopRight;
            }
            else if (hitCorner(0, height, posX - child.posX, posY - child.posY))
            {
              _resizeCorner = ResizeCorner::BottomLeft;
            }
            else if (hitCorner(width, height, posX - child.posX, posY - child.posY))
            {
              _resizeCorner = ResizeCorner::BottomRight;
            }

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
      Glib::RefPtr<Gtk::GestureDrag> _drag;
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
          auto child = ctx.registry.create(ctx, childNode);
          applyCommonProps(child->widget(), childNode);

          int const posX = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("x", 0));
          int const posY = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("y", 0));
          int const width = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("width", -1));
          int const height = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("height", -1));
          int const zIndex = static_cast<std::int32_t>(childNode.getLayout<std::int64_t>("zIndex", 0));

          _canvas.addChild(childNode.id, child->widget(), posX, posY, width, height, zIndex);
          _children.push_back(std::move(child));
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

  void registerContainerComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "absoluteCanvas",
                                .displayName = "Absolute Canvas",
                                .category = "Containers",
                                .container = true,
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
    registry.registerComponent({.type = "box",
                                .displayName = "Box",
                                .category = "Containers",
                                .container = true,
                                .props = {{.name = "orientation",
                                           .kind = PropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"vertical"},
                                           .enumValues = {"vertical", "horizontal"}},
                                          {.name = "spacing",
                                           .kind = PropertyKind::Int,
                                           .label = "Spacing",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}},
                                          {.name = "homogeneous",
                                           .kind = PropertyKind::Bool,
                                           .label = "Homogeneous",
                                           .defaultValue = LayoutValue{false}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = std::nullopt},
                               createBox);

    registry.registerComponent({.type = "centerBox",
                                .displayName = "Center Box",
                                .category = "Containers",
                                .container = true,
                                .props = {{.name = "orientation",
                                           .kind = PropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"horizontal"},
                                           .enumValues = {"horizontal", "vertical"}}},
                                .layoutProps = {{.name = "slot",
                                                .kind = PropertyKind::Enum,
                                                .label = "Slot",
                                                .defaultValue = LayoutValue{""},
                                                .enumValues = {"", "start", "center", "end"}}},
                                .minChildren = 0,
                                .optMaxChildren = 3},
                               createCenterBox);

    registry.registerComponent(
      {.type = "split",
       .displayName = "Split Pane",
       .category = "Containers",
       .container = true,
       .props =
         {{.name = "orientation",
           .kind = PropertyKind::Enum,
           .label = "Orientation",
           .defaultValue = LayoutValue{"vertical"},
           .enumValues = {"vertical", "horizontal"}},
          {.name = "position",
           .kind = PropertyKind::Int,
           .label = "Position",
           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
          {.name = "resizeStart",
           .kind = PropertyKind::Bool,
           .label = "Resize Start",
           .defaultValue = LayoutValue{true}},
          {.name = "shrinkStart",
           .kind = PropertyKind::Bool,
           .label = "Shrink Start",
           .defaultValue = LayoutValue{false}},
          {.name = "resizeEnd", .kind = PropertyKind::Bool, .label = "Resize End", .defaultValue = LayoutValue{true}},
          {.name = "shrinkEnd", .kind = PropertyKind::Bool, .label = "Shrink End", .defaultValue = LayoutValue{false}}},
       .layoutProps = {},
       .minChildren = 2,
       .optMaxChildren = 2},
      createSplit);

    registry.registerComponent({.type = "scroll",
                                .displayName = "Scroll Window",
                                .category = "Containers",
                                .container = true,
                                .props = {{.name = "hscrollPolicy",
                                           .kind = PropertyKind::Enum,
                                           .label = "H. Scroll Policy",
                                           .defaultValue = LayoutValue{"automatic"},
                                           .enumValues = {"automatic", "always", "never"}},
                                          {.name = "vscrollPolicy",
                                           .kind = PropertyKind::Enum,
                                           .label = "V. Scroll Policy",
                                           .defaultValue = LayoutValue{"automatic"},
                                           .enumValues = {"automatic", "always", "never"}},
                                          {.name = "minContentWidth",
                                           .kind = PropertyKind::Int,
                                           .label = "Min Content Width",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
                                          {.name = "minContentHeight",
                                           .kind = PropertyKind::Int,
                                           .label = "Min Content Height",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
                                          {.name = "propagateNaturalWidth",
                                           .kind = PropertyKind::Bool,
                                           .label = "Propagate Nat. Width",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "propagateNaturalHeight",
                                           .kind = PropertyKind::Bool,
                                           .label = "Propagate Nat. Height",
                                           .defaultValue = LayoutValue{false}}},
                                .layoutProps = {},
                                .minChildren = 1,
                                .optMaxChildren = 1},
                               createScroll);

    registry.registerComponent({.type = "spacer",
                                .displayName = "Spacer",
                                .category = "Containers",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSpacer);

    registry.registerComponent({.type = "separator",
                                .displayName = "Separator",
                                .category = "Containers",
                                .container = false,
                                .props = {{.name = "orientation",
                                           .kind = PropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"horizontal"},
                                           .enumValues = {"horizontal", "vertical"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSeparator);

    registry.registerComponent(
      {.type = "tabs",
       .displayName = "Tabs",
       .category = "Containers",
       .container = true,
       .props = {},
       .layoutProps =
         {{.name = "title", .kind = PropertyKind::String, .label = "Tab Title", .defaultValue = LayoutValue{""}},
          {.name = "icon", .kind = PropertyKind::String, .label = "Tab Icon", .defaultValue = LayoutValue{""}}},
       .minChildren = 1,
       .optMaxChildren = std::nullopt},
      createTabs);
  }
} // namespace ao::gtk::layout
