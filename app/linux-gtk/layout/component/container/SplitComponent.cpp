// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AllocationObserver.h"
#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/StatefulComponentState.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr auto kSplitPositionSaveDelay = std::chrono::milliseconds{150};

    /**
     * @brief A split container component (Gtk::Paned).
     */
    class SplitComponent final : public ILayoutComponent
    {
    public:
      SplitComponent(LayoutContext& ctx, LayoutNode const& node)
        : _state{ctx, node, kSplitComponentType}
      {
        if (node.children.size() != 2)
        {
          _errorPtr = std::make_unique<Gtk::Label>();
          _errorPtr->set_markup(
            "<span foreground='red'><b>[Layout Error]</b> split requires exactly 2 children</span>");
          _errorPtr->add_css_class("ao-layout-error");
          return;
        }

        auto orientation = Gtk::Orientation::VERTICAL;

        if (node.getProp<std::string>("orientation", "") == "horizontal")
        {
          orientation = Gtk::Orientation::HORIZONTAL;
        }

        _paned.set_orientation(orientation);
        _allocationRoot.setChild(_paned);

        _startChildPtr = ctx.registry.create(ctx, node.children[0]);
        _paned.set_start_child(_startChildPtr->widget());

        _endChildPtr = ctx.registry.create(ctx, node.children[1]);
        _paned.set_end_child(_endChildPtr->widget());

        _paned.set_resize_start_child(node.getProp<bool>("resizeStart", true));
        _paned.set_shrink_start_child(node.getProp<bool>("shrinkStart", false));
        _paned.set_resize_end_child(node.getProp<bool>("resizeEnd", true));
        _paned.set_shrink_end_child(node.getProp<bool>("shrinkEnd", false));

        if (auto const& optState = _state.restored(); optState && optState->state.contains("positionPercent"))
        {
          _initialPercent = std::clamp(optState->state.at("positionPercent").asDouble(), 0.0, 1.0);
          _allocationRoot.setAllocatedCallback([this](std::int32_t width, std::int32_t height)
                                               { applyInitialPercent(width, height); });
        }
        else if (auto const it = node.props.find("position"); it != node.props.end())
        {
          _paned.set_position(static_cast<std::int32_t>(it->second.asInt()));
        }
        else if (auto const pit = node.props.find("initialPositionPercent"); pit != node.props.end())
        {
          _initialPercent = pit->second.asDouble();
          _allocationRoot.setAllocatedCallback([this](std::int32_t width, std::int32_t height)
                                               { applyInitialPercent(width, height); });
        }

        _positionChangedConn = _paned.property_position().signal_changed().connect([this] { schedulePositionSave(); });
      }

      ~SplitComponent() override
      {
        if (_saveDebounceConn.connected())
        {
          _saveDebounceConn.disconnect();
          savePositionPercent();
        }

        _positionChangedConn.disconnect();
        _allocationRoot.setAllocatedCallback({});
        _allocationRoot.clearChild();
      }

      SplitComponent(SplitComponent const&) = delete;
      SplitComponent& operator=(SplitComponent const&) = delete;
      SplitComponent(SplitComponent&&) = delete;
      SplitComponent& operator=(SplitComponent&&) = delete;

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr)
                                      : static_cast<Gtk::Widget&>(_allocationRoot);
      }

    private:
      void applyInitialPercent(std::int32_t width, std::int32_t height)
      {
        if (_initialPositionSet)
        {
          return;
        }

        int const total = (_paned.get_orientation() == Gtk::Orientation::HORIZONTAL) ? width : height;

        if (total <= 0)
        {
          return;
        }

        std::int32_t const newPos =
          std::max(0, static_cast<std::int32_t>(static_cast<double>(total) * _initialPercent));
        auto const suppress = ScopedPositionSaveSuppressor{*this};
        _paned.set_position(newPos);
        _initialPositionSet = true;
        _allocationRoot.setAllocatedCallback({});
      }

      void schedulePositionSave()
      {
        if (_suppressPositionSave || !_state.canWrite())
        {
          return;
        }

        _saveDebounceConn.disconnect();
        _saveDebounceConn = Glib::signal_timeout().connect(
          [this]
          {
            savePositionPercent();
            return false;
          },
          kSplitPositionSaveDelay.count());
      }

      void savePositionPercent()
      {
        if (!_state.canWrite())
        {
          return;
        }

        auto const total =
          (_paned.get_orientation() == Gtk::Orientation::HORIZONTAL) ? _paned.get_width() : _paned.get_height();

        if (total <= 0)
        {
          return;
        }

        auto const percent =
          std::clamp(static_cast<double>(_paned.get_position()) / static_cast<double>(total), 0.0, 1.0);
        _state.write({{"positionPercent", LayoutValue{percent}}});
      }

      struct ScopedPositionSaveSuppressor final
      {
        explicit ScopedPositionSaveSuppressor(SplitComponent& owner)
          : component{owner}, previous{owner._suppressPositionSave}
        {
          component._suppressPositionSave = true;
        }

        ~ScopedPositionSaveSuppressor() { component._suppressPositionSave = previous; }

        ScopedPositionSaveSuppressor(ScopedPositionSaveSuppressor const&) = delete;
        ScopedPositionSaveSuppressor& operator=(ScopedPositionSaveSuppressor const&) = delete;
        ScopedPositionSaveSuppressor(ScopedPositionSaveSuppressor&&) = delete;
        ScopedPositionSaveSuppressor& operator=(ScopedPositionSaveSuppressor&&) = delete;

        SplitComponent& component;
        bool previous;
      };

      StatefulComponentState _state;
      AllocationObserver _allocationRoot;
      Gtk::Paned _paned;
      sigc::connection _positionChangedConn;
      sigc::connection _saveDebounceConn;
      double _initialPercent = 0.0;
      bool _initialPositionSet = false;
      bool _suppressPositionSave = false;
      std::unique_ptr<Gtk::Label> _errorPtr;
      std::unique_ptr<ILayoutComponent> _startChildPtr;
      std::unique_ptr<ILayoutComponent> _endChildPtr;
    };

    std::unique_ptr<ILayoutComponent> createSplit(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SplitComponent>(ctx, node);
    }
  } // namespace

  void registerSplitComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = std::string{kSplitComponentType},
                                .displayName = "Split Pane",
                                .category = LayoutComponentCategory::Container,
                                .props = {{.name = "orientation",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"vertical"},
                                           .enumValues = {"vertical", "horizontal"}},
                                          {.name = "position",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Position",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
                                          {.name = "initialPositionPercent",
                                           .kind = LayoutPropertyKind::Double,
                                           .label = "Initial Position (%)",
                                           .defaultValue = LayoutValue{0.0}},
                                          {.name = "resizeStart",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Resize Start",
                                           .defaultValue = LayoutValue{true}},
                                          {.name = "shrinkStart",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Shrink Start",
                                           .defaultValue = LayoutValue{false}},
                                          {.name = "resizeEnd",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Resize End",
                                           .defaultValue = LayoutValue{true}},
                                          {.name = "shrinkEnd",
                                           .kind = LayoutPropertyKind::Bool,
                                           .label = "Shrink End",
                                           .defaultValue = LayoutValue{false}}},
                                .layoutProps = {},
                                .minChildren = 2,
                                .optMaxChildren = 2},
                               createSplit);
  }
} // namespace ao::gtk::layout
