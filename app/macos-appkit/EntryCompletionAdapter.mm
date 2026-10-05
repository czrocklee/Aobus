// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EntryCompletionAdapter.h"

#include "AppKitText.h"
#include "NativeCallback.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
  using ao::appkit::nativeCallback;
  using ao::appkit::nativeText;

  constexpr CGFloat kCompletionPopoverWidth = 320.0;
  constexpr CGFloat kCompletionRowHeight = 26.0;
  constexpr std::size_t kCompletionMaxVisibleRows = 8;
  constexpr CGFloat kCompletionTitleFontSize = 13;
  constexpr CGFloat kCompletionDetailFontSize = 11;
  constexpr CGFloat kCompletionHorizontalInset = 10;

  // Caret and selection commands dismiss the visible popover and keep the
  // field editor's ordinary behavior; a live text selection never completes.
  bool isCaretOrSelectionCommand(SEL command)
  {
    return command == @selector(moveLeft:) || command == @selector(moveRight:) ||
           command == @selector(moveToBeginningOfLine:) || command == @selector(moveToEndOfLine:) ||
           command == @selector(moveToBeginningOfParagraph:) || command == @selector(moveToEndOfParagraph:) ||
           command == @selector(moveLeftAndModifySelection:) || command == @selector(moveRightAndModifySelection:) ||
           command == @selector(moveToBeginningOfLineAndModifySelection:) ||
           command == @selector(moveToEndOfLineAndModifySelection:) ||
           command == @selector(moveToBeginningOfParagraphAndModifySelection:) ||
           command == @selector(moveToEndOfParagraphAndModifySelection:) || command == @selector(selectAll:);
  }

  // Byte replacement spans map exactly to UTF-16 field-editor ranges through
  // lossless Foundation substrings; a boundary that would split a surrogate
  // pair or a UTF-8 scalar is rejected instead of mapped approximately.
  std::optional<std::size_t> utf16IndexToByteOffset(NSString* text, NSUInteger index)
  {
    if (index > text.length)
    {
      return std::nullopt;
    }

    if (index > 0 && index < text.length)
    {
      if (unichar const previous = [text characterAtIndex:index - 1];
          CFStringIsSurrogateHighCharacter(previous) != 0 &&
          CFStringIsSurrogateLowCharacter([text characterAtIndex:index]) != 0)
      {
        return std::nullopt;
      }
    }

    NSString* const prefix = [text substringToIndex:index];
    return static_cast<std::size_t>([prefix lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
  }

  std::optional<NSUInteger> byteOffsetToUtf16Index(std::string_view text, std::size_t byteOffset)
  {
    if (byteOffset > text.size())
    {
      return std::nullopt;
    }

    NSString* const prefix = [[NSString alloc] initWithBytes:text.data()
                                                      length:byteOffset
                                                    encoding:NSUTF8StringEncoding];

    if (prefix == nil)
    {
      return std::nullopt;
    }

    return prefix.length;
  }

  std::size_t wrappedRow(std::ptrdiff_t current, std::ptrdiff_t delta, std::size_t itemCount)
  {
    auto const count = static_cast<std::ptrdiff_t>(itemCount);
    auto next = (current + delta) % count;

    if (next < 0)
    {
      next += count;
    }

    return static_cast<std::size_t>(next);
  }
} // namespace

// TU-local table adapter: renders the controller's transient CompletionItem
// rows and forwards popover close and double-click acceptance. Its
// back-pointer is retired before the controller tears down, so no callback
// can reach a destroyed controller.
@interface AobusCompletionTableAdapter : NSObject<NSTableViewDataSource, NSTableViewDelegate, NSPopoverDelegate>
- (instancetype)initWithController:(ao::appkit::EntryCompletionAdapter*)controller
                             items:(std::vector<ao::rt::CompletionItem> const*)items
                           catalog:(ao::i18n::MessageCatalog)catalog;
- (void)retire;
- (void)observeEditor:(NSTextView*)editor;
- (void)stopObserving;
@end

@implementation AobusCompletionTableAdapter {
  ao::appkit::EntryCompletionAdapter* _controller;
  std::vector<ao::rt::CompletionItem> const* _items;
  std::optional<ao::i18n::MessageCatalog> _optCatalog;
}

- (instancetype)initWithController:(ao::appkit::EntryCompletionAdapter*)controller
                             items:(std::vector<ao::rt::CompletionItem> const*)items
                           catalog:(ao::i18n::MessageCatalog)catalog
{
  self = [super init];

  if (self != nil)
  {
    _controller = controller;
    _items = items;
    _optCatalog.emplace(std::move(catalog));
  }

  return self;
}

- (void)retire
{
  [self stopObserving];
  _controller = nullptr;
  _items = nullptr;
}

- (void)observeEditor:(NSTextView*)editor
{
  [self stopObserving];
  auto* const center = NSNotificationCenter.defaultCenter;
  [center addObserver:self
             selector:@selector(editorSelectionDidChange:)
                 name:NSTextViewDidChangeSelectionNotification
               object:editor];
  [center addObserver:self
             selector:@selector(applicationDidResignActive:)
                 name:NSApplicationDidResignActiveNotification
               object:NSApp];
}

- (void)stopObserving
{
  [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)editorSelectionDidChange:(NSNotification*) [[maybe_unused]] notification
{
  nativeCallback(
    [&]
    {
      if (_controller != nullptr)
      {
        _controller->handleEditorSelectionChange();
      }
    });
}

- (void)applicationDidResignActive:(NSNotification*) [[maybe_unused]] notification
{
  nativeCallback(
    [&]
    {
      if (_controller != nullptr)
      {
        _controller->dismiss();
      }
    });
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*) [[maybe_unused]] tableView
{
  return _items == nullptr ? 0 : static_cast<NSInteger>(_items->size());
}

- (NSView*)tableView:(NSTableView*)tableView
  viewForTableColumn:(NSTableColumn*) [[maybe_unused]] column
                 row:(NSInteger)row
{
  NSView* cell = nil;
  nativeCallback(
    [&]
    {
      if (_items == nullptr || row < 0 || static_cast<std::size_t>(row) >= _items->size())
      {
        return;
      }

      auto const& item = (*_items)[static_cast<std::size_t>(row)];
      cell = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, tableView.bounds.size.width, kCompletionRowHeight)];
      cell.identifier = @"completion-cell";

      auto* const title = [NSTextField labelWithString:nativeText(item.displayText)];
      title.font = [NSFont systemFontOfSize:kCompletionTitleFontSize];
      title.lineBreakMode = NSLineBreakByTruncatingTail;
      title.maximumNumberOfLines = 1;
      title.translatesAutoresizingMaskIntoConstraints = NO;
      [cell addSubview:title];

      auto const detailText = ao::uimodel::completionDetail(*_optCatalog, item.detail);
      auto* const detail = [NSTextField labelWithString:nativeText(detailText)];
      detail.font = [NSFont systemFontOfSize:kCompletionDetailFontSize];
      detail.textColor = NSColor.secondaryLabelColor;
      detail.lineBreakMode = NSLineBreakByTruncatingTail;
      detail.maximumNumberOfLines = 1;
      detail.translatesAutoresizingMaskIntoConstraints = NO;
      [cell addSubview:detail];
      detail.hidden = static_cast<BOOL>(detailText.empty());
      [detail setContentHuggingPriority:NSLayoutPriorityRequired
                         forOrientation:NSLayoutConstraintOrientationHorizontal];
      [detail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                       forOrientation:NSLayoutConstraintOrientationHorizontal];
      [title setContentHuggingPriority:NSLayoutPriorityDefaultLow
                        forOrientation:NSLayoutConstraintOrientationHorizontal];

      [NSLayoutConstraint activateConstraints:@[
        [title.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:kCompletionHorizontalInset],
        [title.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        [detail.leadingAnchor constraintGreaterThanOrEqualToAnchor:title.trailingAnchor constant:8],
        [detail.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-kCompletionHorizontalInset],
        [detail.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor]
      ]];
    });
  return cell;
}

- (void)acceptCompletion:(id) [[maybe_unused]] sender
{
  nativeCallback(
    [&]
    {
      if (_controller != nullptr)
      {
        std::ignore = _controller->tryApplySelected();
      }
    });
}

- (void)popoverDidClose:(NSNotification*) [[maybe_unused]] notification
{
  nativeCallback(
    [&]
    {
      if (_controller != nullptr)
      {
        // Idempotent: mirrors the external transient close into cleared state.
        _controller->dismiss();
      }
    });
}
@end

namespace ao::appkit
{
  EntryCompletionAdapter::EntryCompletionAdapter(NSTextField* field,
                                                 i18n::MessageCatalog textCatalog,
                                                 rt::CompletionProvider provider,
                                                 EntryCompletionOptions options)
    : _field{field}, _textCatalog{std::move(textCatalog)}, _provider{std::move(provider)}, _options{options}
  {
  }

  EntryCompletionAdapter::~EntryCompletionAdapter()
  {
    detach();
  }

  void EntryCompletionAdapter::update()
  {
    if (_detached)
    {
      return;
    }

    auto* const editor = activeEditor();

    if (editor == nil || [editor hasMarkedText] != NO)
    {
      // No completion while the field is not editing or IME composition owns the text.
      dismiss();
      return;
    }

    auto* const text = [editor string];
    auto const selected = editor.selectedRange;

    if (selected.length != 0)
    {
      // A live text selection never completes; only a plain caret maps safely.
      dismiss();
      return;
    }

    // Foundation reports a failed conversion as zero bytes; canBeConvertedToEncoding:
    // still accepts a lone surrogate.
    if (text.length != 0 && [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] == 0)
    {
      // A lone surrogate has no UTF-8 byte offset, so no provider span can map back.
      dismiss();
      return;
    }

    auto const optCursorBytes = utf16IndexToByteOffset(text, selected.location);

    if (!optCursorBytes)
    {
      dismiss();
      return;
    }

    auto const utf8Text = utf8(text);

    if (_hasSuppression && utf8Text == _suppressedText)
    {
      // The acceptance's own text-change notification must not reopen the
      // popover for the state it just produced; any different text clears the
      // suppression and queries the provider again.
      dismiss();
      return;
    }

    _hasSuppression = false;
    auto optResult = _provider(utf8Text, *optCursorBytes);

    if (!optResult || optResult->items.empty() || optResult->replaceBegin > optResult->replaceEnd ||
        optResult->replaceEnd > utf8Text.size())
    {
      dismiss();
      return;
    }

    // The advertised byte span must map exactly onto UTF-16 field-editor units.
    auto const optBeginUtf16 = byteOffsetToUtf16Index(utf8Text, optResult->replaceBegin);
    auto const optEndUtf16 = byteOffsetToUtf16Index(utf8Text, optResult->replaceEnd);

    if (!optBeginUtf16 || !optEndUtf16)
    {
      dismiss();
      return;
    }

    _sourceText = utf8Text;
    _sourceCursorUtf16 = selected.location;
    _replaceBegin = optResult->replaceBegin;
    _replaceEnd = optResult->replaceEnd;
    _replacementRange = NSMakeRange(*optBeginUtf16, *optEndUtf16 - *optBeginUtf16);
    _items = std::move(optResult->items);
    _hasReplacement = true;

    ensureNativeViews();
    [_tableView reloadData];
    auto const visibleRows = static_cast<CGFloat>(std::clamp(_items.size(), std::size_t{1}, kCompletionMaxVisibleRows));
    [_popover setContentSize:NSMakeSize(kCompletionPopoverWidth, visibleRows * kCompletionRowHeight)];
    [_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [_tableView scrollRowToVisible:0];

    if (_popover.shown == NO)
    {
      [_popover showRelativeToRect:_field.bounds ofView:_field preferredEdge:NSMinYEdge];
      [static_cast<AobusCompletionTableAdapter*>(_tableAdapter) observeEditor:editor];
    }
  }

  void EntryCompletionAdapter::dismiss()
  {
    if (_detached)
    {
      return;
    }

    clearCompletionState();
    [static_cast<AobusCompletionTableAdapter*>(_tableAdapter) stopObserving];

    if (_popover.shown != NO)
    {
      [_popover close];
    }
  }

  bool EntryCompletionAdapter::tryApplySelected()
  {
    if (_detached || !_hasReplacement || _items.empty())
    {
      dismiss();
      return false;
    }

    auto* const editor = activeEditor();

    if (editor == nil || [editor hasMarkedText] != NO)
    {
      dismiss();
      return false;
    }

    auto const selected = editor.selectedRange;

    if (selected.length != 0)
    {
      // A live text selection never completes; only a plain caret maps safely.
      dismiss();
      return false;
    }

    if (auto const currentText = utf8([editor string]);
        currentText != _sourceText || selected.location != _sourceCursorUtf16)
    {
      // The source moved under the popover; never apply a stale span.
      dismiss();
      return false;
    }

    auto const row = _tableView.selectedRow;
    auto const index = row < 0 ? std::size_t{0} : static_cast<std::size_t>(row);

    if (index >= _items.size())
    {
      dismiss();
      return false;
    }

    auto const& item = _items[index];
    auto* const insertNative = [[NSString alloc] initWithBytes:item.insertText.data()
                                                        length:item.insertText.size()
                                                      encoding:NSUTF8StringEncoding];

    if (insertNative == nil)
    {
      // Provider insert text must be valid UTF-8; never replace with a lossy stand-in.
      dismiss();
      return false;
    }

    // Copy every native replacement input into strong locals, settle the
    // accepted state, and dismiss before mutating the editor: the change
    // callbacks fire synchronously and may dismiss or detach this controller.
    auto const replacementRange = _replacementRange;
    auto const newCursorUtf16 = replacementRange.location + insertNative.length;
    auto newText = _sourceText;
    newText.replace(_replaceBegin, _replaceEnd - _replaceBegin, item.insertText);
    _suppressedText = std::move(newText);
    _hasSuppression = true;
    dismiss();

    // Strong locals only below; `this` is not touched after the mutation.
    // The field editor publishes through the host's normal text-change path.
    [editor insertText:insertNative replacementRange:replacementRange];
    [editor setSelectedRange:NSMakeRange(newCursorUtf16, 0)];
    return true;
  }

  bool EntryCompletionAdapter::tryHandleCommand(SEL command)
  {
    if (_detached)
    {
      return false;
    }

    auto const caretCommand = isCaretOrSelectionCommand(command);

    if (_popover.shown == NO)
    {
      if (caretCommand)
      {
        dismiss();
      }

      return false;
    }

    if (command == @selector(moveUp:))
    {
      return tryMoveSelection(-1);
    }

    if (command == @selector(moveDown:))
    {
      return tryMoveSelection(1);
    }

    if (command == @selector(pageUp:) || command == @selector(scrollPageUp:))
    {
      return tryMovePageSelection(-1);
    }

    if (command == @selector(pageDown:) || command == @selector(scrollPageDown:))
    {
      return tryMovePageSelection(1);
    }

    // A stale source only dismisses, so the key keeps its ordinary meaning.
    if (command == @selector(insertTab:))
    {
      return tryApplySelected();
    }

    if (command == @selector(insertNewline:))
    {
      if (_options.acceptOnReturn)
      {
        return tryApplySelected();
      }

      dismiss();
      return false;
    }

    if (command == @selector(cancelOperation:))
    {
      dismiss();
      return true;
    }

    if (caretCommand)
    {
      // Caret movement dismisses and keeps its ordinary field behavior.
      dismiss();
      return false;
    }

    return false;
  }

  void EntryCompletionAdapter::handleEditorSelectionChange()
  {
    if (_detached || !_hasReplacement)
    {
      return;
    }

    auto* const editor = activeEditor();

    if (editor == nil)
    {
      dismiss();
      return;
    }

    if (auto const selected = editor.selectedRange; selected.length == 0 && selected.location == _sourceCursorUtf16)
    {
      return;
    }

    // Typing moves the caret too; the host's text-change path refreshes that state.
    if (utf8([editor string]) != _sourceText)
    {
      return;
    }

    dismiss();
  }

  void EntryCompletionAdapter::detach()
  {
    if (_detached)
    {
      return;
    }

    _detached = true;
    // Retire the adapter before closing so teardown callbacks cannot reach this controller.
    [static_cast<AobusCompletionTableAdapter*>(_tableAdapter) retire];
    clearCompletionState();

    if (_popover.shown != NO)
    {
      [_popover close];
    }
  }

  void EntryCompletionAdapter::ensureNativeViews()
  {
    if (_popover != nil)
    {
      return;
    }

    _tableAdapter = [[AobusCompletionTableAdapter alloc] initWithController:this items:&_items catalog:_textCatalog];

    _tableView = [[NSTableView alloc] initWithFrame:NSZeroRect];
    auto* const column = [[NSTableColumn alloc] initWithIdentifier:@"completion"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [_tableView addTableColumn:column];
    _tableView.headerView = nil;
    _tableView.style = NSTableViewStylePlain;
    _tableView.identifier = @"completion-list";
    _tableView.rowHeight = kCompletionRowHeight;
    _tableView.refusesFirstResponder = YES;
    _tableView.dataSource = _tableAdapter;
    _tableView.delegate = _tableAdapter;
    _tableView.target = _tableAdapter;
    _tableView.doubleAction = @selector(acceptCompletion:);

    _scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, kCompletionPopoverWidth, kCompletionRowHeight)];
    _scrollView.documentView = _tableView;
    _scrollView.hasVerticalScroller = YES;
    _scrollView.hasHorizontalScroller = NO;
    _scrollView.autohidesScrollers = YES;

    auto* const content = [[NSViewController alloc] init];
    content.view = _scrollView;

    _popover = [[NSPopover alloc] init];
    // Application-defined keeps keyboard focus in the borrowed field editor;
    // the host dismisses on focus end and busy or stale refresh, and this
    // adapter on caret movement and application deactivation.
    _popover.behavior = NSPopoverBehaviorApplicationDefined;
    _popover.delegate = _tableAdapter;
    _popover.contentViewController = content;
    [_popover setContentSize:NSMakeSize(kCompletionPopoverWidth, kCompletionRowHeight)];
  }

  NSTextView* EntryCompletionAdapter::activeEditor() const
  {
    if (_field == nil || _field.window == nil || _field.enabled == NO || _field.editable == NO)
    {
      return nil;
    }

    auto* const editor = _field.currentEditor;

    if (editor == nil || [editor isKindOfClass:NSTextView.class] == NO)
    {
      return nil;
    }

    return static_cast<NSTextView*>(editor);
  }

  void EntryCompletionAdapter::clearCompletionState()
  {
    _hasReplacement = false;
    _sourceText.clear();

    if (!_items.empty())
    {
      _items.clear();
      [_tableView reloadData];
    }
  }

  bool EntryCompletionAdapter::tryMovePageSelection(std::ptrdiff_t const direction)
  {
    if (_items.empty())
    {
      return false;
    }

    // The shared list semantics move by the visible rows and stop at the
    // boundary instead of wrapping, like the popover's visible page.
    auto const visibleRows =
      static_cast<std::ptrdiff_t>(std::clamp(_items.size(), std::size_t{1}, kCompletionMaxVisibleRows));
    auto const row = _tableView.selectedRow;
    auto const current = row < 0 ? std::ptrdiff_t{0} : static_cast<std::ptrdiff_t>(row);
    auto const next = std::clamp(
      current + (direction * visibleRows), std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(_items.size()) - 1);

    [_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(next)] byExtendingSelection:NO];
    [_tableView scrollRowToVisible:static_cast<NSInteger>(next)];
    return true;
  }

  bool EntryCompletionAdapter::tryMoveSelection(std::ptrdiff_t const delta)
  {
    if (_items.empty())
    {
      return false;
    }

    auto const row = _tableView.selectedRow;
    auto const current = row < 0 ? std::size_t{0} : static_cast<std::size_t>(row);
    auto const next = wrappedRow(static_cast<std::ptrdiff_t>(current), delta, _items.size());

    [_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(next)] byExtendingSelection:NO];
    [_tableView scrollRowToVisible:static_cast<NSInteger>(next)];
    return true;
  }
} // namespace ao::appkit
