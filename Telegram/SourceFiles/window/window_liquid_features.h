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

namespace Data {
class Thread;
} // namespace Data

namespace Ui {
class PopupMenu;
class InputField;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Liquid glass fork: features that are not in Telegram Desktop.
namespace Window::Liquid {

// Reminders and the night theme by sunset, call when a window is ready.
void Start(not_null<SessionController*> controller);

// Peer menu.
void ShowNoteBox(
	not_null<SessionController*> controller,
	not_null<PeerData*> peer);
void ShowStatsBox(
	not_null<SessionController*> controller,
	not_null<History*> history);
void ShowPhotoTextSearch(
	not_null<SessionController*> controller,
	not_null<History*> history);
void OpenSideBySide(
	not_null<SessionController*> controller,
	not_null<Data::Thread*> thread);

// Message menu: reminders, a quote as a picture, downloads via the bot.
void FillMessageMenu(
	not_null<Ui::PopupMenu*> menu,
	not_null<SessionController*> controller,
	HistoryItem *item,
	std::vector<not_null<HistoryItem*>> selected,
	const QString &link);

// Message field: `;name` templates and translating with Ctrl+Shift+T.
void SetupFieldFeatures(
	not_null<Ui::InputField*> field,
	Fn<SessionController*()> controller);

// Settings boxes.
void ShowTemplatesBox(not_null<SessionController*> controller);
void ShowSunsetBox(not_null<SessionController*> controller);

} // namespace Window::Liquid
