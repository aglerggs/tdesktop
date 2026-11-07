/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

// aglergram: bookmarks, all drafts, the command palette, focus profiles
// and locking when the monitors change.
namespace Window::Liquid {

// Ctrl+Shift+K palette and the monitor lock, call when a window is ready.
void StartExtras(not_null<SessionController*> controller);

// Bookmarks: a star on a message, all of them in one box.
[[nodiscard]] bool IsBookmarked(not_null<HistoryItem*> item);
void ToggleBookmark(
	not_null<SessionController*> controller,
	not_null<HistoryItem*> item);
void ShowBookmarksBox(not_null<SessionController*> controller);

// Unsent drafts of every chat.
void ShowAllDraftsBox(not_null<SessionController*> controller);

// Ctrl+Shift+K (Ctrl+K is a link in the field): jump to a chat, a setting or a feature by typing.
void ShowCommandPalette(not_null<SessionController*> controller);

// Focus profiles: only the chosen chats notify, a folder shows them.
void ShowFocusBox(not_null<SessionController*> controller);
[[nodiscard]] bool FocusBlocksNotification(not_null<History*> history);

// Lock with the local passcode when a monitor is added or removed.
[[nodiscard]] bool LockOnScreensChange();
void SetLockOnScreensChange(bool enabled);

} // namespace Window::Liquid
