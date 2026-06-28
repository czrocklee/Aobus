// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AllocationObserver.h"
#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/StatefulComponentState.h"
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr std::int32_t kMinCollapsibleSplitSize = 50;
    constexpr std::int32_t kCollapsibleSplitGutterReserve = 12;
    constexpr double kCollapsibleSplitDragThreshold = 3.0;
    constexpr std::int32_t kBootstrapSize = 50;

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
          _child->measure(
            orientation, constrainedChildForSize(forSize), minimum, natural, minimumBaseline, naturalBaseline);
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
          auto const childWidth = (_orientation == Gtk::Orientation::HORIZONTAL) ? std::max(width, _paneSize) : width;
          auto const childHeight = (_orientation == Gtk::Orientation::VERTICAL) ? std::max(height, _paneSize) : height;
          _child->size_allocate({0, 0, childWidth, childHeight}, baseline);
        }
      }

      std::int32_t constrainedChildForSize(std::int32_t forSize) const
      {
        if (forSize < 0)
        {
          return forSize;
        }

        return std::max(forSize, _paneSize);
      }

    private:
      Gtk::Widget* _child = nullptr;
      Gtk::Orientation _orientation = Gtk::Orientation::HORIZONTAL;
      std::int32_t _paneSize = 0;
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
        : _state{ctx, node, kCollapsibleSplitComponentType}
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
        _allocationRoot.setChild(_container);

        // Build children
        _startChildPtr = ctx.registry.create(ctx, node.children[0]);
        _endChildPtr = ctx.registry.create(ctx, node.children[1]);

        auto const& optState = _state.restored();
        bool initiallyRevealed = node.getProp<bool>("revealed", true);
        auto optRestoredSize = std::optional<std::int32_t>{};

        if (optState)
        {
          if (auto const it = optState->state.find("revealed"); it != optState->state.end())
          {
            initiallyRevealed = it->second.asBool(initiallyRevealed);
          }

          if (auto const it = optState->state.find("size"); it != optState->state.end())
          {
            auto const size = static_cast<std::int32_t>(it->second.asInt());

            if (size > 0)
            {
              optRestoredSize = size;
              _optPersistedSizeFallback = size;
            }
          }
        }

        _revealer.set_reveal_child(initiallyRevealed);

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

        applyInitialProps(node, initiallyRevealed, optRestoredSize);

        // Layout assembly
        assembleLayout();

        // Drag logic
        setupDragGesture();

        _toggleButton.signal_clicked().connect([this] { toggleRevealed(); });

        updateHandleIcon();
      }

      ~CollapsibleSplitComponent() override
      {
        if (_dragGesturePtr != nullptr)
        {
          _container.remove_controller(_dragGesturePtr);
        }

        _allocationRoot.setAllocatedCallback({});
        _allocationRoot.clearChild();
        _paneSizer.clearChild();

        // Unparent the direct child before its component is destroyed. The
        // collapsible child lives inside _paneSizer and is cleared above.
        if (_collapseSide == Side::End)
        {
          if (_startChildPtr != nullptr)
          {
            _container.remove(_startChildPtr->widget());
          }
        }
        else
        {
          if (_endChildPtr != nullptr)
          {
            _container.remove(_endChildPtr->widget());
          }
        }
      }

      CollapsibleSplitComponent(CollapsibleSplitComponent const&) = delete;
      CollapsibleSplitComponent& operator=(CollapsibleSplitComponent const&) = delete;
      CollapsibleSplitComponent(CollapsibleSplitComponent&&) = delete;
      CollapsibleSplitComponent& operator=(CollapsibleSplitComponent&&) = delete;

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr)
                                      : static_cast<Gtk::Widget&>(_allocationRoot);
      }

    private:
      void applyInitialProps(LayoutNode const& node,
                             bool initiallyRevealed,
                             std::optional<std::int32_t> optRestoredSize)
      {
        // Initial size from "position" (we treat it as the fixed size of the collapsible panel)
        auto const requestedSize = node.getProp<std::int64_t>("position", -1);
        auto const hasPosition = node.props.find("position") != node.props.end();
        auto const percentIt = node.props.find("initialPositionPercent");

        APP_LOG_INFO("CollapsibleSplit: construct id='{}' requestedSize={} hasPosition={} hasPercent={} revealed={}",
                     node.id,
                     requestedSize,
                     hasPosition,
                     percentIt != node.props.end(),
                     initiallyRevealed);

        if (optRestoredSize)
        {
          APP_LOG_INFO("CollapsibleSplit: using persisted state id='{}' size={}", node.id, *optRestoredSize);
          _currentSize = kBootstrapSize;
          setSize(_currentSize);
          scheduleRestoredSize(*optRestoredSize);
        }
        else if (requestedSize > 0)
        {
          APP_LOG_INFO("CollapsibleSplit: using fixed position id='{}' size={}", node.id, requestedSize);
          _currentSize = static_cast<std::int32_t>(requestedSize);
          _persistableSizeKnown = true;
          setSize(_currentSize);
        }
        else if (percentIt != node.props.end())
        {
          double const percent = percentIt->second.asDouble();
          APP_LOG_INFO("CollapsibleSplit: using percent id='{}' percent={}", node.id, percent);

          // Use bootstrap size to avoid measurement warnings during the first frame
          _currentSize = kBootstrapSize;
          setSize(_currentSize);
          scheduleInitialPercent(percent);
        }
        else
        {
          _currentSize = 0;
          setSize(_currentSize);
        }
      }

      void assembleLayout()
      {
        _gutterBox.set_orientation(_orientation);
        _gutterBox.add_css_class("ao-detail-resize-grip");
        _gutterBox.set_cursor(resizeCursor());

        _toggleButton.add_css_class("ao-detail-handle");
        _toggleButton.set_valign(Gtk::Align::CENTER);
        _toggleButton.set_focus_on_click(false);
        _toggleButton.set_cursor(Gdk::Cursor::create("pointer"));

        if (_collapseSide == Side::Start)
        {
          _gutterBox.append(_toggleButton);

          _container.append(_revealer);
          _container.append(_gutterBox);
          _container.append(_endChildPtr->widget());
        }
        else
        {
          _gutterBox.append(_toggleButton);

          _container.append(_startChildPtr->widget());
          _container.append(_gutterBox);
          _container.append(_revealer);
        }
      }

      void setupDragGesture()
      {
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

            _startSizeOnDrag =
              (_orientation == Gtk::Orientation::HORIZONTAL) ? _paneSizer.get_width() : _paneSizer.get_height();
          });

        _dragGesturePtr->signal_drag_update().connect(
          [this](double offsetX, double offsetY)
          {
            if (!_dragAccepted)
            {
              return;
            }

            double const delta = (_orientation == Gtk::Orientation::HORIZONTAL) ? offsetX : offsetY;

            if (!_dragActive)
            {
              if (double const absDelta = std::abs(delta); absDelta < kCollapsibleSplitDragThreshold)
              {
                return;
              }

              if (!_revealer.get_reveal_child())
              {
                return;
              }

              _dragGesturePtr->set_state(Gtk::EventSequenceState::CLAIMED);
              _dragActive = true;
              _manualSizeSelected = true;
              applyDragCursor();
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
            _persistableSizeKnown = true;
            setSize(_currentSize);
          });

        _dragGesturePtr->signal_drag_end().connect(
          [this](double, double)
          {
            if (_dragActive)
            {
              restoreDragCursor();
              saveRuntimeState();
            }

            _dragAccepted = false;
            _dragActive = false;
          });
      }

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

      static std::int32_t percentSize(std::int32_t total, double percent)
      {
        auto const scaled = static_cast<std::int32_t>(static_cast<double>(total) * percent);
        return std::max(kMinCollapsibleSplitSize, scaled);
      }

      static std::int32_t clampedRestoredSize(std::int32_t total, std::int32_t requested)
      {
        if (total <= 0)
        {
          return std::max(kMinCollapsibleSplitSize, requested);
        }

        auto const maxSize =
          std::max(kMinCollapsibleSplitSize, total - kMinCollapsibleSplitSize - kCollapsibleSplitGutterReserve);
        return std::clamp(requested, kMinCollapsibleSplitSize, maxSize);
      }

      void scheduleInitialPercent(double percent)
      {
        _initialPercent = percent;

        _allocationRoot.setAllocatedCallback([this](std::int32_t width, std::int32_t height)
                                             { applyInitialPercentAllocation(width, height); });
      }

      void applyInitialPercentAllocation(std::int32_t width, std::int32_t height)
      {
        if (_manualSizeSelected)
        {
          APP_LOG_INFO("CollapsibleSplit: percent allocation stop manual={}", _manualSizeSelected);
          _allocationRoot.setAllocatedCallback({});
          return;
        }

        std::int32_t const total = (_orientation == Gtk::Orientation::HORIZONTAL) ? width : height;

        if (total <= kBootstrapSize)
        {
          APP_LOG_INFO(
            "CollapsibleSplit: percent allocation waiting total={} width={} height={}", total, width, height);
          return;
        }

        std::int32_t const newSize = percentSize(total, _initialPercent);

        if (_initialPositionSet && _currentSize == newSize)
        {
          return;
        }

        _currentSize = newSize;
        APP_LOG_INFO("CollapsibleSplit: percent allocation {} percent={} total={} size={}",
                     _initialPositionSet ? "update" : "apply",
                     _initialPercent,
                     total,
                     _currentSize);
        setSize(_currentSize);
        _initialPositionSet = true;
        _persistableSizeKnown = true;
      }

      void scheduleRestoredSize(std::int32_t size)
      {
        _optPendingRestoredSize = size;

        _allocationRoot.setAllocatedCallback([this](std::int32_t width, std::int32_t height)
                                             { applyRestoredSizeAllocation(width, height); });
      }

      void applyRestoredSizeAllocation(std::int32_t width, std::int32_t height)
      {
        if (!_optPendingRestoredSize)
        {
          _allocationRoot.setAllocatedCallback({});
          return;
        }

        std::int32_t const total = (_orientation == Gtk::Orientation::HORIZONTAL) ? width : height;

        if (total <= kBootstrapSize)
        {
          APP_LOG_INFO(
            "CollapsibleSplit: restored allocation waiting total={} width={} height={}", total, width, height);
          return;
        }

        _currentSize = clampedRestoredSize(total, *_optPendingRestoredSize);
        _optPersistedSizeFallback = _currentSize;
        _persistableSizeKnown = true;
        APP_LOG_INFO("CollapsibleSplit: restored allocation apply requested={} total={} size={}",
                     *_optPendingRestoredSize,
                     total,
                     _currentSize);
        setSize(_currentSize);
        _optPendingRestoredSize.reset();
        _allocationRoot.setAllocatedCallback({});
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

        return _gutterBox.contains(gutterX, gutterY);
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
        saveRuntimeState();
      }

      void saveRuntimeState()
      {
        if (!_state.canWrite())
        {
          return;
        }

        auto state = std::map<std::string, LayoutValue, std::less<>>{};

        if (_persistableSizeKnown && _currentSize > 0)
        {
          state["size"] = LayoutValue{static_cast<std::int64_t>(_currentSize)};
          _optPersistedSizeFallback = _currentSize;
        }
        else if (_optPersistedSizeFallback)
        {
          state["size"] = LayoutValue{static_cast<std::int64_t>(*_optPersistedSizeFallback)};
        }

        state["revealed"] = LayoutValue{_revealer.get_reveal_child()};

        _state.write(std::move(state));
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

      StatefulComponentState _state;
      AllocationObserver _allocationRoot;
      Gtk::Box _container;
      Gtk::Box _gutterBox;
      Gtk::Button _toggleButton;
      Gtk::Revealer _revealer;
      FixedSplitPane _paneSizer;
      Gtk::Orientation _orientation;
      Side _collapseSide;
      Glib::RefPtr<Gtk::GestureDrag> _dragGesturePtr;

      std::int32_t _currentSize = 0;
      std::int32_t _startSizeOnDrag = 0;
      std::optional<std::int32_t> _optPendingRestoredSize;
      std::optional<std::int32_t> _optPersistedSizeFallback;
      double _initialPercent = 0.0;
      bool _initialPositionSet = false;
      bool _manualSizeSelected = false;
      bool _persistableSizeKnown = false;
      bool _dragAccepted = false;
      bool _dragActive = false;

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
    registry.registerComponent({.type = std::string{kCollapsibleSplitComponentType},
                                .displayName = "Collapsible Split",
                                .category = LayoutComponentCategory::Container,
                                .props = {{.name = "orientation",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"vertical"},
                                           .enumValues = {"vertical", "horizontal"}},
                                          {.name = "position",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Position",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}},
                                          {.name = "initialPositionPercent",
                                           .kind = LayoutPropertyKind::Double,
                                           .label = "Initial Position (%)",
                                           .defaultValue = LayoutValue{0.0}},
                                          {.name = "collapseSide",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Collapse Side",
                                           .defaultValue = LayoutValue{"end"},
                                           .enumValues = {"start", "end"}},
                                          {.name = "revealed",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Initially Revealed",
                                           .defaultValue = LayoutValue{true}}},
                                .layoutProps = {},
                                .minChildren = 2,
                                .optMaxChildren = 2},
                               createCollapsibleSplit);
  }
} // namespace ao::gtk::layout
