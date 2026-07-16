// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/query/Parser.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <algorithm>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

namespace ao::uimodel
{
  namespace
  {
    class VariableVisitor final
    {
    public:
      void visit(query::Expression const& expr)
      {
        if (std::holds_alternative<query::VariableExpression>(expr))
        {
          auto const& var = std::get<query::VariableExpression>(expr);
          visitVariable(var);
          return;
        }

        if (std::holds_alternative<std::unique_ptr<query::BinaryExpression>>(expr))
        {
          auto const& binaryPtr = std::get<std::unique_ptr<query::BinaryExpression>>(expr);
          visit(binaryPtr->operand);

          if (binaryPtr->optOperation)
          {
            visit(binaryPtr->optOperation->operand);
          }

          return;
        }

        if (std::holds_alternative<std::unique_ptr<query::UnaryExpression>>(expr))
        {
          auto const& unaryPtr = std::get<std::unique_ptr<query::UnaryExpression>>(expr);
          visit(unaryPtr->operand);
        }
      }

      bool hasWork() const noexcept { return _hasWork; }
      bool hasComposer() const noexcept { return _hasComposer; }
      bool hasTag() const noexcept { return _hasTag; }
      bool hasGenre() const noexcept { return _hasGenre; }
      bool hasYear() const noexcept { return _hasYear; }
      bool hasAlbumArtist() const noexcept { return _hasAlbumArtist; }
      bool hasArtist() const noexcept { return _hasArtist; }
      bool hasAlbum() const noexcept { return _hasAlbum; }
      bool hasTechnical() const noexcept { return _hasTechnical; }

    private:
      void visitVariable(query::VariableExpression const& var)
      {
        if (var.type == query::VariableType::Tag)
        {
          _hasTag = true;
          return;
        }

        auto const* const descriptor = query::findQueryVariableDescriptor(var.type, var.name);

        if (descriptor == nullptr)
        {
          return;
        }

        switch (descriptor->field)
        {
          case query::Field::WorkId: _hasWork = true; break;
          case query::Field::ComposerId: _hasComposer = true; break;
          case query::Field::GenreId: _hasGenre = true; break;
          case query::Field::Year: _hasYear = true; break;
          case query::Field::AlbumArtistId: _hasAlbumArtist = true; break;
          case query::Field::ArtistId: _hasArtist = true; break;
          case query::Field::AlbumId: _hasAlbum = true; break;
          case query::Field::SampleRate:
          case query::Field::BitDepth:
          case query::Field::Bitrate: _hasTechnical = true; break;
          default: break;
        }
      }

      bool _hasWork = false;
      bool _hasComposer = false;
      bool _hasTag = false;
      bool _hasGenre = false;
      bool _hasYear = false;
      bool _hasAlbumArtist = false;
      bool _hasArtist = false;
      bool _hasAlbum = false;
      bool _hasTechnical = false;
    };

    rt::TrackPresentationSpec fallbackSpec(std::span<rt::TrackPresentationPreset const> builtinPresets)
    {
      if (builtinPresets.empty())
      {
        return {};
      }

      return builtinPresets.front().spec;
    }
  } // namespace

  rt::TrackPresentationSpec recommendPresentation(ListPresentationContext const& context,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets)
  {
    auto findPreset = [&](std::string_view id) -> rt::TrackPresentationSpec
    {
      if (auto const iter = std::ranges::find(
            builtinPresets, id, [](rt::TrackPresentationPreset const& preset) { return preset.spec.id; });
          iter != builtinPresets.end())
      {
        return iter->spec;
      }

      if (auto const iter = std::ranges::find(
            customPresets, id, [](rt::CustomTrackPresentationPreset const& preset) { return preset.spec.id; });
          iter != customPresets.end())
      {
        return iter->spec;
      }

      return fallbackSpec(builtinPresets);
    };

    if (context.sourceKind == ListPresentationSourceKind::Manual)
    {
      return findPreset(rt::kListOrderTrackPresentationId);
    }

    auto fallbackSpec = findPreset("albums");

    if (context.sourceKind != ListPresentationSourceKind::Smart || context.smartListFilter.empty())
    {
      return fallbackSpec;
    }

    auto const expr = query::parse(context.smartListFilter);

    if (!expr)
    {
      APP_LOG_DEBUG("TrackPresentationRecommender: parse failed for expression: {}", expr.error().message);
      return fallbackSpec;
    }

    auto visitor = VariableVisitor{};
    visitor.visit(*expr);

    if (visitor.hasWork())
    {
      return findPreset("classical-works");
    }

    if (visitor.hasComposer())
    {
      return findPreset("classical-composers");
    }

    if (visitor.hasTechnical())
    {
      return findPreset("technical");
    }

    if (visitor.hasTag())
    {
      return findPreset("tagging");
    }

    if (visitor.hasGenre() || visitor.hasYear())
    {
      return findPreset("albums");
    }

    if (visitor.hasAlbumArtist())
    {
      return findPreset("artists");
    }

    if (visitor.hasArtist() || visitor.hasAlbum())
    {
      return findPreset("albums");
    }

    return fallbackSpec;
  }
} // namespace ao::uimodel
