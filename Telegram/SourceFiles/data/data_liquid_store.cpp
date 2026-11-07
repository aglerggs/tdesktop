/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_liquid_store.h"

#include "main/main_session.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Data::LiquidStore {
namespace {

struct State {
	QString folder;
	QJsonObject notes;
	std::vector<Reminder> reminders;
	std::vector<Template> templates;
	Sunset sunset;
	QString translateTo = u"en"_q;
	rpl::event_stream<> notesChanged;
};

State &Current() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString NoteKey(not_null<Main::Session*> session, PeerId peer) {
	return QString::number(session->userId().bare)
		+ ':'
		+ QString::number(peer.value);
}

void Save() {
	auto &state = Current();
	if (state.folder.isEmpty()) {
		return;
	}
	auto reminders = QJsonArray();
	for (const auto &reminder : state.reminders) {
		reminders.push_back(QJsonObject{
			{ u"user"_q, QString::number(reminder.userId) },
			{ u"peer"_q, QString::number(reminder.peer.value) },
			{ u"msg"_q, QString::number(reminder.msg.bare) },
			{ u"at"_q, double(reminder.at) },
			{ u"title"_q, reminder.title },
			{ u"preview"_q, reminder.preview },
		});
	}
	auto templates = QJsonArray();
	for (const auto &entry : state.templates) {
		templates.push_back(QJsonObject{
			{ u"name"_q, entry.name },
			{ u"text"_q, entry.text },
		});
	}
	const auto object = QJsonObject{
		{ u"notes"_q, state.notes },
		{ u"reminders"_q, reminders },
		{ u"templates"_q, templates },
		{ u"sunset"_q, QJsonObject{
			{ u"enabled"_q, state.sunset.enabled },
			{ u"latitude"_q, state.sunset.latitude },
			{ u"longitude"_q, state.sunset.longitude },
		} },
		{ u"translateTo"_q, state.translateTo },
	};
	auto file = QFile(state.folder + u"liquid_store.json"_q);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

[[nodiscard]] QString OcrPath(
		not_null<Main::Session*> session,
		PeerId peer) {
	return Current().folder
		+ u"liquid_ocr/"_q
		+ QString::number(session->userId().bare)
		+ '_'
		+ QString::number(peer.value)
		+ u".json"_q;
}

} // namespace

void Load(const QString &folder) {
	auto &state = Current();
	state.folder = folder;
	if (!state.folder.isEmpty() && !state.folder.endsWith('/')) {
		state.folder += '/';
	}
	auto file = QFile(state.folder + u"liquid_store.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	state.notes = object.value(u"notes"_q).toObject();
	for (const auto value : object.value(u"reminders"_q).toArray()) {
		const auto entry = value.toObject();
		state.reminders.push_back({
			.userId = entry.value(u"user"_q).toString().toULongLong(),
			.peer = PeerId(entry.value(u"peer"_q).toString().toULongLong()),
			.msg = MsgId(entry.value(u"msg"_q).toString().toLongLong()),
			.at = TimeId(entry.value(u"at"_q).toDouble()),
			.title = entry.value(u"title"_q).toString(),
			.preview = entry.value(u"preview"_q).toString(),
		});
	}
	for (const auto value : object.value(u"templates"_q).toArray()) {
		const auto entry = value.toObject();
		state.templates.push_back({
			.name = entry.value(u"name"_q).toString(),
			.text = entry.value(u"text"_q).toString(),
		});
	}
	const auto sunset = object.value(u"sunset"_q).toObject();
	state.sunset.enabled = sunset.value(u"enabled"_q).toBool(false);
	state.sunset.latitude = sunset.value(u"latitude"_q).toDouble(47.22);
	state.sunset.longitude = sunset.value(u"longitude"_q).toDouble(39.72);
	state.translateTo = object.value(u"translateTo"_q).toString(u"en"_q);
}

QString Folder() {
	return Current().folder;
}

QString Note(not_null<Main::Session*> session, PeerId peer) {
	return Current().notes.value(NoteKey(session, peer)).toString();
}

void SetNote(
		not_null<Main::Session*> session,
		PeerId peer,
		const QString &text) {
	auto &state = Current();
	const auto key = NoteKey(session, peer);
	const auto trimmed = text.trimmed();
	if (trimmed.isEmpty()) {
		state.notes.remove(key);
	} else {
		state.notes.insert(key, trimmed);
	}
	Save();
	state.notesChanged.fire({});
}

rpl::producer<QString> NoteValue(
		not_null<Main::Session*> session,
		PeerId peer) {
	return rpl::single(rpl::empty) | rpl::then(
		Current().notesChanged.events()
	) | rpl::map([=] {
		return Note(session, peer);
	}) | rpl::distinct_until_changed();
}

void AddReminder(Reminder reminder) {
	Current().reminders.push_back(std::move(reminder));
	Save();
}

std::vector<Reminder> TakeDueReminders(TimeId now) {
	auto &list = Current().reminders;
	auto result = std::vector<Reminder>();
	for (auto i = begin(list); i != end(list);) {
		if (i->at <= now) {
			result.push_back(std::move(*i));
			i = list.erase(i);
		} else {
			++i;
		}
	}
	if (!result.empty()) {
		Save();
	}
	return result;
}

int RemindersCount(uint64 userId) {
	return ranges::count(
		Current().reminders,
		userId,
		&Reminder::userId);
}

const std::vector<Template> &Templates() {
	return Current().templates;
}

void SetTemplates(std::vector<Template> list) {
	Current().templates = std::move(list);
	Save();
}

Sunset SunsetSettings() {
	return Current().sunset;
}

void SetSunsetSettings(Sunset settings) {
	Current().sunset = settings;
	Save();
}

QString TranslateTo() {
	return Current().translateTo;
}

void SetTranslateTo(const QString &code) {
	Current().translateTo = code;
	Save();
}

base::flat_map<MsgId, QString> RecognizedTexts(
		not_null<Main::Session*> session,
		PeerId peer) {
	auto result = base::flat_map<MsgId, QString>();
	auto file = QFile(OcrPath(session, peer));
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	for (auto i = object.begin(); i != object.end(); ++i) {
		result.emplace(MsgId(i.key().toLongLong()), i.value().toString());
	}
	return result;
}

void SaveRecognizedTexts(
		not_null<Main::Session*> session,
		PeerId peer,
		const base::flat_map<MsgId, QString> &texts) {
	if (Current().folder.isEmpty()) {
		return;
	}
	QDir().mkpath(Current().folder + u"liquid_ocr"_q);
	auto object = QJsonObject();
	for (const auto &[id, text] : texts) {
		object.insert(QString::number(id.bare), text);
	}
	auto file = QFile(OcrPath(session, peer));
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

} // namespace Data::LiquidStore
