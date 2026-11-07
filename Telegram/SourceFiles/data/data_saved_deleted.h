/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class HistoryItem;
class PeerData;

namespace Window {
class SessionController;
} // namespace Window

// Messages deleted by others and previous versions of edited messages
// are kept on this computer: the deleted ones stay in the chat with
// a mark until a restart and can be seen in a box after it.
// Self-destructing and view-once content is never kept.
namespace Data::SavedDeleted {

// Reads the settings, call once at the start. `folder` is tdata.
void LoadSettings(const QString &folder);
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

[[nodiscard]] bool ShouldKeep(not_null<HistoryItem*> item);

// Saves the message and marks it as deleted for this run.
void Keep(not_null<HistoryItem*> item);
[[nodiscard]] bool IsMarked(not_null<const HistoryItem*> item);

void RememberEdit(
	not_null<HistoryItem*> item,
	const QString &previousText,
	TimeId previousDate);

struct Entry {
	MsgId id = 0;
	TimeId date = 0;
	TimeId changed = 0;
	QString author;
	QString text;
	uint64 fromId = 0;
	bool out = false;
};

// Puts the kept deleted messages back into a slice loaded from the server,
// so they stay in the chat after a restart, at their place, with a mark.
[[nodiscard]] QVector<MTPMessage> WithDeleted(
	not_null<History*> history,
	const QVector<MTPMessage> &slice);

[[nodiscard]] std::vector<Entry> Deleted(not_null<PeerData*> peer);
[[nodiscard]] bool HasDeleted(not_null<PeerData*> peer);
[[nodiscard]] std::vector<Entry> Edits(not_null<HistoryItem*> item);
[[nodiscard]] bool HasEdits(not_null<HistoryItem*> item);

void ShowDeletedBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);
void ShowEditsBox(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Data::SavedDeleted
