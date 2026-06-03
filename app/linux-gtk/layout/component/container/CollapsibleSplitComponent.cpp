// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gdkmm/cursor.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/gesture.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/native.h>
#include <gtkmm/revealer.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr std::int32_t kDefaultCollapsibleSplitSize = 300;
    constexpr std::int32_t kMinCollapsibleSplitSize = 50;
    constexpr std::int32_t kResizeGripThickness = 4;
    constexpr double kCollapsibleSplitDragThreshold = 3.0;

    class FixedSplitPane final : public Gtk::Widget
    {
    public:
      FixedSplitPane() { set_overflow(Gtk::Overflow::HIDDEN); }

      ~FixedSplitPane() override
      {
        if (_child != nullptr)
        {
          _child->unparent();
        }
      }

      FixedSplitPane(FixedSplitPane const&) = delete;
      FixedSplitPane& operator=(FixedSplitPane const&) = delete;
      FixedSplitPane(FixedSplitPane&&) = delete;
      FixedSplitPane& operator=(FixedSplitPane&&) = delete;

      void setChild(Gtk::Widget& child)
      {
        clearChild();

        _child = &child;
        child.set_parent(*this);
      }

      void clearChild()
      {
        if (_child == nullptr)
        {
          return;
        }

        _child->unparent();
        _child = nullptr;
      }

      void setSplitOrientation(Gtk::Orientation orientation)
      {
        if (_orientation == orientation)
        {
          return;
        }

        _orientation = orientation;
        queue_resize();
      }

      void setPaneSize(std::int32_t size)
      {
        if (_paneSize == size)
        {
          return;
        }

        _paneSize = size;
        queue_resize();
      }

    protected:
      void measure_vfunc(Gtk::Orientation orientation,
                         int forSize,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        if (orientation == _orientation)
        {
          minimum = _paneSize;
          natural = _paneSize;
          minimumBaseline = -1;
          naturalBaseline = -1;
          return;
        }

        if (_child != nullptr)
        {
          _child->measure(orientation, forSize, minimum, natural, minimumBaseline, naturalBaseline);
          return;
        }

        minimum = 0;
        natural = 0;
        minimumBaseline = -1;
        naturalBaseline = -1;
      }

      void size_allocate_vfunc(int width, int height, int baseline) override
      {
        if (_child != nullptr)
        {
          _child->size_allocate({0, 0, width, height}, baseline);
        }
      }

    private:
      Gtk::Widget* _child = nullptr;
      Gtk::Orientation _orientation = Gtk::Orientation::HORIZONTAL;
      std::int32_t _paneSize = kDefaultCollapsibleSplitSize;
    };

    /**
     * @brief A resizable and collapsible split container.
     */
    class CollapsibleSplitComponent final : public ILayoutComponent
    {
    public:
      enum class Side : std::uint8_t
      {
        Start,
        End
      };

      CollapsibleSplitComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 2)
        {
          _errorPtr = std::make_unique<Gtk::Label>();
          _errorPtr->set_markup(
            "<span foreground='red'><b>[Layout Error]</b> collapsibleSplit requires exactly 2 children</span>");
          _errorPtr->add_css_class("ao-layout-error");
          return;
        }

        auto const orientationStr = node.getProp<std::string>("orientation", "horizontal");
        _orientation = (orientationStr == "vertical") ? Gtk::Orientation::VERTICAL : Gtk::Orientation::HORIZONTAL;
        _collapseSide = (node.getProp<std::string>("collapseSide", "end") == "start") ? Side::Start : Side::End;

        _container.set_orientation(_orientation);
        _paneSizer.setSplitOrientation(_orientation);

        // Build children
        _startChildPtr = ctx.registry.create(ctx, node.children[0]);
        _endChildPtr = ctx.registry.create(ctx, node.children[1]);

        bool const initiallyRevealed = node.getProp<bool>("revealed", true);
        _revealer.set_reveal_child(initiallyRevealed);

        _resizeGrip.add_css_class("ao-detail-resize-grip");
        _resizeGrip.set_can_target(true);
        _resizeGrip.set_cursor(resizeCursor());

        _toggleButton.add_css_class("ao-detail-handle");
        _toggleButton.set_valign(Gtk::Align::CENTER);
        _toggleButton.set_focus_on_click(false);
        _toggleButton.set_cursor(Gdk::Cursor::create("pointer"));

        if (_orientation == Gtk::Orientation::HORIZONTAL)
        {
          _resizeGrip.set_size_request(kResizeGripThickness, -1);
          _resizeGrip.set_vexpand(true);
        }
        else
        {
          _resizeGrip.set_size_request(-1, kResizeGripThickness);
          _resizeGrip.set_hexpand(true);
        }

        // Setup expansion
        if (_collapseSide == Side::End)
        {
          _startChildPtr->widget().set_hexpand(_orientation == Gtk::Orientation::HORIZONTAL);
          _startChildPtr->widget().set_vexpand(_orientation == Gtk::Orientation::VERTICAL);
          _collapsibleWidget = &_endChildPtr->widget();
        }
        else
        {
          _endChildPtr->widget().set_hexpand(_orientation == Gtk::Orientation::HORIZONTAL);
          _endChildPtr->widget().set_vexpand(_orientation == Gtk::Orientation::VERTICAL);
          _collapsibleWidget = &_startChildPtr->widget();
        }

        _collapsibleWidget->set_hexpand(true);
        _collapsibleWidget->set_vexpand(true);

        _paneSizer.setChild(*_collapsibleWidget);
        _paneSizer.set_hexpand(_orientation == Gtk::Orientation::VERTICAL);
        _paneSizer.set_vexpand(_orientation == Gtk::Orientation::HORIZONTAL);

        _revealer.set_child(_paneSizer);
        _revealer.set_hexpand(_orientation == Gtk::Orientation::VERTICAL);
        _revealer.set_vexpand(_orientation == Gtk::Orientation::HORIZONTAL);
        _revealer.set_transition_type(getTransitionType());

        // Initial size from "position" (we treat it as the fixed size of the collapsible panel)
        auto const requestedSize = node.getProp<std::int64_t>("position", kDefaultCollapsibleSplitSize);
        _currentSize = requestedSize > 0 ? static_cast<std::int32_t>(requestedSize) : kDefaultCollapsibleSplitSize;
        setSize(_currentSize);

        // Layout assembly
        _gutterBox.set_orientation(_orientation);
        _gutterBox.set_cursor(resizeCursor());

        if (_collapseSide == Side::Start)
        {
          _gutterBox.append(_toggleButton);
          _gutterBox.append(_resizeGrip);

          _container.append(_revealer);
          _container.append(_gutterBox);
          _container.append(_endChildPtr->widget());
        }
        else
        {
          _gutterBox.append(_resizeGrip);
          _gutterBox.append(_toggleButton);

          _container.append(_startChildPtr->widget());
          _container.append(_gutterBox);
          _container.append(_revealer);
        }

        // Drag logic
        _dragGesturePtr = Gtk::GestureDrag::create();
        _dragGesturePtr->set_button(1);
        _dragGesturePtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
        _container.add_controller(_dragGesturePtr);

        _dragGesturePtr->signal_drag_begin().connect(
          [this](double startX, double startY)
          {
            _dragAccepted = dragStartedInGutter(startX, startY);

            if (!_dragAccepted)
            {
              _dragGesturePtr->set_state(Gtk::EventSequenceState::DENIED);
              return;
            }

            _dragGesturePtr->set_state(Gtk::EventSequenceState::CLAIMED);
            _startSizeOnDrag =
              (_orientation == Gtk::Orientation::HORIZONTAL) ? _paneSizer.get_width() : _paneSizer.get_height();
            applyDragCursor();
          });

        _dragGesturePtr->signal_drag_update().connect(
          [this](double offsetX, double offsetY)
          {
            if (!_dragAccepted)
            {
              return;
            }

            double const delta = (_orientation == Gtk::Orientation::HORIZONTAL) ? offsetX : offsetY;

            if (double const absDelta = std::abs(delta); absDelta < kCollapsibleSplitDragThreshold)
            {
              return;
            }

            if (!_revealer.get_reveal_child())
            {
              return;
            }

            std::int32_t newSize = _startSizeOnDrag;

            if (_collapseSide == Side::End)
            {
              newSize -= static_cast<std::int32_t>(delta);
            }
            else
            {
              newSize += static_cast<std::int32_t>(delta);
            }

            _currentSize = std::max(kMinCollapsibleSplitSize, newSize);
            setSize(_currentSize);
          });

        _dragGesturePtr->signal_drag_end().connect(
          [this](double, double)
          {
            if (_dragAccepted)
            {
              restoreDragCursor();
            }

            _dragAccepted = false;
          });

        _toggleButton.signal_clicked().connect([this] { toggleRevealed(); });

        updateHandleIcon();
      }

      ~CollapsibleSplitComponent() override { _paneSizer.clearChild(); }

      CollapsibleSplitComponent(CollapsibleSplitComponent const&) = delete;
      CollapsibleSplitComponent& operator=(CollapsibleSplitComponent const&) = delete;
      CollapsibleSplitComponent(CollapsibleSplitComponent&&) = delete;
      CollapsibleSplitComponent& operator=(CollapsibleSplitComponent&&) = delete;

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr) : static_cast<Gtk::Widget&>(_container);
      }

    private:
      Gtk::RevealerTransitionType getTransitionType()
      {
        if (_orientation == Gtk::Orientation::HORIZONTAL)
        {
          return (_collapseSide == Side::Start) ? Gtk::RevealerTransitionType::SLIDE_RIGHT
                                                : Gtk::RevealerTransitionType::SLIDE_LEFT;
        }

        return (_collapseSide == Side::Start) ? Gtk::RevealerTransitionType::SLIDE_DOWN
                                              : Gtk::RevealerTransitionType::SLIDE_UP;
      }

      void setSize(std::int32_t size) { _paneSizer.setPaneSize(size); }

      Glib::RefPtr<Gdk::Cursor> resizeCursor() const
      {
        return Gdk::Cursor::create(_orientation == Gtk::Orientation::HORIZONTAL ? "col-resize" : "row-resize");
      }

      bool dragStartedInGutter(double containerX, double containerY)
      {
        double gutterX = 0.0;
        double gutterY = 0.0;

        if (!_container.translate_coordinates(_gutterBox, containerX, containerY, gutterX, gutterY))
        {
          return false;
        }

        if (!_gutterBox.contains(gutterX, gutterY))
        {
          return false;
        }

        double btnX = 0.0;

        if (double btnY = 0.0; _container.translate_coordinates(_toggleButton, containerX, containerY, btnX, btnY))
        {
          if (_toggleButton.contains(btnX, btnY))
          {
            return false;
          }
        }

        return true;
      }

      void applyDragCursor()
      {
        if (auto* const native = _container.get_native(); native != nullptr)
        {
          if (auto const surfacePtr = native->get_surface(); surfacePtr)
          {
            surfacePtr->set_cursor(resizeCursor());
          }
        }
      }

      void restoreDragCursor()
      {
        if (auto* const native = _container.get_native(); native != nullptr)
        {
          if (auto const surfacePtr = native->get_surface(); surfacePtr)
          {
            surfacePtr->set_cursor();
          }
        }
      }

      void toggleRevealed()
      {
        bool const revealed = !_revealer.get_reveal_child();
        _revealer.set_reveal_child(revealed);
        updateHandleIcon();
      }

      void updateHandleIcon()
      {
        if (bool const revealed = _revealer.get_reveal_child(); _orientation == Gtk::Orientation::HORIZONTAL)
        {
          if (_collapseSide == Side::Start)
          {
            _toggleButton.set_icon_name(revealed ? "pan-start-symbolic" : "pan-end-symbolic");
          }
          else
          {
            _toggleButton.set_icon_name(revealed ? "pan-end-symbolic" : "pan-start-symbolic");
          }
        }
        else
        {
          if (_collapseSide == Side::Start)
          {
            _toggleButton.set_icon_name(revealed ? "pan-up-symbolic" : "pan-down-symbolic");
          }
          else
          {
            _toggleButton.set_icon_name(revealed ? "pan-down-symbolic" : "pan-up-symbolic");
          }
        }
      }

      Gtk::Box _container;
      Gtk::Box _gutterBox;
      Gtk::Box _resizeGrip;
      Gtk::Button _toggleButton;
      Gtk::Revealer _revealer;
      FixedSplitPane _paneSizer;
      Gtk::Orientation _orientation;
      Side _collapseSide;
      Glib::RefPtr<Gtk::GestureDrag> _dragGesturePtr;

      std::int32_t _currentSize = kDefaultCollapsibleSplitSize;
      std::int32_t _startSizeOnDrag = kDefaultCollapsibleSplitSize;
      bool _dragAccepted = false;

      Gtk::Widget* _collapsibleWidget = nullptr;
      std::unique_ptr<Gtk::Label> _errorPtr;
      std::unique_ptr<ILayoutComponent> _startChildPtr;
      std::unique_ptr<ILayoutComponent> _endChildPtr;
    };

    std::unique_ptr<ILayoutComponent> createCollapsibleSplit(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<CollapsibleSplitComponent>(ctx, node);
    }
  } // namespace

  void registerCollapsibleSplitComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "collapsibleSplit",
       .displayName = "Collapsible Split",
       .category = "Containers",
       .container = true,
       .props = {{.name = "orientation",
                  .kind = PropertyKind::Enum,
                  .label = "Orientation",
                  .defaultValue = LayoutValue{"vertical"},
                  .enumValues = {"vertical", "horizontal"}},
                 {.name = "position",
                  .kind = PropertyKind::Int,
                  .label = "Position",
                  .defaultValue = LayoutValue{static_cast<std::int64_t>(kDefaultCollapsibleSplitSize)}},
                 {.name = "collapseSide",
                  .kind = PropertyKind::Enum,
                  .label = "Collapse Side",
                  .defaultValue = LayoutValue{"end"},
                  .enumValues = {"start", "end"}},
                 {.name = "revealed",
                  .kind = PropertyKind::Bool,
                  .label = "Initially Revealed",
                  .defaultValue = LayoutValue{true}}},
       .layoutProps = {},
       .minChildren = 2,
       .optMaxChildren = 2},
      createCollapsibleSplit);
  }
} // namespace ao::gtk::layout
