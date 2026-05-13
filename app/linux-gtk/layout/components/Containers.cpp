// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "Containers.h"

#include <gdk/gdk.h>
#include <gdkmm/graphene_rect.h>
#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/snapshot.h>
#include <gtkmm/stack.h>
#include <gtkmm/stackswitcher.h>

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
      auto const alignment = it->second.asString();

      if (alignment == "fill")
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
      auto const alignment = it->second.asString();

      if (alignment == "fill")
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

    if (auto const it = layout.find("margin"); it != layout.end())
    {
      int const margin = static_cast<int>(it->second.asInt());

      widget.set_margin_start(margin);
      widget.set_margin_end(margin);
      widget.set_margin_top(margin);
      widget.set_margin_bottom(margin);
    }

    int width = -1;
    int height = -1;
    bool sizeChanged = false;

    if (auto const it = layout.find("minWidth"); it != layout.end())
    {
      width = static_cast<int>(it->second.asInt());
      sizeChanged = true;
    }

    if (auto const it = layout.find("minHeight"); it != layout.end())
    {
      height = static_cast<int>(it->second.asInt());
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
      BoxComponent(LayoutDependencies& ctx, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::VERTICAL;

        if (node.getProp<std::string>("orientation", "") == "horizontal")
        {
          orientation = Gtk::Orientation::HORIZONTAL;
        }

        _box.set_orientation(orientation);
        _box.set_spacing(static_cast<int>(node.getProp<std::int64_t>("spacing", 0)));
        _box.set_homogeneous(node.getProp<bool>("homogeneous", false));

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
     * @brief A split container component (Gtk::Paned).
     */
    class SplitComponent final : public ILayoutComponent
    {
    public:
      SplitComponent(LayoutDependencies& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 2)
        {
          _error = std::make_unique<Gtk::Label>();
          _error->set_markup("<span foreground='red'><b>[Layout Error]</b> split requires exactly 2 children</span>");
          int const errorMargin = 10;
          _error->set_margin(errorMargin);
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
          _paned.set_position(static_cast<int>(it->second.asInt()));
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
      ScrollComponent(LayoutDependencies& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 1)
        {
          _error = std::make_unique<Gtk::Label>();
          _error->set_markup("<span foreground='red'><b>[Layout Error]</b> scroll requires exactly 1 child</span>");
          int const errorMargin = 10;
          _error->set_margin(errorMargin);
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

        _sw.set_min_content_width(static_cast<int>(node.getProp<std::int64_t>("minContentWidth", -1)));
        _sw.set_min_content_height(static_cast<int>(node.getProp<std::int64_t>("minContentHeight", -1)));

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
      SpacerComponent(LayoutDependencies& /*ctx*/, LayoutNode const& /*node*/) {}

      Gtk::Widget& widget() override { return _box; }

    private:
      Gtk::Box _box;
    };

    /**
     * @brief A stack of tabs (Gtk::Stack).
     */
    class TabsComponent final : public ILayoutComponent
    {
    public:
      TabsComponent(LayoutDependencies& ctx, LayoutNode const& node)
        : _box(Gtk::Orientation::VERTICAL)
      {
        if (node.children.empty())
        {
          _error = std::make_unique<Gtk::Label>();
          _error->set_markup("<span foreground='red'><b>[Layout Error]</b> tabs require at least 1 child</span>");
          int const errorMargin = 10;
          _error->set_margin(errorMargin);
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

    std::unique_ptr<ILayoutComponent> createBox(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<BoxComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSplit(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<SplitComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createScroll(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<ScrollComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createSpacer(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<SpacerComponent>(ctx, node);
    }

    std::unique_ptr<ILayoutComponent> createTabs(LayoutDependencies& ctx, LayoutNode const& node)
    {
      return std::make_unique<TabsComponent>(ctx, node);
    }

    class AbsoluteCanvasWidget final : public Gtk::Widget
    {
    public:
      AbsoluteCanvasWidget(bool editMode,
                           std::function<void(std::string const&, int, int)> onMoved,
                           bool snapToGrid = true,
                           int gridSize = 8)
        : _editMode(editMode), _snapToGrid(snapToGrid), _gridSize(gridSize), _onMoved(std::move(onMoved))
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

      void addChild(std::string const& id, Gtk::Widget& child, int posX, int posY, int width, int height, int zIndex)
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
                         int& minimum_baseline,
                         int& natural_baseline) const override
      {
        minimum = 0;
        natural = 0;
        minimum_baseline = -1;
        natural_baseline = -1;

        for (auto const& child : _children)
        {
          int childMin = 0;
          int childNatural = 0;
          int childMinBaseline = -1;
          int childNaturalBaseline = -1;
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

          int minWidth = 0;
          int naturalWidth = 0;
          int minBaseline = -1;
          int naturalBaseline = -1;
          child.widget->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, naturalWidth, minBaseline, naturalBaseline);
          int const width = child.reqWidth > 0 ? child.reqWidth : naturalWidth;

          int minHeight = 0;
          int naturalHeight = 0;
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

          int minWidth = 0;
          int naturalWidth = 0;
          int minBaseline = -1;
          int naturalBaseline = -1;

          child.widget->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, naturalWidth, minBaseline, naturalBaseline);
          int const width = child.reqWidth > 0 ? child.reqWidth : naturalWidth;

          int minHeight = 0;
          int naturalHeight = 0;
          child.widget->measure(
            Gtk::Orientation::VERTICAL, width, minHeight, naturalHeight, minBaseline, naturalBaseline);
          int const height = child.reqHeight > 0 ? child.reqHeight : naturalHeight;

          Gdk::Graphene::Rect const rect(static_cast<float>(child.posX),
                                         static_cast<float>(child.posY),
                                         static_cast<float>(width),
                                         static_cast<float>(height));
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

          auto const drawHandle = [&](int handleX, int handleY)
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

      bool hitCorner(int cornerX, int cornerY, double mouseX, double mouseY) const
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

        for (auto it = _children.rbegin(); it != _children.rend(); ++it)
        {
          int minWidth = 0;
          int naturalWidth = 0;
          int minBaseline = -1;
          int naturalBaseline = -1;
          it->widget->measure(Gtk::Orientation::HORIZONTAL, -1, minWidth, naturalWidth, minBaseline, naturalBaseline);
          int const width = it->reqWidth > 0 ? it->reqWidth : naturalWidth;

          int minHeight = 0;
          int naturalHeight = 0;
          it->widget->measure(
            Gtk::Orientation::VERTICAL, width, minHeight, naturalHeight, minBaseline, naturalBaseline);
          int const height = it->reqHeight > 0 ? it->reqHeight : naturalHeight;

          if (posX >= it->posX && posX <= (it->posX + width) && posY >= it->posY && posY <= (it->posY + height))
          {
            _dragChild = &(*it);
            it->startX = it->posX;
            it->startY = it->posY;
            it->startReqWidth = it->reqWidth;
            it->startReqHeight = it->reqHeight;

            _selectedId = it->id;
            queue_draw();

            // Detect corner for resize
            if (hitCorner(0, 0, posX - it->posX, posY - it->posY))
            {
              _resizeCorner = ResizeCorner::TopLeft;
            }
            else if (hitCorner(width, 0, posX - it->posX, posY - it->posY))
            {
              _resizeCorner = ResizeCorner::TopRight;
            }
            else if (hitCorner(0, height, posX - it->posX, posY - it->posY))
            {
              _resizeCorner = ResizeCorner::BottomLeft;
            }
            else if (hitCorner(width, height, posX - it->posX, posY - it->posY))
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

        int const offX = static_cast<int>(offsetX);
        int const offY = static_cast<int>(offsetY);

        if (_resizeCorner != ResizeCorner::None)
        {
          auto const snap = [this](int value)
          { return _snapToGrid ? ((value + _gridSize / 2) / _gridSize) * _gridSize : value; };

          int childNaturalWidth = 0;
          int childNaturalHeight = 0;
          int childMinBaseline = -1;
          int childNaturalBaseline = -1;

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

        int const offX = static_cast<int>(offsetX);
        int const offY = static_cast<int>(offsetY);

        if (_resizeCorner == ResizeCorner::None)
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

      struct ChildData
      {
        std::string id;
        Gtk::Widget* widget;
        int posX;
        int posY;
        int reqWidth;
        int reqHeight;
        int zIndex;
        int insertOrder;
        int startX;
        int startY;
        int startReqWidth;
        int startReqHeight;
      };

      std::vector<ChildData> _children;
      std::string _selectedId;
      int _insertCount = 0;

      bool _editMode = false;
      bool _snapToGrid = true;
      int _gridSize = 8;
      ResizeCorner _resizeCorner = ResizeCorner::None;
      std::function<void(std::string const&, int, int)> _onMoved;
      Glib::RefPtr<Gtk::GestureDrag> _drag;
      ChildData* _dragChild = nullptr;
    };

    class AbsoluteCanvasComponent final : public ILayoutComponent
    {
    public:
      AbsoluteCanvasComponent(LayoutDependencies& ctx, LayoutNode const& node)
        : _canvas(ctx.editMode,
                  ctx.onNodeMoved,
                  node.getProp<bool>("snapToGrid", true),
                  static_cast<int>(node.getProp<std::int64_t>("gridSize", 8)))
      {
        for (auto const& childNode : node.children)
        {
          auto child = ctx.registry.create(ctx, childNode);
          applyCommonProps(child->widget(), childNode);

          int const posX = static_cast<int>(childNode.getLayout<std::int64_t>("x", 0));
          int const posY = static_cast<int>(childNode.getLayout<std::int64_t>("y", 0));
          int const width = static_cast<int>(childNode.getLayout<std::int64_t>("width", -1));
          int const height = static_cast<int>(childNode.getLayout<std::int64_t>("height", -1));
          int const zIndex = static_cast<int>(childNode.getLayout<std::int64_t>("zIndex", 0));

          _canvas.addChild(childNode.id, child->widget(), posX, posY, width, height, zIndex);
          _children.push_back(std::move(child));
        }

        _canvas.sortChildren();
      }

      Gtk::Widget& widget() override { return _canvas; }

    private:
      AbsoluteCanvasWidget _canvas;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
    };

    std::unique_ptr<ILayoutComponent> createAbsoluteCanvas(LayoutDependencies& ctx, LayoutNode const& node)
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
                                .maxChildren = std::nullopt},
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
                                .maxChildren = std::nullopt},
                               createBox);

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
       .maxChildren = 2},
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
                                .maxChildren = 1},
                               createScroll);

    registry.registerComponent({.type = "spacer",
                                .displayName = "Spacer",
                                .category = "Containers",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               createSpacer);

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
       .maxChildren = std::nullopt},
      createTabs);
  }
} // namespace ao::gtk::layout
