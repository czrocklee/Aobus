// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr double kWeightScale = 1000.0;

    double roundedWeight(double value)
    {
      return std::round(value * kWeightScale) / kWeightScale;
    }

    std::int32_t normalizedMinimumWidth(TrackColumnSolveSpec const& spec)
    {
      return std::max(0, spec.minimumWidth);
    }

    std::int32_t normalizedDefaultWidth(TrackColumnSolveSpec const& spec)
    {
      return std::max(normalizedMinimumWidth(spec), spec.defaultWidth);
    }

    double effectiveWeight(TrackColumnSolveSpec const& spec)
    {
      auto const weight = spec.weight > 0.0 ? spec.weight : defaultTrackFieldColumnWeight(spec.field);
      return weight > 0.0 ? weight : 1.0;
    }

    std::int32_t effectiveFixedWidth(TrackColumnSolveSpec const& spec)
    {
      auto const preferred = spec.fixedWidth > 0 ? spec.fixedWidth : normalizedDefaultWidth(spec);
      return std::max(normalizedMinimumWidth(spec), preferred);
    }

    std::int32_t fallbackWidth(TrackColumnSolveSpec const& spec)
    {
      if (trackFieldColumnSizing(spec.field) == TrackColumnSizing::Fixed || spec.fixedWidth > 0)
      {
        return effectiveFixedWidth(spec);
      }

      return normalizedDefaultWidth(spec);
    }

    std::vector<std::int32_t> allocateByWeight(std::span<std::size_t const> indices,
                                               std::int32_t amount,
                                               std::span<TrackColumnSolveSpec const> specs)
    {
      auto shares = std::vector<std::int32_t>(indices.size(), 0);

      if (indices.empty() || amount <= 0)
      {
        return shares;
      }

      double totalWeight = 0.0;

      for (auto const index : indices)
      {
        totalWeight += effectiveWeight(specs[index]);
      }

      if (totalWeight <= 0.0)
      {
        totalWeight = static_cast<double>(indices.size());
      }

      std::int32_t assigned = 0;

      for (std::size_t index = 0; index < indices.size(); ++index)
      {
        auto const weight = totalWeight > 0.0 ? effectiveWeight(specs[indices[index]]) : 1.0;
        auto const share = static_cast<std::int32_t>(std::floor((static_cast<double>(amount) * weight) / totalWeight));
        shares[index] = share;
        assigned += share;
      }

      for (std::int32_t remainder = amount - assigned; remainder > 0; --remainder)
      {
        ++shares[static_cast<std::size_t>(amount - assigned - remainder) % shares.size()];
      }

      return shares;
    }

    void growColumns(std::vector<std::int32_t>& widths,
                     std::span<std::size_t const> indices,
                     std::int32_t amount,
                     std::span<TrackColumnSolveSpec const> specs)
    {
      if (amount <= 0 || indices.empty())
      {
        return;
      }

      auto const shares = allocateByWeight(indices, amount, specs);

      for (std::size_t index = 0; index < indices.size(); ++index)
      {
        widths[indices[index]] += shares[index];
      }
    }

    std::int32_t shrinkColumns(std::vector<std::int32_t>& widths,
                               std::span<std::size_t const> indices,
                               std::int32_t amount,
                               std::span<TrackColumnSolveSpec const> specs)
    {
      std::int32_t remaining = amount;
      auto active = std::vector<std::size_t>{indices.begin(), indices.end()};

      while (remaining > 0 && !active.empty())
      {
        auto const shares = allocateByWeight(active, remaining, specs);
        std::int32_t consumed = 0;
        auto nextActive = std::vector<std::size_t>{};
        nextActive.reserve(active.size());

        for (std::size_t shareIndex = 0; shareIndex < active.size(); ++shareIndex)
        {
          auto const index = active[shareIndex];
          auto const capacity = std::max(0, widths[index] - normalizedMinimumWidth(specs[index]));
          auto const reduction = std::min(capacity, shares[shareIndex]);
          widths[index] -= reduction;
          consumed += reduction;

          if (capacity > reduction)
          {
            nextActive.push_back(index);
          }
        }

        if (consumed == 0)
        {
          break;
        }

        remaining -= consumed;
        active = std::move(nextActive);
      }

      return remaining;
    }

    std::vector<std::size_t> flexibleIndicesOnSide(std::span<TrackColumnSolveSpec const> specs,
                                                   std::size_t resizedIndex,
                                                   bool rightSide)
    {
      auto indices = std::vector<std::size_t>{};

      if (rightSide)
      {
        for (std::size_t index = resizedIndex + 1; index < specs.size(); ++index)
        {
          if (trackFieldColumnSizing(specs[index].field) == TrackColumnSizing::Flexible)
          {
            indices.push_back(index);
          }
        }

        return indices;
      }

      for (std::size_t offset = 0; offset < resizedIndex; ++offset)
      {
        if (auto const index = resizedIndex - offset - 1;
            trackFieldColumnSizing(specs[index].field) == TrackColumnSizing::Flexible)
        {
          indices.push_back(index);
        }
      }

      std::ranges::reverse(indices);
      return indices;
    }
  } // namespace

  std::vector<TrackColumnSolveSpec> pixelTrackColumnSpecs(std::span<rt::TrackField const> fields,
                                                          std::span<TrackColumnState const> storedLayout)
  {
    auto specs = std::vector<TrackColumnSolveSpec>{};
    specs.reserve(fields.size());

    for (auto const field : fields)
    {
      auto const minWidth = minimumTrackFieldColumnWidth(field);
      auto const defaultWidth = std::max(minWidth, defaultTrackFieldColumnWidth(field));
      auto spec = TrackColumnSolveSpec{.field = field, .defaultWidth = defaultWidth, .minimumWidth = minWidth};
      auto const stateIt = std::ranges::find(storedLayout, field, &TrackColumnState::field);

      if (trackFieldColumnSizing(field) == TrackColumnSizing::Fixed)
      {
        spec.fixedWidth = stateIt != storedLayout.end() && stateIt->width > 0 ? stateIt->width : -1;
      }
      else
      {
        spec.weight = stateIt != storedLayout.end() && stateIt->weight > 0.0 ? stateIt->weight : -1.0;
      }

      specs.push_back(spec);
    }

    return specs;
  }

  std::vector<std::int32_t> solveTrackColumnWidths(std::span<TrackColumnSolveSpec const> specs,
                                                   std::int32_t viewportWidth)
  {
    auto widths = std::vector<std::int32_t>{};
    widths.reserve(specs.size());

    if (viewportWidth <= 0)
    {
      for (auto const& spec : specs)
      {
        widths.push_back(fallbackWidth(spec));
      }

      return widths;
    }

    widths.assign(specs.size(), 0);
    auto flexibleIndices = std::vector<std::size_t>{};
    std::int32_t fixedWidth = 0;
    std::int32_t flexibleMinWidth = 0;

    for (std::size_t index = 0; index < specs.size(); ++index)
    {
      if (auto const& spec = specs[index];
          trackFieldColumnSizing(spec.field) == TrackColumnSizing::Fixed || spec.fixedWidth > 0)
      {
        widths[index] = effectiveFixedWidth(spec);
        fixedWidth += widths[index];
      }
      else
      {
        flexibleIndices.push_back(index);
        flexibleMinWidth += normalizedMinimumWidth(spec);
      }
    }

    if (flexibleIndices.empty())
    {
      return widths;
    }

    auto remainingWidth = viewportWidth - fixedWidth;

    if (remainingWidth <= flexibleMinWidth)
    {
      for (auto const index : flexibleIndices)
      {
        widths[index] = normalizedMinimumWidth(specs[index]);
      }

      return widths;
    }

    auto active = flexibleIndices;

    while (!active.empty())
    {
      auto const shares = allocateByWeight(active, remainingWidth, specs);
      bool clamped = false;
      auto nextActive = std::vector<std::size_t>{};
      nextActive.reserve(active.size());

      for (std::size_t shareIndex = 0; shareIndex < active.size(); ++shareIndex)
      {
        auto const columnIndex = active[shareIndex];
        auto const minWidth = normalizedMinimumWidth(specs[columnIndex]);

        if (shares[shareIndex] < minWidth)
        {
          widths[columnIndex] = minWidth;
          remainingWidth -= minWidth;
          clamped = true;
        }
        else
        {
          nextActive.push_back(columnIndex);
        }
      }

      if (!clamped)
      {
        for (std::size_t shareIndex = 0; shareIndex < active.size(); ++shareIndex)
        {
          widths[active[shareIndex]] = shares[shareIndex];
        }

        break;
      }

      active = std::move(nextActive);
    }

    return widths;
  }

  std::vector<TrackColumnSolveSpec> specsFromWidths(std::span<TrackColumnSolveSpec const> priorSpecs,
                                                    std::span<std::int32_t const> widths)
  {
    auto specs = std::vector<TrackColumnSolveSpec>{priorSpecs.begin(), priorSpecs.end()};

    if (specs.size() != widths.size())
    {
      return specs;
    }

    auto flexibleIndices = std::vector<std::size_t>{};
    std::int32_t flexibleWidth = 0;

    for (std::size_t index = 0; index < specs.size(); ++index)
    {
      auto const clampedWidth = std::max(widths[index], normalizedMinimumWidth(specs[index]));

      if (trackFieldColumnSizing(specs[index].field) == TrackColumnSizing::Fixed || specs[index].fixedWidth > 0)
      {
        specs[index].fixedWidth = clampedWidth;
        specs[index].weight = -1.0;
      }
      else
      {
        flexibleIndices.push_back(index);
        flexibleWidth += clampedWidth;
        specs[index].fixedWidth = -1;
      }
    }

    if (flexibleWidth <= 0 || flexibleIndices.empty())
    {
      return specs;
    }

    auto const flexibleCount = static_cast<double>(flexibleIndices.size());

    for (auto const index : flexibleIndices)
    {
      auto const clampedWidth = std::max(widths[index], normalizedMinimumWidth(specs[index]));
      specs[index].weight =
        roundedWeight((static_cast<double>(clampedWidth) * flexibleCount) / static_cast<double>(flexibleWidth));
    }

    return specs;
  }

  std::vector<TrackColumnSolveSpec> resizeTrackColumnSpecs(std::span<TrackColumnSolveSpec const> specs,
                                                           rt::TrackField resizedField,
                                                           std::int32_t targetWidth,
                                                           std::int32_t viewportWidth)
  {
    auto resizedIt = std::ranges::find(specs, resizedField, &TrackColumnSolveSpec::field);

    if (resizedIt == specs.end())
    {
      return {specs.begin(), specs.end()};
    }

    auto const resizedIndex = static_cast<std::size_t>(std::distance(specs.begin(), resizedIt));
    auto widths = solveTrackColumnWidths(specs, viewportWidth);

    if (widths.size() != specs.size())
    {
      return {specs.begin(), specs.end()};
    }

    auto const resizedIsFlexible = trackFieldColumnSizing(specs[resizedIndex].field) == TrackColumnSizing::Flexible;
    auto const minTarget = normalizedMinimumWidth(specs[resizedIndex]);
    auto clampedTarget = std::max(targetWidth, minTarget);

    if (resizedIsFlexible)
    {
      std::int32_t otherFixedWidth = 0;
      std::int32_t otherFlexibleMinWidth = 0;
      bool hasOtherFlexible = false;

      for (std::size_t index = 0; index < specs.size(); ++index)
      {
        if (index == resizedIndex)
        {
          continue;
        }

        if (trackFieldColumnSizing(specs[index].field) == TrackColumnSizing::Fixed || specs[index].fixedWidth > 0)
        {
          otherFixedWidth += widths[index];
        }
        else
        {
          otherFlexibleMinWidth += normalizedMinimumWidth(specs[index]);
          hasOtherFlexible = true;
        }
      }

      auto const maxTarget = std::max(minTarget, viewportWidth - otherFixedWidth - otherFlexibleMinWidth);
      auto const minFeasibleTarget = hasOtherFlexible ? minTarget : maxTarget;
      clampedTarget = std::clamp(clampedTarget, minFeasibleTarget, maxTarget);
    }

    auto const delta = clampedTarget - widths[resizedIndex];
    widths[resizedIndex] = clampedTarget;

    auto const rightFlex = flexibleIndicesOnSide(specs, resizedIndex, true);
    auto const leftFlex = flexibleIndicesOnSide(specs, resizedIndex, false);

    if (delta > 0)
    {
      auto remainingDelta = shrinkColumns(widths, rightFlex, delta, specs);
      std::ignore = shrinkColumns(widths, leftFlex, remainingDelta, specs);
    }
    else if (delta < 0)
    {
      if (auto const growth = -delta; !rightFlex.empty())
      {
        growColumns(widths, rightFlex, growth, specs);
      }
      else
      {
        growColumns(widths, leftFlex, growth, specs);
      }
    }

    return specsFromWidths(specs, widths);
  }

  TrackColumnState canonicalTrackColumnState(TrackColumnSolveSpec const& spec)
  {
    if (trackFieldColumnSizing(spec.field) == TrackColumnSizing::Fixed || spec.fixedWidth > 0)
    {
      return TrackColumnState{.field = spec.field, .width = effectiveFixedWidth(spec), .weight = -1.0};
    }

    return TrackColumnState{.field = spec.field, .width = -1, .weight = roundedWeight(effectiveWeight(spec))};
  }
} // namespace ao::uimodel
