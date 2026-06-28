// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AllocationObserver.h"
#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr std::int32_t kDefaultCompactMax = 820;
    constexpr std::int32_t kDefaultRegularMax = 1180;

    class ResponsiveClassComponent final : public ILayoutComponent
    {
    public:
      ResponsiveClassComponent(LayoutContext& ctx, LayoutNode const& node)
        : _axis{node.getProp<std::string>("axis", "width")}
        , _compactMax{static_cast<std::int32_t>(node.getProp<std::int64_t>("compactMax", kDefaultCompactMax))}
        , _regularMax{static_cast<std::int32_t>(node.getProp<std::int64_t>("regularMax", kDefaultRegularMax))}
        , _classPrefix{node.getProp<std::string>("classPrefix", "ao-width")}
      {
        if (node.children.size() != 1)
        {
          _errorPtr = std::make_unique<Gtk::Label>();
          _errorPtr->set_markup(
            "<span foreground='red'><b>[Layout Error]</b> responsiveClass requires exactly 1 child</span>");
          _errorPtr->add_css_class("ao-layout-error");
          return;
        }

        _regularMax = std::max(_regularMax, _compactMax);

        if (_classPrefix.empty())
        {
          _classPrefix = "ao-width";
        }

        _childPtr = ctx.registry.create(ctx, node.children[0]);
        _root.setChild(_childPtr->widget());
        _root.setAllocatedCallback([this](std::int32_t width, std::int32_t height) { updateClass(width, height); });
      }

      ~ResponsiveClassComponent() override
      {
        _root.setAllocatedCallback({});
        _root.clearChild();
      }

      ResponsiveClassComponent(ResponsiveClassComponent const&) = delete;
      ResponsiveClassComponent& operator=(ResponsiveClassComponent const&) = delete;
      ResponsiveClassComponent(ResponsiveClassComponent&&) = delete;
      ResponsiveClassComponent& operator=(ResponsiveClassComponent&&) = delete;

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr) : static_cast<Gtk::Widget&>(_root);
      }

    private:
      enum class Bucket : std::uint8_t
      {
        None,
        Compact,
        Regular,
        Wide
      };

      void updateClass(std::int32_t width, std::int32_t height)
      {
        std::int32_t const metric = (_axis == "height") ? height : width;
        auto const next = bucketFor(metric);

        if (next == _bucket)
        {
          return;
        }

        removeBucketClass(_bucket);
        addBucketClass(next);
        _bucket = next;
      }

      Bucket bucketFor(std::int32_t metric) const
      {
        if (metric <= _compactMax)
        {
          return Bucket::Compact;
        }

        if (metric <= _regularMax)
        {
          return Bucket::Regular;
        }

        return Bucket::Wide;
      }

      std::string classFor(Bucket bucket) const
      {
        switch (bucket)
        {
          case Bucket::Compact: return _classPrefix + "-compact";
          case Bucket::Regular: return _classPrefix + "-regular";
          case Bucket::Wide: return _classPrefix + "-wide";
          case Bucket::None:
          default: return {};
        }
      }

      void addBucketClass(Bucket bucket)
      {
        if (auto const className = classFor(bucket); !className.empty())
        {
          _root.add_css_class(className);
        }
      }

      void removeBucketClass(Bucket bucket)
      {
        if (auto const className = classFor(bucket); !className.empty())
        {
          _root.remove_css_class(className);
        }
      }

      AllocationObserver _root;
      std::string _axis;
      std::int32_t _compactMax;
      std::int32_t _regularMax;
      std::string _classPrefix;
      Bucket _bucket = Bucket::None;
      std::unique_ptr<Gtk::Label> _errorPtr;
      std::unique_ptr<ILayoutComponent> _childPtr;
    };

    std::unique_ptr<ILayoutComponent> createResponsiveClass(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ResponsiveClassComponent>(ctx, node);
    }
  } // namespace

  void registerResponsiveClassComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "responsiveClass",
                                .displayName = "Responsive Class",
                                .category = LayoutComponentCategory::Decorator,
                                .props = {{.name = "axis",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Axis",
                                           .defaultValue = LayoutValue{"width"},
                                           .enumValues = {"width", "height"}},
                                          {.name = "compactMax",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Compact Max",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(kDefaultCompactMax)}},
                                          {.name = "regularMax",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Regular Max",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(kDefaultRegularMax)}},
                                          {.name = "classPrefix",
                                           .kind = LayoutPropertyKind::String,
                                           .label = "Class Prefix",
                                           .defaultValue = LayoutValue{"ao-width"}}},
                                .layoutProps = {},
                                .minChildren = 1,
                                .optMaxChildren = 1},
                               createResponsiveClass);
  }
} // namespace ao::gtk::layout
