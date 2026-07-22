// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "status/ActivityStatusWidget.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

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
    ActivityStatusWidgetVariant parseVariant(std::string const& value)
    {
      if (value == "classicInline")
      {
        return ActivityStatusWidgetVariant::ClassicInline;
      }

      return ActivityStatusWidgetVariant::Ambient;
    }

    ActivityStatusWidgetIdleBehavior parseIdleBehavior(std::string const& value,
                                                       ActivityStatusWidgetVariant const variant)
    {
      if (value == "reserve")
      {
        return ActivityStatusWidgetIdleBehavior::Reserve;
      }

      if (value == "hidden")
      {
        return ActivityStatusWidgetIdleBehavior::Hidden;
      }

      return variant == ActivityStatusWidgetVariant::ClassicInline ? ActivityStatusWidgetIdleBehavior::Reserve
                                                                   : ActivityStatusWidgetIdleBehavior::Hidden;
    }

    constexpr std::int32_t kMaxTextCharsMin = 8;
    constexpr std::int32_t kMaxTextCharsMax = 120;

    ActivityStatusWidgetOptions optionsFromNode(LayoutNode const& node)
    {
      auto const variant = parseVariant(node.propertyOr<std::string>("variant", "ambient"));
      auto const rawMaxTextChars = node.propertyOr<std::int64_t>("maxTextChars", kDefaultMaxTextChars);
      return ActivityStatusWidgetOptions{
        .variant = variant,
        .idleBehavior = parseIdleBehavior(node.propertyOr<std::string>("idleBehavior", ""), variant),
        .maxTextChars = static_cast<std::int32_t>(
          std::clamp(rawMaxTextChars, std::int64_t{kMaxTextCharsMin}, std::int64_t{kMaxTextCharsMax})),
      };
    }

    class ActivityStatusComponent final : public LayoutComponent
    {
    public:
      ActivityStatusComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _widget{ActivityStatusWidgetDependencies{
            .notifications = ctx.runtime.notifications(),
            .libraryChanges = &ctx.runtime.library().changes(),
            .options = optionsFromNode(node),
          }}
      {
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      ActivityStatusWidget _widget;
    };

    std::unique_ptr<LayoutComponent> createActivityStatus(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ActivityStatusComponent>(ctx, node);
    }
  } // namespace

  void registerActivityStatusComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "status.activityStatus",
                                .displayName = "Activity Status",
                                .category = LayoutComponentCategory::Status,
                                .props = {{.name = "variant",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Variant",
                                           .defaultValue = LayoutValue{"ambient"},
                                           .enumValues = {"ambient", "classicInline"}},
                                          {.name = "idleBehavior",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Idle Behavior",
                                           .defaultValue = LayoutValue{""},
                                           .enumValues = {"", "hidden", "reserve"}},
                                          {.name = "maxTextChars",
                                           .kind = LayoutPropertyKind::Int,
                                           .label = "Max Text Chars",
                                           .defaultValue = LayoutValue{kDefaultMaxTextChars}}},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createActivityStatus);
  }
} // namespace ao::gtk::layout
