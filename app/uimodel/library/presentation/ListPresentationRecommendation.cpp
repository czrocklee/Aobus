// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/query/Parser.h>
#include <ao/rt/Log.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <algorithm>
#include <memory>
#include <span>
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

  rt::TrackPresentationSpec recommendListPresentation(ListPresentationContext const& context,
                                                      std::span<rt::TrackPresentationPreset const> builtinPresets)
  {
    auto findPreset = [builtinPresets](rt::BuiltinPreset id) -> rt::TrackPresentationSpec
    {
      auto const iter = std::ranges::find(builtinPresets, id, &rt::TrackPresentationPreset::presetId);
      return iter != builtinPresets.end() ? iter->spec : fallbackSpec(builtinPresets);
    };

    auto fallbackSpec = findPreset(rt::BuiltinPreset::Albums);

    if (context.sourceKind != ListPresentationSourceKind::SavedList || context.listExpression.empty())
    {
      return fallbackSpec;
    }

    auto const exprRes = query::parse(context.listExpression);

    if (!exprRes)
    {
      APP_LOG_DEBUG("ListPresentations: recommendation parse failed for expression: {}", exprRes.error().message);
      return fallbackSpec;
    }

    auto visitor = VariableVisitor{};
    visitor.visit(*exprRes);

    if (visitor.hasWork())
    {
      return findPreset(rt::BuiltinPreset::ClassicalWorks);
    }

    if (visitor.hasComposer())
    {
      return findPreset(rt::BuiltinPreset::ClassicalComposers);
    }

    if (visitor.hasTechnical())
    {
      return findPreset(rt::BuiltinPreset::Technical);
    }

    if (visitor.hasTag())
    {
      return findPreset(rt::BuiltinPreset::Tagging);
    }

    if (visitor.hasGenre() || visitor.hasYear())
    {
      return fallbackSpec;
    }

    if (visitor.hasAlbumArtist())
    {
      return findPreset(rt::BuiltinPreset::Artists);
    }

    // Artist and album queries fall through on purpose: "albums" is already the
    // default, so they need no branch of their own.
    return fallbackSpec;
  }
} // namespace ao::uimodel
