// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackComponentRegistrations.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "layout/component/track/TrackFieldGridCustomControls.h"
#include "layout/component/track/TrackFieldGridRows.h"
#include "layout/component/track/TrackFieldGridText.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackFieldUi.h"
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionService.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/field/TrackFieldEditPolicy.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/detail/TrackCustomMetadata.h>
#include <ao/uimodel/library/detail/TrackFieldGridPolicy.h>
#include <ao/uimodel/library/detail/TrackFieldGridSchema.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>
#include <sigc++/adaptors/track_obj.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    constexpr float kLabelOpacity = 0.6F;

    class ColumnWidthAnchor final : public Gtk::Widget
    {
    public:
      explicit ColumnWidthAnchor(std::string_view cssClass)
      {
        set_halign(Gtk::Align::FILL);
        set_hexpand(false);
        set_can_target(false);
        set_focusable(false);
        set_overflow(Gtk::Overflow::HIDDEN);
        set_size_request(0, -1);
        set_visible(true);
        add_css_class(std::string{cssClass});
      }

      void setWidgets(std::vector<Gtk::Widget*> widgets)
      {
        _widgets = std::move(widgets);
        queue_resize();
      }

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int /*forSize*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimumBaseline = -1;
        naturalBaseline = -1;
        minimum = 0;
        natural = 0;

        if (orientation != Gtk::Orientation::HORIZONTAL)
        {
          return;
        }

        for (auto* const widget : _widgets)
        {
          if (widget == nullptr)
          {
            continue;
          }

          std::int32_t labelMinimum = 0;
          std::int32_t labelNatural = 0;
          std::int32_t labelMinimumBaseline = -1;
          std::int32_t labelNaturalBaseline = -1;
          widget->measure(
            Gtk::Orientation::HORIZONTAL, -1, labelMinimum, labelNatural, labelMinimumBaseline, labelNaturalBaseline);
          natural = std::max(natural, labelNatural);
        }
      }

      void size_allocate_vfunc(int /*width*/, int /*height*/, int /*baseline*/) override {}

    private:
      std::vector<Gtk::Widget*> _widgets;
    };

    using namespace track_field_grid;

    class TrackFieldGridComponent final : public LayoutComponent
    {
    public:
      TrackFieldGridComponent(LayoutBuildContext& ctx, LayoutNode const& node)
        : _editCoordinator{ctx.parentWindow}
        , _library{ctx.runtime.library()}
        , _completion{ctx.runtime.completion()}
        , _notifications{ctx.runtime.notifications()}
        , _scope{ctx.detailScope}
        , _detailUndo{ctx.detailUndo}
        , _compositePrimarySizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
        , _compositeSecondarySizeGroupPtr{Gtk::SizeGroup::create(Gtk::SizeGroup::Mode::HORIZONTAL)}
      {
        _showAllFieldsButton.set_has_frame(false);
        _showAllFieldsButton.add_css_class("ao-detail-show-all-button");
        _showAllFieldsButton.add_css_class("ao-clickable");
        _showAllFieldsButton.set_halign(Gtk::Align::START);
        _showAllFieldsButton.set_size_request(0, -1);

        if (auto* const labelWidget = _showAllFieldsButton.get_child(); labelWidget != nullptr)
        {
          if (auto* const label = dynamic_cast<Gtk::Label*>(labelWidget); label != nullptr)
          {
            label->set_ellipsize(Pango::EllipsizeMode::END);
            label->set_size_request(0, -1);
          }
        }

        _showAllFieldsButton.signal_clicked().connect([this] { handleToggleShowEmptyMetadata(); });

        _metadataHeader.button.signal_clicked().connect([this] { handleToggleMetadata(); });
        _technicalHeader.button.signal_clicked().connect([this] { handleToggleTechnical(); });

        _metadataHeader.addCssClass("ao-track-detail-section-meta");
        _technicalHeader.addCssClass("ao-track-detail-section-tech");

        _metadataActionBox.set_hexpand(true);
        _metadataActionBox.add_css_class("ao-detail-field-action-row");
        _metadataActionSpacer.set_hexpand(true);
        _metadataActionSpacer.set_size_request(0, -1);
        _metadataActionBox.append(_showAllFieldsButton);
        _metadataActionBox.append(_metadataActionSpacer);
        _metadataActionBox.append(_addMetadataButton.button());

        updateHeaderStyles();

        _wrapper.setGrid(_grid);
        _wrapper.set_vexpand(true);
        _grid.set_hexpand(true);
        _grid.set_column_spacing(kGridColumnSpacing);
        _grid.set_row_spacing(4);
        _grid.set_valign(Gtk::Align::START);
        _grid.set_vexpand(true);
        _valueColumnWidthAnchor.set_hexpand(true);

        auto const requestedCategories =
          node.propertyOr<std::vector<std::string>>("categories", {"metadata", "technical"});
        auto const includesCategory = [&requestedCategories](std::string_view const category)
        {
          return std::ranges::any_of(
            requestedCategories, [category](std::string const& item) { return item == category; });
        };

        _metadataCategoryEnabled = includesCategory("metadata");

        auto const projection = uimodel::buildTrackFieldGridSchema(uimodel::TrackFieldGridSchemaOptions{
          .includeMetadata = _metadataCategoryEnabled,
          .includeTechnical = includesCategory("technical"),
        });

        for (auto const field : projection.metadataFields)
        {
          _metadataRows.emplace_back(field);
          configureBuiltInRow(_metadataRows.back());
        }

        for (auto const fields : projection.compositeMetadataFields)
        {
          _compositeRows.emplace_back(fields.primaryField, fields.secondaryField);
          configureCompositeRow(_compositeRows.back());
        }

        for (auto const field : projection.technicalFields)
        {
          _technicalRows.emplace_back(field);
          configureBuiltInRow(_technicalRows.back(), true);
        }

        buildAddMetadataUi();
        buildGrid();

        if (_scope != nullptr)
        {
          _scopeConn = _scope->signalSnapshotChanged().connect([this](auto const& snap) { handleSnapshot(snap); });
          handleSnapshot(_scope->snapshot());
        }
      }

      ~TrackFieldGridComponent() override = default;

      TrackFieldGridComponent(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent const&) = delete;
      TrackFieldGridComponent(TrackFieldGridComponent&&) = delete;
      TrackFieldGridComponent& operator=(TrackFieldGridComponent&&) = delete;

      Gtk::Widget& widget() override { return _wrapper; }

    private:
      static constexpr std::int32_t kGridColumnSpacing = 12;
      static constexpr std::int32_t kValueColWidth = 3;
      static constexpr std::int32_t kFieldRowHeight = 28;

      bool shouldShowRow(BuiltInRow const& row, rt::TrackDetailSnapshot const& snap) const
      {
        auto const text =
          uimodel::formatTrackFieldDisplayText(row.field, snap, uimodel::kMultipleTrackValuesText, true);
        return shouldShowTrackFieldGridMetadataFieldRow(
          TrackFieldGridMetadataFieldVisibility{.metadataExpanded = _metadataExpanded,
                                                .showEmptyMetadata = _showEmptyMetadata,
                                                .editorEditing = row.valueEditor.isEditing(),
                                                .hasDisplayText = !text.empty()});
      }

      bool shouldShowCompositeRow(CompositeBuiltInRow const& row, rt::TrackDetailSnapshot const& snap) const
      {
        auto const primText =
          uimodel::formatTrackFieldDisplayText(row.primaryField, snap, uimodel::kCompositeMixedTrackText, false);
        auto const secText =
          uimodel::formatTrackFieldDisplayText(row.secondaryField, snap, uimodel::kCompositeMixedTrackText, false);
        return shouldShowCompositeMetadataRow(CompositeMetadataVisibility{
          .metadataExpanded = _metadataExpanded,
          .showEmptyMetadata = _showEmptyMetadata,
          .primaryEditorEditing = row.primaryEditor.isEditing(),
          .secondaryEditorEditing = row.secondaryEditor.isEditing(),
          .hasPrimaryDisplayText = !primText.empty(),
          .hasSecondaryDisplayText = !secText.empty(),
        });
      }

      void updateHeaderLabels(rt::TrackDetailSnapshot const& snap)
      {
        if (_metadataExpanded)
        {
          _metadataHeader.label.set_text("Metadata");
        }
        else
        {
          auto const titleText = validUtf8Text(
            uimodel::formatTrackFieldDisplayText(rt::TrackField::Title, snap, uimodel::kMultipleTrackValuesText, true));
          auto const artistText = validUtf8Text(uimodel::formatTrackFieldDisplayText(
            rt::TrackField::Artist, snap, uimodel::kMultipleTrackValuesText, true));

          _metadataHeader.label.set_text(uimodel::formatMetadataHeader(titleText, artistText));
        }

        if (_technicalExpanded)
        {
          _technicalHeader.label.set_text("Audio Properties");
        }
        else
        {
          auto const codec =
            validUtf8Text(uimodel::formatTrackFieldDisplayText(rt::TrackField::Codec, snap, "", false));
          auto const sampleRate =
            validUtf8Text(uimodel::formatTrackFieldDisplayText(rt::TrackField::SampleRate, snap, "", false));
          auto const bitDepth =
            validUtf8Text(uimodel::formatTrackFieldDisplayText(rt::TrackField::BitDepth, snap, "", false));

          _technicalHeader.label.set_text(uimodel::formatTechnicalHeader(codec, sampleRate, bitDepth));
        }
      }

      void updateMetadataVisibility()
      {
        for (auto& row : _metadataRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        for (auto& row : _compositeRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        _metadataActionSlot.set_visible(_metadataExpanded);

        for (auto& row : _customRows)
        {
          row.labelSlot.set_visible(_metadataExpanded);
          row.valueSlot.set_visible(_metadataExpanded);
        }

        _metadataHeader.label.set_text("Metadata");
      }

      void updateMetadataVisibility(rt::TrackDetailSnapshot const& snap)
      {
        for (auto& row : _metadataRows)
        {
          bool const show = shouldShowRow(row, snap);
          row.labelSlot.set_visible(show);
          row.valueSlot.set_visible(show);
        }

        for (auto& row : _compositeRows)
        {
          bool const show = shouldShowCompositeRow(row, snap);
          row.labelSlot.set_visible(show);
          row.valueSlot.set_visible(show);
        }

        _metadataActionSlot.set_visible(_metadataExpanded);

        for (std::uint32_t i = 0U; i < snap.customMetadata.size() && i < _customRows.size(); ++i)
        {
          updateCustomRow(_customRows[i], snap.customMetadata[i]);
        }

        updateHeaderLabels(snap);
      }

      void handleToggleShowEmptyMetadata()
      {
        _showEmptyMetadata = !_showEmptyMetadata;
        _showAllFieldsButton.set_label(_showEmptyMetadata ? "Hide empty fields" : "Show empty fields");

        if (_scope != nullptr)
        {
          updateMetadataVisibility(_scope->snapshot());
        }
        else
        {
          updateMetadataVisibility();
        }
      }

      void handleToggleMetadata()
      {
        _metadataExpanded = !_metadataExpanded;
        _metadataHeader.setExpanded(_metadataExpanded);
        updateHeaderStyles();

        if (_scope != nullptr)
        {
          updateMetadataVisibility(_scope->snapshot());
        }
        else
        {
          updateMetadataVisibility();
        }
      }

      void handleToggleTechnical()
      {
        _technicalExpanded = !_technicalExpanded;
        _technicalHeader.setExpanded(_technicalExpanded);
        updateHeaderStyles();

        if (_scope != nullptr)
        {
          updateHeaderLabels(_scope->snapshot());
        }

        for (auto& row : _technicalRows)
        {
          row.labelSlot.set_visible(_technicalExpanded);
          row.valueSlot.set_visible(_technicalExpanded);
        }
      }

      void updateHeaderStyles()
      {
        auto toggleClass = [](auto& header, bool const expanded)
        {
          if (expanded)
          {
            header.button.remove_css_class("is-collapsed");
          }
          else
          {
            header.button.add_css_class("is-collapsed");
          }
        };

        toggleClass(_metadataHeader, _metadataExpanded);
        toggleClass(_technicalHeader, _technicalExpanded);
      }

      void handleSnapshot(rt::TrackDetailSnapshot const& snap)
      {
        for (auto& row : _metadataRows)
        {
          updateBuiltInRow(row, snap);
        }

        for (auto& row : _technicalRows)
        {
          updateBuiltInRow(row, snap);
        }

        for (auto& row : _compositeRows)
        {
          updateCompositeRow(row, snap);
        }

        updateCustomRows(snap);
        updateMetadataVisibility(snap);
      }

      void configureBuiltInRow(BuiltInRow& row, bool isTechnical = false)
      {
        auto const* def = rt::trackFieldDefinition(row.field);
        row.label.set_text(
          std::string{def != nullptr ? uimodel::PresentationTextCatalog{}.trackFieldLabel(def->field) : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditor(row.valueEditor);
        row.valueEditor.add_css_class("ao-property-value");

        if (isTechnical)
        {
          row.label.add_css_class("ao-detail-field-technical");
          row.valueEditor.add_css_class("ao-detail-field-technical-value");
        }

        if (row.editable)
        {
          _editCoordinator.registerEditor(row.valueEditor, [this] { beginEditSession(); });

          if (rt::supportsTrackFieldValueCompletion(row.field))
          {
            row.valueEditor.setCompletionProvider(rt::MetadataValueCompleter{_completion, row.field}.asProvider());
          }

          row.valueEditor.add_css_class("ao-property-editable");
          row.valueEditor.signalCommitted().connect(
            [this, field = row.field]
            {
              handleBuiltInEdited(field);
              clearEditSession();
            });
          row.valueEditor.signalCanceled().connect(
            [this, field = row.field]
            {
              clearEditSession();

              if (auto* row = findBuiltInRow(field); row != nullptr)
              {
                if (_scope != nullptr)
                {
                  updateBuiltInRow(*row, _scope->snapshot());
                }
              }
            });
        }
      }

      void configureCompositeRow(CompositeBuiltInRow& row)
      {
        auto const* def = rt::trackFieldDefinition(row.primaryField);
        row.label.set_text(
          std::string{def != nullptr ? uimodel::PresentationTextCatalog{}.trackFieldLabel(def->field) : ""});
        configureKeyLabel(row.label);

        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        row.separatorLabel.set_opacity(kLabelOpacity);

        configureValueEditor(row.primaryEditor);
        row.primaryEditor.add_css_class("ao-property-value");
        row.primaryEditor.removeMaxWidthConstraint();
        row.primaryEditor.set_hexpand(false);
        row.primaryEditor.set_halign(Gtk::Align::START);
        _compositePrimarySizeGroupPtr->add_widget(row.primaryEditor);

        configureValueEditor(row.secondaryEditor);
        row.secondaryEditor.add_css_class("ao-property-value");
        row.secondaryEditor.removeMaxWidthConstraint();
        row.secondaryEditor.set_hexpand(false);
        row.secondaryEditor.set_halign(Gtk::Align::START);
        _compositeSecondarySizeGroupPtr->add_widget(row.secondaryEditor);

        auto bindEditor = [this](DetailFieldEditor& editor, bool isEditable, rt::TrackField field)
        {
          if (isEditable)
          {
            _editCoordinator.registerEditor(editor, [this] { beginEditSession(); });

            if (rt::supportsTrackFieldValueCompletion(field))
            {
              editor.setCompletionProvider(rt::MetadataValueCompleter{_completion, field}.asProvider());
            }

            editor.add_css_class("ao-property-editable");
            editor.signalCommitted().connect(
              [this, field]
              {
                handleCompositeEdited(field);
                clearEditSession();
              });
            editor.signalCanceled().connect(
              [this, field]
              {
                clearEditSession();

                if (auto* row = findCompositeBuiltInRow(field); row != nullptr)
                {
                  if (_scope != nullptr)
                  {
                    updateCompositeRow(*row, _scope->snapshot());
                  }
                }
              });
          }
        };

        bindEditor(row.primaryEditor, row.primaryEditableFlag, row.primaryField);
        bindEditor(row.secondaryEditor, row.secondaryEditableFlag, row.secondaryField);
      }

      void updateBuiltInRow(BuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        if (row.valueEditor.isEditing())
        {
          return;
        }

        auto const displayText =
          validUtf8Text(uimodel::formatTrackFieldDisplayText(row.field, snap, uimodel::kMultipleTrackValuesText, true));
        row.valueEditor.setText(displayText);
        row.valueEditor.set_tooltip_text(displayText);
      }

      void updateCompositeRow(CompositeBuiltInRow& row, rt::TrackDetailSnapshot const& snap)
      {
        auto updateField = [](DetailFieldEditor& editor, rt::TrackField field, rt::TrackDetailSnapshot const& snap)
        {
          if (editor.isEditing())
          {
            return;
          }

          auto const displayText =
            validUtf8Text(uimodel::formatTrackFieldDisplayText(field, snap, uimodel::kCompositeMixedTrackText, false));
          editor.setText(displayText);
          editor.set_tooltip_text(displayText);
        };

        updateField(row.primaryEditor, row.primaryField, snap);
        updateField(row.secondaryEditor, row.secondaryField, snap);
      }

      bool applyFieldEdit(rt::TrackField field,
                          DetailFieldEditor& editor,
                          std::string_view newValue,
                          rt::TrackDetailSnapshot const& snap,
                          std::string_view mixedText,
                          bool showTechnicalUnknown)
      {
        auto const* uiDef = trackFieldUiDefinition(field);

        if (uiDef == nullptr || uiDef->parseInlineEdit == nullptr || !uimodel::canWriteTrackFieldPatch(field) ||
            _editSessionPtr == nullptr)
        {
          return false;
        }

        auto const oldText =
          validUtf8Text(uimodel::formatTrackFieldDisplayText(field, snap, mixedText, showTechnicalUnknown));
        auto const newText = std::string{newValue};

        if (newText == oldText)
        {
          return true;
        }

        auto const editValueResult = uiDef->parseInlineEdit(newText);

        if (!editValueResult)
        {
          APP_LOG_ERROR(
            "Failed to parse edit value for {}: {}", rt::trackFieldId(field), editValueResult.error().message);
          _notifications.post(
            rt::NotificationSeverity::Error, editValueResult.error().message, rt::NotificationLifetime::history());
          return false;
        }

        auto patch = rt::MetadataPatch{};

        if (!uimodel::writeTrackFieldPatch(patch, field, *editValueResult))
        {
          return false;
        }

        auto const replyResult = _editSessionPtr->submitMetadata(patch);

        if (reportMetadataSubmissionFailure(replyResult, "Metadata update"))
        {
          return false;
        }

        switch (replyResult->status)
        {
          case rt::TrackAuthoringStatus::Applied: editor.setText(newText); return true;
          case rt::TrackAuthoringStatus::NoOp: editor.setText(oldText); return true;
          case rt::TrackAuthoringStatus::Stale:
          case rt::TrackAuthoringStatus::Missing:
          case rt::TrackAuthoringStatus::Unavailable: return false;
        }

        return false;
      }

      void handleBuiltInEdited(rt::TrackField field)
      {
        auto* row = findBuiltInRow(field);

        if (row == nullptr || !row->editable || row->valueEditor.isEditing() || !_optEditSnapshot ||
            _editSessionPtr == nullptr)
        {
          return;
        }

        auto const& snap = *_optEditSnapshot;

        if (snap.trackIds.empty())
        {
          return;
        }

        auto const newValue = row->valueEditor.text().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, false))
        {
          return;
        }

        if (!applyFieldEdit(field, row->valueEditor, newValue, snap, uimodel::kMultipleTrackValuesText, true))
        {
          updateBuiltInRow(*row, snap);
        }
      }

      void handleCompositeEdited(rt::TrackField field)
      {
        auto* row = findCompositeBuiltInRow(field);

        if (row == nullptr || !_optEditSnapshot || _editSessionPtr == nullptr)
        {
          return;
        }

        bool const isPrimary = (row->primaryField == field);
        bool const editable = isPrimary ? row->primaryEditableFlag : row->secondaryEditableFlag;
        auto& editor = isPrimary ? row->primaryEditor : row->secondaryEditor;

        if (!editable || editor.isEditing())
        {
          return;
        }

        auto const& snap = *_optEditSnapshot;
        auto const newValue = editor.text().raw();

        if (isProtectedFieldEditValue(field, snap, newValue, true))
        {
          return;
        }

        if (!applyFieldEdit(field, editor, newValue, snap, uimodel::kCompositeMixedTrackText, false))
        {
          updateCompositeRow(*row, snap);
        }
      }

      void updateCustomRows(rt::TrackDetailSnapshot const& snap)
      {
        bool const selectedTracksChanged = _customSectionHasSelectedTracks != !snap.trackIds.empty();
        bool keysChanged = (snap.customMetadata.size() != _customRows.size());

        if (!keysChanged)
        {
          for (std::uint32_t i = 0U; i < snap.customMetadata.size(); ++i)
          {
            if (snap.customMetadata[i].key != _customRows[i].key)
            {
              keysChanged = true;
              break;
            }
          }
        }

        if (keysChanged || selectedTracksChanged)
        {
          for (auto& row : _customRows)
          {
            _editCoordinator.forgetEditor(row.editor);
          }

          _customRows.clear();

          for (auto const& item : snap.customMetadata)
          {
            _customRows.emplace_back(item.key, kGridColumnSpacing);
            configureCustomRow(_customRows.back());
          }

          buildGrid();
        }

        for (std::uint32_t i = 0U; i < snap.customMetadata.size(); ++i)
        {
          updateCustomRow(_customRows[i], snap.customMetadata[i]);
        }
      }

      void configureCustomRow(CustomRow& row)
      {
        row.label.set_text(validUtf8Text(row.key));
        configureKeyLabel(row.label);
        row.label.set_ellipsize(Pango::EllipsizeMode::END);
        row.label.set_width_chars(0);
        row.label.set_max_width_chars(24);
        row.label.set_opacity(kLabelOpacity);
        row.label.add_css_class("ao-property-label");

        configureValueBox(row.valueBox);
        configureValueEditor(row.editor);
        row.editor.add_css_class("ao-property-value");
        row.editor.add_css_class("ao-property-editable");
        _editCoordinator.registerEditor(row.editor, [this] { beginEditSession(); });
        row.editor.signalCommitted().connect(
          [this, key = row.key]
          {
            handleCustomEdited(key);
            clearEditSession();
          });
        row.editor.signalCanceled().connect(
          [this, key = row.key]
          {
            clearEditSession();

            if (auto* row = findCustomRow(key); row != nullptr)
            {
              if (_scope != nullptr)
              {
                updateCustomRows(_scope->snapshot());
              }
            }
          });

        row.deleteButton.set_icon_name("user-trash-symbolic");
        row.deleteButton.set_halign(Gtk::Align::END);
        row.deleteButton.set_hexpand(false);
        row.deleteButton.set_has_frame(false);
        row.deleteButton.add_css_class("ao-icon-button");
        row.deleteButton.add_css_class("ao-detail-field-delete");
        row.deleteButton.set_tooltip_text("Delete Custom Metadata");
        row.deleteButton.signal_clicked().connect([this, key = row.key] { handleCustomDeleted(key); });

        row.partialIcon.set_from_icon_name("dialog-warning-symbolic");
        row.partialIcon.set_opacity(kLabelOpacity);
        row.partialIcon.set_tooltip_text("Missing on some tracks");
      }

      void updateCustomRow(CustomRow& row, rt::CustomMetadataItem const& item)
      {
        auto const displayText = validUtf8Text(uimodel::formatTrackCustomMetadataDisplayText(item));
        auto const show = shouldShowTrackFieldGridMetadataFieldRow(
          TrackFieldGridMetadataFieldVisibility{.metadataExpanded = _metadataExpanded,
                                                .showEmptyMetadata = _showEmptyMetadata,
                                                .editorEditing = row.editor.isEditing(),
                                                .hasDisplayText = !displayText.empty()});
        row.labelSlot.set_visible(show);
        row.valueSlot.set_visible(show);

        if (row.editor.isEditing())
        {
          return;
        }

        row.editor.setText(displayText);
        row.editor.set_tooltip_text(displayText);
        row.partialIcon.set_visible(!item.presentOnAll);
      }

      void handleCustomEdited(std::string key)
      {
        auto* row = findCustomRow(key);

        if (row == nullptr || row->editor.isEditing() || !_optEditSnapshot || _editSessionPtr == nullptr)
        {
          return;
        }

        auto const& snap = *_optEditSnapshot;
        auto const newValue = row->editor.text().raw();

        if (uimodel::isProtectedTrackCustomMetadataEditText(newValue))
        {
          return;
        }

        auto const replyResult = _editSessionPtr->submitMetadata(uimodel::makeCustomMetadataUpdatePatch(key, newValue));

        if (reportMetadataSubmissionFailure(replyResult, "Custom metadata update"))
        {
          return;
        }

        if (replyResult->status == rt::TrackAuthoringStatus::Applied && _detailUndo != nullptr)
        {
          _detailUndo->clearIfAffectsCustomMetadata(key, snap.trackIds);
        }
      }

      void handleCustomDeleted(std::string key)
      {
        if (_scope == nullptr)
        {
          return;
        }

        auto const snap = _scope->snapshot();

        auto const optPrevValue = uimodel::undoValueForDeletedTrackCustomMetadata(snap, key);
        auto sessionResult = uimodel::TrackAuthoringSession::begin(_library, snap.trackIds);

        if (!sessionResult)
        {
          APP_LOG_ERROR("Custom metadata delete could not start: {}", sessionResult.error().message);
          _notifications.post(
            rt::NotificationSeverity::Error, sessionResult.error().message, rt::NotificationLifetime::history());
          return;
        }

        auto const replyResult = (*sessionResult)->submitMetadata(uimodel::makeCustomMetadataDeletePatch(key));

        if (reportMetadataSubmissionFailure(replyResult, "Custom metadata delete"))
        {
          return;
        }

        if (replyResult->status == rt::TrackAuthoringStatus::Applied && optPrevValue && _detailUndo != nullptr)
        {
          _detailUndo->presentCustomMetadataDeletedUndo(std::move(key), *optPrevValue, std::move(*sessionResult));
        }
      }

      void handleCustomAdded(std::string key, std::string value)
      {
        if (_scope == nullptr)
        {
          return;
        }

        auto const& snap = _scope->snapshot();

        if (uimodel::validateCustomMetadataAddition(snap, key) != uimodel::CustomMetadataAddValidation::Accepted)
        {
          _addMetadataButton.markKeyError();
          return;
        }

        _addMetadataButton.popdown();

        auto sessionResult = uimodel::TrackAuthoringSession::begin(_library, snap.trackIds);

        if (!sessionResult)
        {
          APP_LOG_ERROR("Custom metadata add could not start: {}", sessionResult.error().message);
          _notifications.post(
            rt::NotificationSeverity::Error, sessionResult.error().message, rt::NotificationLifetime::history());
          return;
        }

        auto const replyResult = (*sessionResult)->submitMetadata(uimodel::makeCustomMetadataUpdatePatch(key, value));

        if (reportMetadataSubmissionFailure(replyResult, "Custom metadata add"))
        {
          return;
        }

        if (replyResult->status == rt::TrackAuthoringStatus::Applied && _detailUndo != nullptr)
        {
          _detailUndo->clearIfAffectsCustomMetadata(key, snap.trackIds);
        }

        _addMetadataButton.clearInputs();
      }

      bool reportMetadataSubmissionFailure(Result<uimodel::TrackMetadataSubmitResult> const& result,
                                           std::string_view operation)
      {
        auto message = std::string{};

        if (!result)
        {
          message = result.error().message;
        }
        else
        {
          switch (result->status)
          {
            case rt::TrackAuthoringStatus::Applied:
            case rt::TrackAuthoringStatus::NoOp: return false;
            case rt::TrackAuthoringStatus::Stale:
              message = "Library changed while this edit was open. Reload the value and try again.";
              break;
            case rt::TrackAuthoringStatus::Missing: message = "One or more selected tracks no longer exist."; break;
            case rt::TrackAuthoringStatus::Unavailable: message = "Library editing is currently unavailable."; break;
          }
        }

        APP_LOG_ERROR("{} failed: {}", operation, message);
        _notifications.post(rt::NotificationSeverity::Error, message, rt::NotificationLifetime::history());
        return true;
      }

      void buildGrid()
      {
        _addMetadataButton.popdown();
        clearGrid();
        std::int32_t rowIndex = 0;

        refreshColumnWidthAnchors();
        _grid.attach(_keyColumnWidthAnchor, 0, 0, 1, 1);
        _grid.attach(_valueColumnWidthAnchor, 1, 0, kValueColWidth, 1);

        auto const sectionAvailability = TrackFieldGridSectionAvailability{
          .metadataCategoryEnabled = _metadataCategoryEnabled,
          .hasMetadataFields = !_metadataRows.empty() || !_compositeRows.empty(),
          .hasSelectedTracks = (_scope != nullptr && !_scope->snapshot().trackIds.empty()),
          .hasTechnicalFields = !_technicalRows.empty(),
        };
        _customSectionHasSelectedTracks = shouldRenderCustomMetadataArea(sectionAvailability);

        if (shouldRenderMetadataSection(sectionAvailability))
        {
          _metadataHeader.setExpanded(_metadataExpanded);
          _grid.attach(_metadataHeader.button, 0, rowIndex++, 1 + kValueColWidth, 1);

          attachBuiltInGroup(_metadataRows, rowIndex, _metadataExpanded);

          for (auto& row : _compositeRows)
          {
            attachCompositeRow(row, rowIndex++);
            row.labelSlot.set_visible(_metadataExpanded);
            row.valueSlot.set_visible(_metadataExpanded);
          }

          for (auto& row : _customRows)
          {
            _grid.attach(row.labelSlot, 0, rowIndex, 1, 1);
            _grid.attach(row.valueSlot, 1, rowIndex, kValueColWidth, 1);
            row.labelSlot.set_visible(_metadataExpanded);
            row.valueSlot.set_visible(_metadataExpanded);
            rowIndex++;
          }

          _showAllFieldsButton.set_visible(sectionAvailability.hasMetadataFields);
          _addMetadataButton.button().set_visible(shouldRenderCustomMetadataArea(sectionAvailability));
          _grid.attach(_metadataActionSlot, 0, rowIndex++, 1 + kValueColWidth, 1);
          _metadataActionSlot.set_visible(_metadataExpanded);
        }

        if (shouldRenderTechnicalSection(sectionAvailability))
        {
          _technicalHeader.setExpanded(_technicalExpanded);
          _grid.attach(_technicalHeader.button, 0, rowIndex++, 1 + kValueColWidth, 1);
          attachBuiltInGroup(_technicalRows, rowIndex, _technicalExpanded);
        }
      }

      void clearGrid()
      {
        while (auto* child = _grid.get_first_child())
        {
          _grid.remove(*child);
        }
      }

      void attachBuiltInRow(BuiltInRow& row, std::int32_t const rowNum)
      {
        _grid.attach(row.labelSlot, 0, rowNum, 1, 1);
        _grid.attach(row.valueSlot, 1, rowNum, kValueColWidth, 1);
      }

      void attachCompositeRow(CompositeBuiltInRow& row, std::int32_t const rowNum)
      {
        _grid.attach(row.labelSlot, 0, rowNum, 1, 1);
        _grid.attach(row.valueSlot, 1, rowNum, kValueColWidth, 1);
      }

      void attachBuiltInGroup(std::deque<BuiltInRow>& rows, std::int32_t& rowIndex, bool const expanded)
      {
        for (auto& row : rows)
        {
          attachBuiltInRow(row, rowIndex++);
          row.labelSlot.set_visible(expanded);
          row.valueSlot.set_visible(expanded);
        }
      }

      void refreshColumnWidthAnchors()
      {
        auto keyWidgets = std::vector<Gtk::Widget*>{};
        auto valueWidgets = std::vector<Gtk::Widget*>{};
        auto const rowCount = _metadataRows.size() + _compositeRows.size() + _technicalRows.size() + _customRows.size();
        keyWidgets.reserve(rowCount);
        valueWidgets.reserve(rowCount + 1);

        for (auto& row : _metadataRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _compositeRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _technicalRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        for (auto& row : _customRows)
        {
          keyWidgets.push_back(&row.label);
          valueWidgets.push_back(&row.valueBox);
        }

        _keyColumnWidthAnchor.setWidgets(std::move(keyWidgets));
        _valueColumnWidthAnchor.setWidgets(std::move(valueWidgets));
      }

      void configureValueBox(Gtk::Widget& box)
      {
        box.set_halign(Gtk::Align::FILL);
        box.set_hexpand(true);
        box.set_overflow(Gtk::Overflow::HIDDEN);
        box.set_size_request(0, -1);
      }

      void configureKeyLabel(Gtk::Label& label)
      {
        label.set_halign(Gtk::Align::END);
        label.set_xalign(1.0F);
        label.set_hexpand(false);
        label.set_overflow(Gtk::Overflow::HIDDEN);
        label.set_size_request(0, -1);
        label.set_ellipsize(Pango::EllipsizeMode::NONE);
        label.set_wrap(false);
        label.set_lines(1);
      }

      void configureValueEditor(DetailFieldEditor& editor)
      {
        editor.set_halign(Gtk::Align::FILL);
        editor.set_hexpand(true);
        editor.set_overflow(Gtk::Overflow::HIDDEN);
        editor.set_size_request(0, -1);
      }

      void beginEditSession()
      {
        clearEditSession();

        if (_scope == nullptr)
        {
          return;
        }

        auto snapshot = _scope->snapshot();

        if (snapshot.trackIds.empty())
        {
          return;
        }

        auto sessionResult = uimodel::TrackAuthoringSession::begin(_library, snapshot.trackIds);

        if (!sessionResult)
        {
          APP_LOG_ERROR("Metadata edit could not start: {}", sessionResult.error().message);
          _notifications.post(
            rt::NotificationSeverity::Error, sessionResult.error().message, rt::NotificationLifetime::history());
          return;
        }

        _optEditSnapshot.emplace(std::move(snapshot));
        _editSessionPtr = std::move(*sessionResult);
        _editSessionInvalidatedSubscription = _editSessionPtr->onInvalidated(
          [this]
          {
            Glib::signal_idle().connect_once(sigc::track_object(
              [this]
              {
                if (_editSessionPtr != nullptr && !_editSessionPtr->isCurrent())
                {
                  _editCoordinator.cancelActive();
                }
              },
              _wrapper));
          });
      }

      void clearEditSession()
      {
        _editSessionInvalidatedSubscription = {};
        _editSessionPtr.reset();
        _optEditSnapshot.reset();
      }

      void buildAddMetadataUi()
      {
        _addMetadataButton.signalAddRequested().connect([this](std::string key, std::string value)
                                                        { handleCustomAdded(std::move(key), std::move(value)); });
      }

      BuiltInRow* findBuiltInRow(rt::TrackField field)
      {
        for (auto& row : _metadataRows)
        {
          if (row.field == field)
          {
            return &row;
          }
        }

        for (auto& row : _technicalRows)
        {
          if (row.field == field)
          {
            return &row;
          }
        }

        return nullptr;
      }

      CompositeBuiltInRow* findCompositeBuiltInRow(rt::TrackField field)
      {
        for (auto& row : _compositeRows)
        {
          if (row.primaryField == field || row.secondaryField == field)
          {
            return &row;
          }
        }

        return nullptr;
      }

      CustomRow* findCustomRow(std::string const& key)
      {
        for (auto& row : _customRows)
        {
          if (row.key == key)
          {
            return &row;
          }
        }

        return nullptr;
      }

      Gtk::Grid _grid;
      DetailEditCoordinator _editCoordinator;
      rt::Library& _library;
      rt::CompletionService& _completion;
      rt::NotificationService& _notifications;
      TrackDetailScope* _scope;
      TrackDetailUndoController* _detailUndo;
      std::unique_ptr<uimodel::TrackAuthoringSession> _editSessionPtr;
      async::Subscription _editSessionInvalidatedSubscription;
      std::optional<rt::TrackDetailSnapshot> _optEditSnapshot;
      std::deque<BuiltInRow> _metadataRows;
      std::deque<CompositeBuiltInRow> _compositeRows;
      std::deque<BuiltInRow> _technicalRows;
      std::deque<CustomRow> _customRows;

      bool _metadataExpanded = true;
      bool _technicalExpanded = false;
      bool _metadataCategoryEnabled = true;
      bool _customSectionHasSelectedTracks = false;

      SectionHeaderRow _metadataHeader{"Metadata"};
      SectionHeaderRow _technicalHeader{"Audio Properties"};

      Gtk::Button _showAllFieldsButton{"Show empty fields"};
      Gtk::Label _metadataActionSpacer;
      AddCustomMetadataButton _addMetadataButton;
      Gtk::Box _metadataActionBox{Gtk::Orientation::HORIZONTAL, 4};
      FixedHeightWidgetSlot _metadataActionSlot{_metadataActionBox, false, false, kFieldRowHeight};
      bool _showEmptyMetadata = false;

      Glib::RefPtr<Gtk::SizeGroup> _compositePrimarySizeGroupPtr;
      Glib::RefPtr<Gtk::SizeGroup> _compositeSecondarySizeGroupPtr;

      ColumnWidthAnchor _keyColumnWidthAnchor{"ao-key-column-width-anchor"};
      ColumnWidthAnchor _valueColumnWidthAnchor{"ao-value-column-width-anchor"};

      sigc::connection _scopeConn;

      ConstrainedGridBox _wrapper;
    };

    std::unique_ptr<LayoutComponent> createTrackFieldGrid(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<TrackFieldGridComponent>(ctx, node);
    }
  } // namespace

  void registerTrackFieldGridComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "track.fieldGrid",
       .displayName = "Field Grid",
       .category = LayoutComponentCategory::Track,
       .props = {{.name = "categories", .kind = LayoutPropertyKind::StringList, .label = "Categories"}},
       .minChildren = 0,
       .optMaxChildren = 0},
      createTrackFieldGrid);
  }
} // namespace ao::gtk::layout
