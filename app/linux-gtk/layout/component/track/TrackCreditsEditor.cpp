// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackCreditsEditor.h"

#include "common/AccessibleLabel.h"
#include "common/UiWorkflow.h"
#include "common/WidgetMeasure.h"
#include "completion/EntryCompletionController.h"
#include "i18n/GtkText.h"
#include "layout/component/track/TrackDetailScope.h"
#include "layout/component/track/TrackDetailUndo.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/Credits.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/comboboxtext.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/root.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>
#include <sigc++/adaptors/track_obj.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using i18n::MessageId;

  struct TrackCreditsEditor::Row final
  {
    Gtk::Grid grid;
    Gtk::Label kindLabel;
    Gtk::Label roleLabel;
    Gtk::Label nameLabel;
    Gtk::ComboBoxText kind;
    Gtk::Entry role;
    Gtk::Entry name;
    Gtk::Box actions{Gtk::Orientation::VERTICAL, 4};
    Gtk::Button deleteButton;
    Gtk::Button upButton;
    Gtk::Button downButton;
    std::unique_ptr<EntryCompletionController> nameCompletionPtr;
    std::unique_ptr<EntryCompletionController> roleCompletionPtr;
    sigc::scoped_connection nameConnection;
    sigc::scoped_connection roleConnection;
    sigc::scoped_connection kindConnection;
    sigc::scoped_connection deleteConnection;
    sigc::scoped_connection upConnection;
    sigc::scoped_connection downConnection;

    Gtk::Widget& validationControl(uimodel::TrackCreditValidationReason reason)
    {
      switch (reason)
      {
        case uimodel::TrackCreditValidationReason::InvalidRoleText: return role;
        case uimodel::TrackCreditValidationReason::InvalidKind: return kind;
        case uimodel::TrackCreditValidationReason::BlankName:
        case uimodel::TrackCreditValidationReason::InvalidNameText: return name;
      }

      return name;
    }
  };

  struct TrackCreditsEditor::PreparedEdit final
  {
    uimodel::TrackAuthoringSession session;
    uimodel::TrackCreditSections baseline;
  };

  TrackCreditsEditor::TrackCreditsEditor(async::Runtime& asyncRuntime,
                                         rt::Library& library,
                                         rt::CompletionService& completion,
                                         rt::NotificationService& notifications,
                                         i18n::MessageCatalog textCatalog,
                                         TrackDetailScope* scope,
                                         TrackDetailUndoController* undo)
    : Gtk::Widget{}
    , _async{&asyncRuntime}
    , _library{&library}
    , _completion{completion}
    , _notifications{&notifications}
    , _textCatalog{std::move(textCatalog)}
    , _scope{scope}
    , _undo{undo}
  {
    initialize();
  }

  TrackCreditsEditor::TrackCreditsEditor(rt::CompletionService& completion,
                                         i18n::MessageCatalog textCatalog,
                                         uimodel::TrackPropertiesFormModel& form,
                                         std::function<void()> changed)
    : Gtk::Widget{}
    , _completion{completion}
    , _textCatalog{std::move(textCatalog)}
    , _form{&form}
    , _changed{std::move(changed)}
  {
    initialize();
  }

  TrackCreditsEditor::~TrackCreditsEditor()
  {
    _scopeConnection.disconnect();
    resetSession();
    _tasks.cancelAll();

    for (auto& rowPtr : _rows)
    {
      rowPtr->nameCompletionPtr.reset();
      rowPtr->roleCompletionPtr.reset();
      _rowsBox.remove(rowPtr->grid);
    }

    _rows.clear();
    _contentBox.unparent();
  }

  Gtk::SizeRequestMode TrackCreditsEditor::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void TrackCreditsEditor::size_allocate_vfunc(int width, int height, int baseline)
  {
    std::int32_t minimum = 0;
    std::int32_t natural = 0;
    std::int32_t minimumBaseline = 0;
    std::int32_t naturalBaseline = 0;
    _contentBox.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
    _contentBox.size_allocate({0, 0, std::max(width, minimum), height}, baseline);
  }

  void TrackCreditsEditor::measure_vfunc(Gtk::Orientation orientation,
                                         int forSize,
                                         int& minimum,
                                         int& natural,
                                         int& minimumBaseline,
                                         int& naturalBaseline) const
  {
    if (orientation == Gtk::Orientation::VERTICAL && forSize >= 0)
    {
      auto const childWidth = measureWidget(_contentBox, Gtk::Orientation::HORIZONTAL, -1);
      forSize = std::max(forSize, childWidth.minimum);
    }

    _contentBox.measure(orientation, forSize, minimum, natural, minimumBaseline, naturalBaseline);

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      if (forSize < 0)
      {
        minimum = 0;
      }
      else
      {
        auto const childWidth = measureWidget(_contentBox, Gtk::Orientation::HORIZONTAL, -1);
        auto const childHeight = measureWidget(_contentBox, Gtk::Orientation::VERTICAL, childWidth.minimum);
        // Clipping permits zero width only when the allocated content's wrapped
        // height fits. Otherwise retain its inverse width-for-height request.
        if (forSize >= childHeight.minimum)
        {
          minimum = 0;
        }
      }
    }
  }

  void TrackCreditsEditor::initialize()
  {
    _contentBox.set_parent(*this);
    add_css_class("ao-track-credits");
    set_size_request(0, -1);
    set_overflow(Gtk::Overflow::HIDDEN);
    set_hexpand(true);
    _heading.set_text(gtkText(_textCatalog, MessageId::TrackCreditsHeading));
    _heading.set_xalign(0.0F);
    _heading.add_css_class("heading");
    _contentBox.append(_heading);

    for (std::size_t i = 0; i < _categoryButtons.size(); ++i)
    {
      auto& button = _categoryButtons[i];
      button.add_css_class("ao-credit-category");
      button.set_has_frame(false);
      _categoryConnections[i] = button.signal_clicked().connect(
        [this, i] { beginEditing(uimodel::trackCreditScope(static_cast<library::CreditKind>(i))); });
      _contentBox.append(button);
    }

    _scopeLabel.set_wrap(true);
    _scopeLabel.set_xalign(0.0F);
    _contentBox.append(_scopeLabel);
    _contentBox.append(_rowsBox);
    _editButton.set_label(gtkText(_textCatalog, MessageId::GtkEditValue));
    _replaceButton.set_label(gtkText(_textCatalog, MessageId::TrackCreditsReplaceScope));
    _addButton.set_label(gtkText(_textCatalog, MessageId::TrackCreditsAdd));
    _clearButton.set_label(gtkText(_textCatalog, MessageId::TrackCreditsClear));
    _commitButton.set_label(gtkText(_textCatalog, MessageId::TrackCreditsCommit));
    _cancelButton.set_label(gtkText(_textCatalog, MessageId::TrackCreditsCancel));
    _editButton.add_css_class("ao-credits-edit");
    _replaceButton.add_css_class("ao-credits-replace");
    _addButton.add_css_class("ao-credits-add");
    _clearButton.add_css_class("ao-credits-clear");
    _commitButton.add_css_class("ao-credits-commit");
    _cancelButton.add_css_class("ao-credits-cancel");

    for (auto* button : {&_editButton, &_replaceButton, &_addButton, &_clearButton, &_commitButton, &_cancelButton})
    {
      button->set_has_frame(false);
      button->set_tooltip_text(button->get_label());

      if (auto* label = dynamic_cast<Gtk::Label*>(button->get_child()); label != nullptr)
      {
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_size_request(0, -1);
      }

      _actions.append(*button);
    }

    _editConnection = _editButton.signal_clicked().connect([this] { beginEditing(uimodel::allTrackCreditKinds()); });
    _replaceConnection = _replaceButton.signal_clicked().connect(
      [this]
      {
        if (!_submitting && !_renderQueued && draft().isEditing())
        {
          draft().beginReplacement();
          queueRender();
        }
      });
    _addConnection = _addButton.signal_clicked().connect(
      [this]
      {
        if (!_submitting && !_renderQueued && draft().canEdit())
        {
          draft().addEntry(addKind());
          queueRender();
        }
      });
    _clearConnection = _clearButton.signal_clicked().connect([this] { clearCredits(); });
    _commitConnection = _commitButton.signal_clicked().connect([this] { commit(); });
    _cancelConnection = _cancelButton.signal_clicked().connect([this] { cancelEditing(); });
    _contentBox.append(_actions);
    _validation.set_wrap(true);
    _validation.set_xalign(0.0F);
    _validation.add_css_class("error");
    _contentBox.append(_validation);

    if (_scope != nullptr)
    {
      _snapshot = _scope->snapshot();
      _scopeConnection =
        _scope->signalSnapshotChanged().connect([this](auto const& snapshot) { handleSnapshot(snapshot); });
    }

    render();
  }

  uimodel::TrackCreditsEditorModel& TrackCreditsEditor::draft()
  {
    return _form != nullptr ? _form->creditsEditor() : _draft;
  }

  uimodel::TrackCreditSections TrackCreditsEditor::sections() const
  {
    return _form != nullptr ? _form->creditSections() : _snapshot.credits;
  }

  void TrackCreditsEditor::updateVisibility(bool metadataExpanded, bool showEmpty)
  {
    _metadataExpanded = metadataExpanded;
    _showEmpty = showEmpty;
    set_visible(_form != nullptr ||
                uimodel::shouldShowTrackCredits(
                  _metadataExpanded, _showEmpty, !_snapshot.trackIds.empty(), _snapshot.credits, draft().isEditing()));
  }

  void TrackCreditsEditor::refresh()
  {
    queueRender();
  }

  void TrackCreditsEditor::handleSnapshot(rt::TrackDetailSnapshot const& snapshot)
  {
    if (_snapshot.trackIds != snapshot.trackIds)
    {
      ++_generation;
      draft().cancel();
      resetSession();
      _submitting = false;
      _focusTarget = FocusTarget::None;
      _entryPoint = nullptr;
    }

    _snapshot = snapshot;

    if (draft().isEditing() || _submitting)
    {
      // Refresh validity, never reseed the live draft or retire its completions.
      refreshValidation();
      updateVisibility(_metadataExpanded, _showEmpty);
      return;
    }

    queueRender();
  }

  Result<TrackCreditsEditor::PreparedEdit> TrackCreditsEditor::prepareEditing() const
  {
    auto snapshot = _library->snapshot();
    auto sessionRes = uimodel::TrackAuthoringSession::begin(*_library, _snapshot.trackIds, snapshot);

    if (!sessionRes)
    {
      return std::unexpected{sessionRes.error()};
    }

    auto baselineRes = uimodel::loadTrackCreditsEditorBaseline(snapshot, sessionRes->targetIds());

    if (!baselineRes)
    {
      return std::unexpected{baselineRes.error()};
    }

    if (!sessionRes->isCurrent())
    {
      return makeError(Error::Code::InvalidState, gtkText(_textCatalog, MessageId::TrackEditStale));
    }

    return PreparedEdit{.session = std::move(*sessionRes), .baseline = std::move(*baselineRes)};
  }

  void TrackCreditsEditor::beginEditing(std::bitset<library::kCreditKindCount> kinds)
  {
    if (_submitting || draft().isEditing())
    {
      return;
    }

    if (_form != nullptr)
    {
      if (auto res = _form->beginCreditsEdit(kinds); !res)
      {
        reportError(res.error().message);
        return;
      }
    }
    else
    {
      auto preparedRes = prepareEditing();

      if (!preparedRes)
      {
        reportError(preparedRes.error().message);
        return;
      }

      _baseline = std::move(preparedRes->baseline);
      setSession(std::move(preparedRes->session));

      if (auto res = draft().begin(_baseline, kinds); !res)
      {
        reportError(res.error().message);
        resetSession();
        return;
      }
    }

    ++_generation;
    _entryPoint = &_editButton;

    if (kinds.count() == 1)
    {
      for (std::size_t i = 0; i < _categoryButtons.size(); ++i)
      {
        if (kinds.test(i))
        {
          _entryPoint = &_categoryButtons[i];
          break;
        }
      }
    }

    if (!draft().entries().empty())
    {
      draft().focusRow(0);
    }

    _focusTarget = FocusTarget::Draft;
    queueRender();
  }

  void TrackCreditsEditor::cancelEditing()
  {
    ++_generation;

    if (_form != nullptr)
    {
      _form->cancelCreditsEdit();
    }
    else
    {
      draft().cancel();
      resetSession();
    }

    _submitting = false;
    _focusTarget = FocusTarget::EntryPoint;
    queueRender();
  }

  void TrackCreditsEditor::setSession(uimodel::TrackAuthoringSession session)
  {
    resetSession();
    _optSession.emplace(std::move(session));
    _sessionInvalidatedSubscription = _optSession->onInvalidated([this] { refreshValidation(); });
  }

  void TrackCreditsEditor::resetSession()
  {
    _sessionInvalidatedSubscription.reset();
    _optSession.reset();
  }

  void TrackCreditsEditor::clearCredits()
  {
    if (_submitting || _renderQueued || !draft().isEditing())
    {
      return;
    }

    // Clear is explicit and remains a draft until Commit. Its scope stays visible.
    draft().clearScope();
    queueRender();
  }

  void TrackCreditsEditor::commit()
  {
    if (_submitting || _renderQueued)
    {
      return;
    }

    auto patchRes = draft().buildCommitPatch();

    if (!patchRes)
    {
      if (auto const errors = draft().validationErrors(); !errors.empty() && errors.front().rowIndex < _rows.size())
      {
        draft().focusRow(errors.front().rowIndex);
        _rows[errors.front().rowIndex]->validationControl(errors.front().reason).grab_focus();
      }

      refreshValidation();
      return;
    }

    if (_form != nullptr)
    {
      if (auto res = _form->acceptCreditsEdit(); !res)
      {
        reportError(res.error().message);
        return;
      }

      _focusTarget = FocusTarget::EntryPoint;
      queueRender();
      return;
    }

    bool const clear = patchRes->optCredits && patchRes->optCredits->entries.empty();
    submit(std::move(*patchRes), clear);
  }

  void TrackCreditsEditor::submit(rt::MetadataPatch patch, bool clear)
  {
    if (!_optSession)
    {
      return;
    }

    auto& boundSession = *_optSession;
    auto const kinds = draft().scope();
    auto const generation = _generation;
    auto trackIds = std::vector<TrackId>{boundSession.targetIds().begin(), boundSession.targetIds().end()};
    auto optUndoEntries = clear ? uimodel::undoValueForClearedTrackCredits(_baseline, kinds) : std::nullopt;
    _sessionInvalidatedSubscription.reset();
    auto session = std::move(boundSession);
    resetSession();
    auto submission = session.submitMetadataAsync(std::move(patch));
    _submitting = true;
    refreshValidation();
    spawnUiTask(
      *_async,
      _tasks,
      *this,
      "credits update",
      std::move(submission),
      [generation,
       kinds,
       trackIds = std::move(trackIds),
       optUndoEntries = std::move(optUndoEntries),
       session = std::move(session),
       clear](TrackCreditsEditor* owner, Result<uimodel::TrackMetadataSubmitResult> replyRes) mutable
      {
        if (replyRes && replyRes->status == rt::AuthoringStatus::Applied && owner->_undo != nullptr)
        {
          owner->_undo->clearIfAffectsCredits(trackIds, session.boundRevision(), kinds);
        }

        if (owner->_generation != generation || owner->_snapshot.trackIds != trackIds)
        {
          return;
        }

        owner->_submitting = false;

        if (replyRes && replyRes->status == rt::AuthoringStatus::Busy)
        {
          owner->setSession(std::move(session));
          owner->_notifications->post(rt::NotificationSeverity::Warning,
                                      gtkText(owner->_textCatalog, MessageId::LibraryBusyTryAgain),
                                      rt::NotificationLifetime::transient());
          owner->refreshValidation();
          return;
        }

        if (!replyRes ||
            (replyRes->status != rt::AuthoringStatus::Applied && replyRes->status != rt::AuthoringStatus::NoOp))
        {
          owner->reportError(!replyRes ? replyRes.error().message
                                       : gtkText(owner->_textCatalog,
                                                 replyRes->status == rt::AuthoringStatus::Stale
                                                   ? MessageId::TrackEditStale
                                                   : MessageId::TrackEditingUnavailable));
          owner->setSession(std::move(session));
          owner->refreshValidation();
          return;
        }

        if (clear && replyRes->status == rt::AuthoringStatus::Applied && optUndoEntries && owner->_undo != nullptr)
        {
          owner->_undo->presentCreditsClearedUndo(std::move(*optUndoEntries), std::move(session));
        }

        // Do not steal focus if the user moved elsewhere while the write ran.
        auto* root = owner->get_root();

        if (auto* focus = root != nullptr ? root->get_focus() : nullptr; focus == nullptr || focus->is_ancestor(*owner))
        {
          owner->_focusTarget = FocusTarget::EntryPoint;
        }

        owner->draft().cancel();
        owner->queueRender();
      });
  }

  void TrackCreditsEditor::queueRender()
  {
    if (_changed)
    {
      _changed();
    }

    if (_renderQueued)
    {
      return;
    }

    _renderQueued = true;

    if (draft().isEditing() && _focusTarget == FocusTarget::None)
    {
      _focusTarget = FocusTarget::Draft;
    }

    // Topology actions dispatch from buttons/kind controls, not completions.
    // Revoke completion attachments now; row widgets remain alive until idle.
    for (auto& rowPtr : _rows)
    {
      rowPtr->nameCompletionPtr.reset();
      rowPtr->roleCompletionPtr.reset();
    }

    retireRowFocus();
    _rowsBox.set_sensitive(false);
    // Retire on the next main-loop turn, never on a dispatching row's stack.
    Glib::signal_idle().connect_once(sigc::track_object(
      [this]
      {
        _renderQueued = false;
        render();
      },
      *this));
  }

  void TrackCreditsEditor::render()
  {
    for (auto& rowPtr : _rows)
    {
      rowPtr->nameCompletionPtr.reset();
      rowPtr->roleCompletionPtr.reset();
      _rowsBox.remove(rowPtr->grid);
    }

    _rows.clear();
    bool const editing = draft().isEditing();
    auto const currentSections = sections();
    auto const displayRows = uimodel::formatTrackCreditDisplayRows(_textCatalog, currentSections);
    auto const count = editing ? draft().entries().size() : displayRows.size();
    renderCategories(currentSections, editing);
    auto const scopeText = uimodel::trackCreditScopeLabel(_textCatalog, draft().scope());
    _scopeLabel.set_text(i18n::requiredFormat(_textCatalog, MessageId::TrackCreditsScope, {{"scope", scopeText}}));
    _scopeLabel.set_visible(editing);

    for (std::size_t i = 0; i < count; ++i)
    {
      auto rowPtr = std::make_unique<Row>();
      auto& row = *rowPtr;
      row.grid.set_hexpand(true);
      row.grid.set_size_request(0, -1);
      row.name.set_hexpand(true);
      row.name.set_width_chars(1);
      row.role.set_hexpand(true);
      row.role.set_width_chars(1);
      row.name.add_css_class("ao-credit-name");
      row.role.add_css_class("ao-credit-role");
      row.deleteButton.add_css_class("ao-credit-delete");
      row.upButton.add_css_class("ao-credit-up");
      row.downButton.add_css_class("ao-credit-down");
      row.kind.add_css_class("ao-credit-kind");

      if (editing)
      {
        renderEditingRow(row, i, count);
      }
      else
      {
        renderDisplayRow(row, displayRows[i]);
      }

      _rowsBox.append(row.grid);
      _rows.push_back(std::move(rowPtr));
    }

    _editButton.set_visible(!editing);
    _replaceButton.set_visible(editing && !draft().canEdit());
    _addButton.set_visible(editing && draft().canEdit());
    _clearButton.set_visible(editing);
    _commitButton.set_visible(editing);
    _cancelButton.set_visible(editing);
    refreshValidation();
    updateVisibility(_metadataExpanded, _showEmpty);

    restoreFocus();
  }

  void TrackCreditsEditor::renderCategories(uimodel::TrackCreditSections const& currentSections, bool editing)
  {
    for (std::size_t i = 0; i < _categoryButtons.size(); ++i)
    {
      auto const kind = static_cast<library::CreditKind>(i);
      auto text = std::string{uimodel::trackCreditKindLabel(_textCatalog, kind)};
      text += ": ";
      text += uimodel::formatTrackCreditSectionSummary(_textCatalog, currentSections[i]);

      auto& button = _categoryButtons[i];
      button.set_label(text);
      button.set_tooltip_text(text);
      button.set_visible(!editing);
      button.set_sensitive(!_submitting && (_form != nullptr || !_snapshot.trackIds.empty()));

      if (auto* label = dynamic_cast<Gtk::Label*>(button.get_child()); label != nullptr)
      {
        label->set_ellipsize(Pango::EllipsizeMode::END);
        label->set_size_request(0, -1);
      }
    }
  }

  void TrackCreditsEditor::renderEditingRow(Row& row, std::size_t index, std::size_t count)
  {
    auto const& entry = draft().entries()[index];
    row.name.set_text(entry.name);
    row.role.set_text(entry.role);
    row.name.set_placeholder_text(gtkText(_textCatalog, MessageId::TrackCreditName));
    row.role.set_placeholder_text(gtkText(_textCatalog, MessageId::TrackCreditRole));
    setTooltipAndAccessibleLabel(row.name, gtkText(_textCatalog, MessageId::TrackCreditName));
    setTooltipAndAccessibleLabel(row.role, gtkText(_textCatalog, MessageId::TrackCreditRole));
    setTooltipAndAccessibleLabel(row.kind, gtkText(_textCatalog, MessageId::TrackCreditKind));

    for (std::size_t i = 0; i < library::kCreditKindCount; ++i)
    {
      row.kind.append(std::string{uimodel::trackCreditKindLabel(_textCatalog, static_cast<library::CreditKind>(i))});
    }

    row.kind.set_active(static_cast<std::int32_t>(entry.kind));
    row.kind.set_visible(draft().scope().count() > 1);
    row.grid.attach(row.kind, 0, 0);
    row.grid.attach(row.name, 0, 1);
    row.grid.attach(row.role, 0, 2);
    row.deleteButton.set_icon_name("user-trash-symbolic");
    row.upButton.set_icon_name("go-up-symbolic");
    row.downButton.set_icon_name("go-down-symbolic");
    setTooltipAndAccessibleLabel(row.deleteButton, gtkText(_textCatalog, MessageId::TrackCreditDelete));
    setTooltipAndAccessibleLabel(row.upButton, gtkText(_textCatalog, MessageId::TrackCreditMoveUp));
    setTooltipAndAccessibleLabel(row.downButton, gtkText(_textCatalog, MessageId::TrackCreditMoveDown));
    row.upButton.set_sensitive(index > 0 && draft().entries()[index - 1].kind == entry.kind);
    row.downButton.set_sensitive(index + 1 < count && draft().entries()[index + 1].kind == entry.kind);

    for (auto* button : {&row.upButton, &row.downButton, &row.deleteButton})
    {
      button->set_has_frame(false);
      row.actions.append(*button);
    }

    row.grid.attach(row.actions, 0, 3);
    row.nameCompletionPtr = std::make_unique<EntryCompletionController>(
      row.name, _textCatalog, rt::makeCreditNameCompletionProvider(_completion, entry.kind));
    row.roleCompletionPtr = std::make_unique<EntryCompletionController>(
      row.role, _textCatalog, rt::makeCreditRoleCompletionProvider(_completion));
    connectRowSignals(row, index);
  }

  void TrackCreditsEditor::connectRowSignals(Row& row, std::size_t index)
  {
    row.nameConnection = row.name.signal_changed().connect(
      [this, index, handle = &row]
      {
        if (!_renderQueued && !_submitting && draft().isEditing())
        {
          draft().updateName(index, handle->name.get_text().raw());
          draft().focusRow(index);
          refreshValidation();
        }
      });
    row.roleConnection = row.role.signal_changed().connect(
      [this, index, handle = &row]
      {
        if (!_renderQueued && !_submitting && draft().isEditing())
        {
          draft().updateRole(index, handle->role.get_text().raw());
          draft().focusRow(index);
          refreshValidation();
        }
      });
    row.kindConnection = row.kind.signal_changed().connect(
      [this, index, handle = &row]
      {
        if (!_renderQueued && !_submitting && handle->kind.get_active_row_number() >= 0)
        {
          draft().changeKind(index, static_cast<library::CreditKind>(handle->kind.get_active_row_number()));
          queueRender();
        }
      });
    row.upConnection = row.upButton.signal_clicked().connect(
      [this, index]
      {
        if (!_renderQueued && !_submitting && index > 0)
        {
          draft().moveEntry(index, index - 1);
          queueRender();
        }
      });
    row.downConnection = row.downButton.signal_clicked().connect(
      [this, index]
      {
        if (!_renderQueued && !_submitting)
        {
          draft().moveEntry(index, index + 1);
          queueRender();
        }
      });
    row.deleteConnection = row.deleteButton.signal_clicked().connect(
      [this, index]
      {
        if (!_renderQueued && !_submitting)
        {
          draft().deleteEntry(index);
          queueRender();
        }
      });
  }

  void TrackCreditsEditor::renderDisplayRow(Row& row, uimodel::TrackCreditDisplayRow const& displayRow)
  {
    row.kindLabel.set_text(displayRow.kindLabel);
    row.roleLabel.set_text(displayRow.role);
    row.nameLabel.set_text(displayRow.name);

    for (auto* label : {&row.kindLabel, &row.roleLabel, &row.nameLabel})
    {
      label->set_xalign(0.0F);
      label->set_hexpand(true);
      label->set_ellipsize(Pango::EllipsizeMode::END);
      label->set_size_request(0, -1);
      label->set_tooltip_text(label->get_text());
    }

    row.grid.attach(row.kindLabel, 0, 0);
    row.grid.attach(row.nameLabel, 0, 1);
    row.grid.attach(row.roleLabel, 0, 2);
  }

  void TrackCreditsEditor::retireRowFocus()
  {
    auto* root = get_root();

    if (auto* focus = root != nullptr ? root->get_focus() : nullptr; focus != nullptr && focus->is_ancestor(_rowsBox))
    {
      // Deliver focus-out while the entry is still sensitive and parented.
      // Otherwise its cursor tick can outlive focus during deferred retirement.
      root->unset_focus();
    }
  }

  void TrackCreditsEditor::restoreFocus()
  {
    auto const target = std::exchange(_focusTarget, FocusTarget::None);

    if (target == FocusTarget::EntryPoint && !draft().isEditing())
    {
      if (auto* button = std::exchange(_entryPoint, nullptr); button != nullptr)
      {
        button->grab_focus();
      }
    }
    else if (target == FocusTarget::Draft && draft().isEditing())
    {
      if (!draft().canEdit())
      {
        _replaceButton.grab_focus();
      }
      else if (auto const optFocus = draft().focusedRow(); optFocus && *optFocus < _rows.size())
      {
        _rows[*optFocus]->name.grab_focus();
      }
      else
      {
        _addButton.grab_focus();
      }
    }
  }

  void TrackCreditsEditor::refreshValidation()
  {
    bool const editing = draft().isEditing();
    bool const current = _form != nullptr || (_optSession && _optSession->isCurrent());
    auto const errors = draft().validationErrors();

    if (!current && editing)
    {
      _validation.set_text(gtkText(_textCatalog, MessageId::TrackEditStale));
    }
    else if (!errors.empty())
    {
      _validation.set_text(uimodel::formatTrackCreditValidationError(_textCatalog, errors.front()));
    }
    else
    {
      _validation.set_text("");
    }

    _validation.set_visible(editing && (!current || !errors.empty()));
    _commitButton.set_sensitive(draft().canCommit() && current && !_submitting);
    _editButton.set_sensitive(!_submitting && (_form != nullptr || !_snapshot.trackIds.empty()));
    _addButton.set_sensitive(!_submitting && draft().canEdit());
    _clearButton.set_sensitive(!_submitting);
    _replaceButton.set_sensitive(!_submitting);
    _cancelButton.set_sensitive(!_submitting);

    if (_submitting || _renderQueued)
    {
      retireRowFocus();
    }

    _rowsBox.set_sensitive(!_submitting && !_renderQueued);

    for (auto& rowPtr : _rows)
    {
      rowPtr->name.remove_css_class("error");
      rowPtr->role.remove_css_class("error");
      rowPtr->kind.remove_css_class("error");
    }

    for (auto const& error : errors)
    {
      if (error.rowIndex < _rows.size())
      {
        _rows[error.rowIndex]->validationControl(error.reason).add_css_class("error");
      }
    }
  }

  void TrackCreditsEditor::reportError(std::string const& message)
  {
    if (_notifications != nullptr)
    {
      _notifications->post(rt::NotificationSeverity::Error, message, rt::NotificationLifetime::history());
    }
    else
    {
      _validation.set_text(message);
      _validation.set_visible(true);
    }
  }

  library::CreditKind TrackCreditsEditor::addKind() const
  {
    auto const& model = _form != nullptr ? _form->creditsEditor() : _draft;

    if (model.scope().count() == 1)
    {
      for (std::size_t i = 0; i < library::kCreditKindCount; ++i)
      {
        if (model.scope().test(i))
        {
          return static_cast<library::CreditKind>(i);
        }
      }
    }

    return library::CreditKind::Performer;
  }
} // namespace ao::gtk::layout
