/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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

// Text recognized on photos, per chat: message id -> text.
[[nodiscard]] base::flat_map<MsgId, QString> RecognizedTexts(
	not_null<Main::Session*> session,
	PeerId peer);
void SaveRecognizedTexts(
	not_null<Main::Session*> session,
	PeerId peer,
	const base::flat_map<MsgId, QString> &texts);

} // namespace Data::LiquidStore
