// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TrackCreditsEditor.h"

#include "AppKitText.h"
#include "EntryCompletionAdapter.h"
#include "LibraryEditorModel.h"
#include "NativeCallback.h"
#include <ao/rt/completion/MetadataValueCompleter.h>
#include <ao/uimodel/library/detail/TrackCredits.h>

#include <cstddef>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace
{
  using ao::appkit::nativeCallback;
  using ao::appkit::nativeText;
  using ao::i18n::MessageId;
  using ao::library::CreditKind;

  NSStackView* verticalStack()
  {
    auto* const stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    return stack;
  }
} // namespace

@implementation AobusTrackCreditsEditor {
  ao::appkit::LibraryEditorModel* _model;
  ao::rt::CompletionService* _completion;
  void (^_changed)(void);
  NSStackView* _stack;
  NSMutableArray<NSTextField*>* _fields;
  NSMutableArray<NSPopUpButton*>* _kindControls;
  NSMutableArray<NSTextField*>* _errors;
  NSMutableArray<NSControl*>* _editControls;
  NSButton* _save;
  NSButton* _cancel;
  NSButton* _replace;
  NSButton* _clear;
  BOOL _blocked;
  std::vector<std::unique_ptr<ao::appkit::EntryCompletionAdapter>> _completions;
}

- (instancetype)initWithModel:(ao::appkit::LibraryEditorModel&)model
                   completion:(ao::rt::CompletionService&)completion
                      changed:(void (^)(void))changed
{
  self = [super initWithNibName:nil bundle:nil];

  if (self != nil)
  {
    _model = &model;
    _completion = &completion;
    _changed = [changed copy];
    _stack = verticalStack();
    self.view = _stack;
    [self rebuild];
  }

  return self;
}

- (NSString*)text:(MessageId)message
{
  return ao::appkit::catalogText(_model->catalog(), message);
}

- (NSButton*)button:(MessageId)message action:(SEL)action identifier:(NSString*)identifier
{
  auto* const button = [NSButton buttonWithTitle:[self text:message] target:self action:action];
  button.identifier = identifier;
  button.bezelStyle = NSBezelStyleRounded;
  return button;
}

- (void)detachCompletions
{
  for (auto const& controllerPtr : _completions)
  {
    controllerPtr->detach();
  }

  _completions.clear();
}

- (void)retireControls
{
  for (NSUInteger index = 0; index < _fields.count; ++index)
  {
    auto* const field = _fields[index];
    field.delegate = nil;
  }

  for (NSUInteger index = 0; index < _editControls.count; ++index)
  {
    auto* const control = _editControls[index];
    control.target = nil;
    control.action = nullptr;
    control.enabled = NO;
  }

  {
    NSArray<NSButton*>* const buttons = @[_save, _cancel, _replace, _clear];

    for (NSUInteger index = 0; index < buttons.count; ++index)
    {
      auto* const button = buttons[index];
      button.target = nil;
      button.action = nullptr;
      button.enabled = NO;
    }
  }

  [_fields removeAllObjects];
  [_kindControls removeAllObjects];
  [_editControls removeAllObjects];
  _save = nil;
  _cancel = nil;
  _replace = nil;
  _clear = nil;
  [self detachCompletions];
}

- (BOOL)isCurrentEditControl:(NSControl*)control
{
  return static_cast<BOOL>(control != nil && [_editControls indexOfObjectIdenticalTo:control] != NSNotFound);
}

- (BOOL)isCurrentField:(id)field
{
  return static_cast<BOOL>(field != nil && [_fields indexOfObjectIdenticalTo:field] != NSNotFound);
}

- (void)rebuild
{
  if (_save != nil)
  {
    // Revoke admission before resigning focus can deliver a final delegate callback.
    [self retireControls];
  }

  [self.view.window makeFirstResponder:nil];

  {
    auto* const subviews = static_cast<NSArray<NSView*>*>([_stack.arrangedSubviews copy]);

    for (NSUInteger index = 0; index < subviews.count; ++index)
    {
      auto* const view = subviews[index];
      [_stack removeArrangedSubview:view];
      [view removeFromSuperview];
    }
  }

  _fields = [NSMutableArray array];
  _kindControls = [NSMutableArray array];
  _errors = [NSMutableArray array];
  _editControls = [NSMutableArray array];
  auto const& editor = _model->creditsEditor();
  auto const scope = editor.scope();
  auto const scopeName = ao::uimodel::trackCreditScopeLabel(_model->catalog(), scope);

  auto* const scopeLabel =
    [NSTextField wrappingLabelWithString:ao::appkit::catalogFormat(
                                           _model->catalog(), MessageId::TrackCreditsScope, {{"scope", scopeName}})];
  scopeLabel.identifier = @"credits-scope";
  [_stack addArrangedSubview:scopeLabel];
  [_stack addArrangedSubview:[NSTextField wrappingLabelWithString:ao::appkit::catalogFormat(
                                                                    _model->catalog(),
                                                                    MessageId::AppKitEditTracks,
                                                                    {{"count", _model->state().trackIds.size()}})]];

  if (editor.isMixedReplacement())
  {
    [_stack addArrangedSubview:[NSTextField wrappingLabelWithString:[self text:MessageId::TrackMultipleValues]]];
  }

  _replace = [self button:MessageId::TrackCreditsReplaceScope action:@selector(replace:) identifier:@"credits-replace"];
  [_stack addArrangedSubview:_replace];
  [_replace.widthAnchor constraintLessThanOrEqualToAnchor:_stack.widthAnchor].active = YES;
  _replace.cell.wraps = YES;
  _clear = [self button:MessageId::TrackCreditsClear action:@selector(clear:) identifier:@"credits-clear"];
  [_stack addArrangedSubview:_clear];

  for (std::size_t index = 0; index < editor.entries().size(); ++index)
  {
    auto const& entry = editor.entries()[index];
    auto* const row = verticalStack();
    [_stack addArrangedSubview:row];
    [row.widthAnchor constraintEqualToAnchor:_stack.widthAnchor].active = YES;
    auto* const kind = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];

    for (std::size_t kindIndex = 0; kindIndex < ao::library::kCreditKindCount; ++kindIndex)
    {
      [kind addItemWithTitle:nativeText(ao::uimodel::trackCreditKindLabel(
                               _model->catalog(), static_cast<CreditKind>(kindIndex)))];
    }

    [kind selectItemAtIndex:static_cast<NSInteger>(entry.kind)];
    kind.tag = static_cast<NSInteger>(index);
    kind.target = self;
    kind.action = @selector(changeKind:);
    kind.identifier = nativeText(std::format("credit-kind-{}", index));
    kind.accessibilityLabel = [self text:MessageId::TrackCreditKind];
    kind.hidden = static_cast<BOOL>(!scope.all());
    [row addArrangedSubview:kind];
    [_editControls addObject:kind];
    [_kindControls addObject:kind];

    for (std::size_t column = 0; column < 2; ++column)
    {
      auto const message = column == 0 ? MessageId::TrackCreditName : MessageId::TrackCreditRole;
      auto* const label = [NSTextField labelWithString:[self text:message]];
      [row addArrangedSubview:label];
      auto* const field = [NSTextField textFieldWithString:nativeText(column == 0 ? entry.name : entry.role)];
      field.identifier = nativeText(std::format("credit-{}-{}", column == 0 ? "name" : "role", index));
      field.accessibilityLabel = label.stringValue;
      field.tag = static_cast<NSInteger>((index * 2) + column);
      field.delegate = self;
      [row addArrangedSubview:field];
      [field.widthAnchor constraintEqualToAnchor:row.widthAnchor].active = YES;
      [_fields addObject:field];
      [_editControls addObject:field];
      _completions.push_back(std::make_unique<ao::appkit::EntryCompletionAdapter>(
        field,
        _model->catalog(),
        column == 0 ? ao::rt::makeCreditNameCompletionProvider(*_completion, entry.kind)
                    : ao::rt::makeCreditRoleCompletionProvider(*_completion)));
    }

    auto* const error = [NSTextField wrappingLabelWithString:@""];
    error.identifier = nativeText(std::format("credit-error-{}", index));
    error.textColor = NSColor.systemRedColor;
    [row addArrangedSubview:error];
    [error.widthAnchor constraintEqualToAnchor:row.widthAnchor].active = YES;
    [_errors addObject:error];
    auto* const actions = [NSStackView stackViewWithViews:@[]];
    actions.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    for (auto const message :
         {MessageId::TrackCreditMoveUp, MessageId::TrackCreditMoveDown, MessageId::TrackCreditDelete})
    {
      auto* action = @selector(delete:);
      auto const* actionName = "delete";

      if (message == MessageId::TrackCreditMoveUp)
      {
        action = @selector(moveUp:);
        actionName = "up";
      }
      else if (message == MessageId::TrackCreditMoveDown)
      {
        action = @selector(moveDown:);
        actionName = "down";
      }

      auto* const button = [self button:message
                                 action:action
                             identifier:nativeText(std::format("credit-{}-{}", actionName, index))];
      button.tag = static_cast<NSInteger>(index);
      [actions addArrangedSubview:button];
      [_editControls addObject:button];
    }

    [row addArrangedSubview:actions];
  }

  auto* const add = [self button:MessageId::TrackCreditsAdd action:@selector(add:) identifier:@"credits-add"];
  [_stack addArrangedSubview:add];
  [_editControls addObject:add];
  _save = [self button:MessageId::TrackCreditsCommit action:@selector(save:) identifier:@"credits-save"];
  auto* const cancel = [self button:MessageId::TrackCreditsCancel
                             action:@selector(cancel:)
                         identifier:@"credits-cancel"];
  _cancel = cancel;
  _save.keyEquivalent = @"\r";
  _save.keyEquivalentModifierMask = 0;
  cancel.keyEquivalent = @"\033";
  cancel.keyEquivalentModifierMask = 0;
  auto* const actions = [NSStackView stackViewWithViews:@[cancel, _save]];
  [_stack addArrangedSubview:actions];
  [self refresh];
}

- (BOOL)canEdit
{
  return static_cast<BOOL>(_model != nullptr && _blocked == NO && !_model->state().busy && !_model->state().stale);
}

- (void)refreshWithBlocked:(BOOL)blocked
{
  _blocked = blocked;
  [self refresh];
}

- (NSControl*)controlForError:(ao::uimodel::TrackCreditValidationError const&)error
{
  if (error.rowIndex >= _errors.count)
  {
    return nil;
  }

  switch (error.reason)
  {
    case ao::uimodel::TrackCreditValidationReason::BlankName:
    case ao::uimodel::TrackCreditValidationReason::InvalidNameText: return _fields[error.rowIndex * 2];
    case ao::uimodel::TrackCreditValidationReason::InvalidRoleText: return _fields[(error.rowIndex * 2) + 1];
    case ao::uimodel::TrackCreditValidationReason::InvalidKind: return _kindControls[error.rowIndex];
  }

  return nil;
}

- (void)refresh
{
  if (_model == nullptr)
  {
    return;
  }

  auto const& editor = _model->creditsEditor();
  auto const enabled = [self canEdit] != NO;
  _replace.hidden = static_cast<BOOL>(!editor.isMixedReplacement() || editor.canEdit());
  _replace.enabled = static_cast<BOOL>(enabled);
  _clear.enabled = static_cast<BOOL>(enabled);
  _save.enabled = static_cast<BOOL>(enabled && editor.canCommit());
  _cancel.enabled = static_cast<BOOL>(_blocked == NO && !_model->state().busy);

  for (NSUInteger controlIndex = 0; controlIndex < _editControls.count; ++controlIndex)
  {
    auto* const control = _editControls[controlIndex];
    bool canUse = enabled && editor.canEdit();

    if (auto const index = static_cast<std::size_t>(control.tag); control.action == @selector(moveUp:))
    {
      canUse = canUse && index > 0 && editor.entries()[index - 1].kind == editor.entries()[index].kind;
    }
    else if (control.action == @selector(moveDown:))
    {
      canUse = canUse && index + 1 < editor.entries().size() &&
               editor.entries()[index + 1].kind == editor.entries()[index].kind;
    }

    control.enabled = static_cast<BOOL>(canUse);
  }

  for (NSUInteger index = 0; index < _fields.count; ++index)
  {
    auto* const field = _fields[index];
    field.editable = static_cast<BOOL>(enabled && editor.canEdit());
    field.accessibilityHelp = nil;
  }

  for (NSUInteger index = 0; index < _kindControls.count; ++index)
  {
    _kindControls[index].accessibilityHelp = nil;
  }

  for (NSUInteger index = 0; index < _errors.count; ++index)
  {
    auto* const error = _errors[index];
    error.stringValue = @"";
    error.hidden = YES;
  }

  for (auto const& error : editor.validationErrors())
  {
    if (error.rowIndex < _errors.count)
    {
      auto* const label = _errors[error.rowIndex];
      label.stringValue = nativeText(ao::uimodel::formatTrackCreditValidationError(_model->catalog(), error));
      label.hidden = NO;
      [self controlForError:error].accessibilityHelp = label.stringValue;
    }
  }

  if (!enabled)
  {
    for (auto const& controllerPtr : _completions)
    {
      controllerPtr->dismiss();
    }
  }
}

- (void)focusDraftRow
{
  if (_model == nullptr || [self canEdit] == NO)
  {
    return;
  }

  auto const& editor = _model->creditsEditor();

  if (auto const optRow = editor.focusedRow(); optRow && *optRow * 2 < _fields.count)
  {
    NSControl* control = _fields[*optRow * 2];

    for (auto const& error : editor.validationErrors())
    {
      if (error.rowIndex == *optRow)
      {
        control = [self controlForError:error];
        break;
      }
    }

    if (control != nil)
    {
      [control scrollRectToVisible:control.bounds];
      [self.view.window makeFirstResponder:control];
    }
  }
}

- (void)changedStructure
{
  // Structural callbacks retain their controller and sender through this synchronous render.
  [self rebuild];
  _changed();
  [self focusDraftRow];
}

- (void)replace:(id)sender
{
  nativeCallback(
    [self, sender]
    {
      if ([self canEdit] && sender == _replace)
      {
        _model->creditsEditor().beginReplacement();
        [self changedStructure];
      }
    });
}

- (void)clear:(id)sender
{
  nativeCallback(
    [self, sender]
    {
      if ([self canEdit] && sender == _clear)
      {
        _model->creditsEditor().clearScope();
        [self changedStructure];
      }
    });
}

- (void)add:(NSControl*)sender
{
  nativeCallback(
    [self, sender]
    {
      if (![self canEdit] || ![self isCurrentEditControl:sender])
      {
        return;
      }

      auto const scope = _model->creditsEditor().scope();
      auto kind = CreditKind::Performer;

      for (std::size_t index = 0; index < ao::library::kCreditKindCount; ++index)
      {
        if (scope.count() == 1 && scope.test(index))
        {
          kind = static_cast<CreditKind>(index);
        }
      }

      _model->creditsEditor().addEntry(kind);
      [self changedStructure];
    });
}

- (void)delete:(NSButton*)sender
{
  nativeCallback(
    [self, sender]
    {
      if ([self canEdit] && [self isCurrentEditControl:sender])
      {
        _model->creditsEditor().deleteEntry(static_cast<std::size_t>(sender.tag));
        [self changedStructure];
      }
    });
}

- (void)moveUp:(NSButton*)sender
{
  nativeCallback(
    [self, sender]
    {
      if ([self canEdit] && [self isCurrentEditControl:sender] && sender.tag > 0)
      {
        auto const index = static_cast<std::size_t>(sender.tag);
        _model->creditsEditor().moveEntry(index, index - 1);
        [self changedStructure];
      }
    });
}

- (void)moveDown:(NSButton*)sender
{
  nativeCallback(
    [self, sender]
    {
      if ([self canEdit] && [self isCurrentEditControl:sender])
      {
        auto const index = static_cast<std::size_t>(sender.tag);
        _model->creditsEditor().moveEntry(index, index + 1);
        [self changedStructure];
      }
    });
}

- (void)changeKind:(NSPopUpButton*)sender
{
  nativeCallback(
    [self, sender]
    {
      if ([self canEdit] && [self isCurrentEditControl:sender])
      {
        _model->creditsEditor().changeKind(
          static_cast<std::size_t>(sender.tag), static_cast<CreditKind>(sender.indexOfSelectedItem));
        [self changedStructure];
      }
    });
}

- (void)save:(id)sender
{
  // Acceptance can remove this controller from its parent synchronously.
  nativeCallback(
    [self, sender]
    {
      if (![self canEdit] || sender != _save)
      {
        return;
      }

      if (auto res = _model->acceptCreditsEdit(); res)
      {
        [self detach];
        _changed();
      }
      else
      {
        [self refresh];

        if (_model != nullptr && [self canEdit])
        {
          if (auto const errors = _model->creditsEditor().validationErrors(); !errors.empty())
          {
            _model->creditsEditor().focusRow(errors.front().rowIndex);
            [self focusDraftRow];
          }
        }
      }
    });
}

- (void)cancel:(id)sender
{
  nativeCallback(
    [self, sender]
    {
      if (_model == nullptr || _blocked != NO || sender != _cancel)
      {
        return;
      }

      _model->cancelCreditsEdit();
      [self detach];
      _changed();
    });
}

- (void)controlTextDidChange:(NSNotification*)notification
{
  nativeCallback(
    [&]
    {
      if (![self canEdit] || ![self isCurrentField:notification.object])
      {
        return;
      }

      NSTextField* const field = notification.object;
      auto const index = static_cast<std::size_t>(field.tag);

      if (index % 2 == 0)
      {
        _model->creditsEditor().updateName(index / 2, ao::appkit::utf8(field.stringValue));
      }
      else
      {
        _model->creditsEditor().updateRole(index / 2, ao::appkit::utf8(field.stringValue));
      }

      [self refresh];
      _changed();

      if ([self canEdit] && [self isCurrentField:field])
      {
        _completions[index]->update();
      }
    });
}

- (void)controlTextDidBeginEditing:(NSNotification*)notification
{
  nativeCallback(
    [&]
    {
      if ([self canEdit] && [self isCurrentField:notification.object])
      {
        NSControl* const field = notification.object;
        _model->creditsEditor().focusRow(static_cast<std::size_t>(field.tag) / 2);
      }
    });
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*) [[maybe_unused]] textView doCommandBySelector:(SEL)command
{
  BOOL consumed = NO;
  nativeCallback(
    [&]
    {
      if ([self canEdit] && [self isCurrentField:control])
      {
        consumed = static_cast<BOOL>(_completions[static_cast<std::size_t>(control.tag)]->tryHandleCommand(command));
      }
    });
  return consumed;
}

- (void)controlTextDidEndEditing:(NSNotification*)notification
{
  nativeCallback(
    [&]
    {
      if (_model != nullptr && [self isCurrentField:notification.object])
      {
        NSControl* const field = notification.object;
        _completions[static_cast<std::size_t>(field.tag)]->dismiss();
      }
    });
}

- (void)detach
{
  _model = nullptr;

  if (_save != nil)
  {
    [self retireControls];
  }
}
@end
