/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonValue>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

// Liquid glass fork: small local values of the fork's own features,
// kept in tdata/liquid_store.json on this computer only.
namespace Data::LiquidStore {

// Call once at the start. `folder` is tdata.
void Load(const QString &folder);
[[nodiscard]] QString Folder();

// Private notes about a chat or a person.
[[nodiscard]] QString Note(not_null<Main::Session*> session, PeerId peer);
void SetNote(
	not_null<Main::Session*> session,
	PeerId peer,
	const QString &text);
[[nodiscard]] rpl::producer<QString> NoteValue(
	not_null<Main::Session*> session,
	PeerId peer);

// Reminders about messages.
struct Reminder {
	uint64 userId = 0;
	PeerId peer = 0;
	MsgId msg = 0;
	TimeId at = 0;
	QString title;
	QString preview;
};
void AddReminder(Reminder reminder);
[[nodiscard]] std::vector<Reminder> TakeDueReminders(TimeId now);
[[nodiscard]] int RemindersCount(uint64 userId);

// Text templates, `;name` in a message field expands to the text.
struct Template {
	QString name;
	QString text;
};
[[nodiscard]] const std::vector<Template> &Templates();
void SetTemplates(std::vector<Template> list);

// Night theme by the real sunset.
struct Sunset {
	bool enabled = false;
	float64 latitude = 47.22;
	float64 longitude = 39.72;
};
[[nodiscard]] Sunset SunsetSettings();
void SetSunsetSettings(Sunset settings);

// Language code for translating the message being typed.
[[nodiscard]] QString TranslateTo();
void SetTranslateTo(const QString &code);

// The last version whose patch notes were shown.
[[nodiscard]] QString LastSeenVersion();
void SetLastSeenVersion(const QString &version);

// Live wallpaper: a video file path, a resource path or empty.
[[nodiscard]] QString LiveWallpaper();
void SetLiveWallpaper(const QString &path);

// Twitch live statuses: the aglergram live service and its bot.
struct Twitch {
	QString server;
	QString bot;
	bool notify = true;
};
[[nodiscard]] Twitch TwitchSettings();
void SetTwitchSettings(Twitch settings);

// Any other small value of the fork: bookmarks, focus profiles, modes.
[[nodiscard]] QJsonValue Value(const QString &key);
void SetValue(const QString &key, const QJsonValue &value);
[[nodiscard]] rpl::producer<QString> ValueChanges();

// "user:peer" key, the same as for the notes.
[[nodiscard]] QString PeerKey(not_null<Main::Session*> session, PeerId peer);

// Hidden chats: out of the chats list and silent, opened from a box
// behind the PIN.
[[nodiscard]] bool IsHiddenChat(
	not_null<Main::Session*> session,
	PeerId peer);
void SetHiddenChat(
	not_null<Main::Session*> session,
	PeerId peer,
	bool hidden);
[[nodiscard]] std::vector<PeerId> HiddenChats(
	not_null<Main::Session*> session);

// The PIN of hidden chats and locked folders, only its salted hash.
[[nodiscard]] bool HasPin();
[[nodiscard]] bool CheckPin(const QString &pin);
void SetPin(const QString &pin);

// Text recognized on photos, per chat: message id -> text.
[[nodiscard]] base::flat_map<MsgId, QString> RecognizedTexts(
	not_null<Main::Session*> session,
	PeerId peer);
void SaveRecognizedTexts(
	not_null<Main::Session*> session,
	PeerId peer,
	const base::flat_map<MsgId, QString> &texts);

// Local chat names: seen only on this computer, the real name is kept
// in memory to show it and to restore it.
[[nodiscard]] QString LocalName(
	not_null<Main::Session*> session,
	PeerId peer);
void SetLocalName(
	not_null<Main::Session*> session,
	PeerId peer,
	const QString &name);
void RememberRealName(
	not_null<Main::Session*> session,
	PeerId peer,
	const QString &name);
[[nodiscard]] QString RealName(
	not_null<Main::Session*> session,
	PeerId peer);

// The text of a message is about to change and has changed: the edit
// animation snapshots the old bubble on `before` and plays on `changed`.
struct EditEvent {
	not_null<HistoryItem*> item;
	bool before = false;
	bool changed = false;
};
void NotifyEdit(EditEvent event);
[[nodiscard]] rpl::producer<EditEvent> EditEvents();

} // namespace Data::LiquidStore
