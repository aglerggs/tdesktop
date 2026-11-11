/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_liquid_store.h"

#include "main/main_session.h"
#include "base/random.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QSet>

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
	QString lastSeenVersion;
	QString liveWallpaper;
	Twitch twitch;
	QJsonObject extra;
	rpl::event_stream<> notesChanged;
	rpl::event_stream<QString> valueChanges;
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
		{ u"lastSeenVersion"_q, state.lastSeenVersion },
		{ u"liveWallpaper"_q, state.liveWallpaper },
		{ u"twitch"_q, QJsonObject{
			{ u"server"_q, state.twitch.server },
			{ u"bot"_q, state.twitch.bot },
			{ u"notify"_q, state.twitch.notify },
		} },
		{ u"extra"_q, state.extra },
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
	state.lastSeenVersion = object.value(u"lastSeenVersion"_q).toString();
	state.liveWallpaper = object.value(u"liveWallpaper"_q).toString();
	const auto twitch = object.value(u"twitch"_q).toObject();
	state.twitch.server = twitch.value(u"server"_q).toString();
	state.twitch.bot = twitch.value(u"bot"_q).toString();
	state.twitch.notify = twitch.value(u"notify"_q).toBool(true);
	state.extra = object.value(u"extra"_q).toObject();
}

QJsonValue Value(const QString &key) {
	return Current().extra.value(key);
}

void SetValue(const QString &key, const QJsonValue &value) {
	auto &state = Current();
	if (value.isUndefined() || value.isNull()) {
		state.extra.remove(key);
	} else {
		state.extra.insert(key, value);
	}
	Save();
	state.valueChanges.fire_copy(key);
}

namespace {

rpl::event_stream<EditEvent> &EditStream() {
	static auto result = rpl::event_stream<EditEvent>();
	return result;
}

[[nodiscard]] base::flat_map<QString, QString> &RealNames() {
	static auto result = base::flat_map<QString, QString>();
	return result;
}

} // namespace

QString LocalName(not_null<Main::Session*> session, PeerId peer) {
	if (!Value(u"plugins"_q).toObject().value(u"rename"_q).toBool(true)) {
		return QString();
	}
	return Value(u"localNames"_q).toObject().value(
		NoteKey(session, peer)).toString();
}

void SetLocalName(
		not_null<Main::Session*> session,
		PeerId peer,
		const QString &name) {
	auto all = Value(u"localNames"_q).toObject();
	const auto key = NoteKey(session, peer);
	const auto trimmed = name.trimmed();
	if (trimmed.isEmpty()) {
		all.remove(key);
	} else {
		all.insert(key, trimmed);
	}
	SetValue(
		u"localNames"_q,
		all.isEmpty() ? QJsonValue() : QJsonValue(all));
}

void RememberRealName(
		not_null<Main::Session*> session,
		PeerId peer,
		const QString &name) {
	RealNames()[NoteKey(session, peer)] = name;
}

QString RealName(not_null<Main::Session*> session, PeerId peer) {
	const auto i = RealNames().find(NoteKey(session, peer));
	return (i != end(RealNames())) ? i->second : QString();
}

void NotifyEdit(EditEvent event) {
	EditStream().fire(std::move(event));
}

rpl::producer<EditEvent> EditEvents() {
	return EditStream().events();
}

rpl::producer<QString> ValueChanges() {
	return Current().valueChanges.events();
}

QString PeerKey(not_null<Main::Session*> session, PeerId peer) {
	return NoteKey(session, peer);
}

namespace {

[[nodiscard]] QSet<QString> &HiddenCache() {
	static auto result = [] {
		auto set = QSet<QString>();
		for (const auto value : Value(u"hidden"_q).toArray()) {
			set.insert(value.toString());
		}
		return set;
	}();
	return result;
}

[[nodiscard]] QByteArray PinHash(const QByteArray &salt, const QString &pin) {
	auto result = salt + pin.toUtf8();
	for (auto i = 0; i != 20000; ++i) {
		result = QCryptographicHash::hash(
			salt + result,
			QCryptographicHash::Sha256);
	}
	return result;
}

} // namespace

bool IsHiddenChat(not_null<Main::Session*> session, PeerId peer) {
	const auto &cache = HiddenCache();
	return !cache.isEmpty() && cache.contains(NoteKey(session, peer));
}

void SetHiddenChat(
		not_null<Main::Session*> session,
		PeerId peer,
		bool hidden) {
	auto &cache = HiddenCache();
	const auto key = NoteKey(session, peer);
	if (hidden) {
		cache.insert(key);
	} else {
		cache.remove(key);
	}
	auto list = QJsonArray();
	for (const auto &entry : cache) {
		list.push_back(entry);
	}
	SetValue(u"hidden"_q, list.isEmpty() ? QJsonValue() : QJsonValue(list));
}

std::vector<PeerId> HiddenChats(not_null<Main::Session*> session) {
	auto result = std::vector<PeerId>();
	const auto prefix = QString::number(session->userId().bare) + ':';
	for (const auto &key : HiddenCache()) {
		if (key.startsWith(prefix)) {
			result.push_back(PeerId(key.mid(prefix.size()).toULongLong()));
		}
	}
	return result;
}

bool HasPin() {
	return !Value(u"pin"_q).toObject().value(u"hash"_q).toString().isEmpty();
}

bool CheckPin(const QString &pin) {
	const auto object = Value(u"pin"_q).toObject();
	const auto salt = QByteArray::fromBase64(
		object.value(u"salt"_q).toString().toLatin1());
	const auto hash = QByteArray::fromBase64(
		object.value(u"hash"_q).toString().toLatin1());
	return !hash.isEmpty() && (PinHash(salt, pin) == hash);
}

void SetPin(const QString &pin) {
	if (pin.isEmpty()) {
		SetValue(u"pin"_q, QJsonValue());
		return;
	}
	auto salt = QByteArray(16, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	SetValue(u"pin"_q, QJsonObject{
		{ u"salt"_q, QString::fromLatin1(salt.toBase64()) },
		{ u"hash"_q, QString::fromLatin1(PinHash(salt, pin).toBase64()) },
	});
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

QString LastSeenVersion() {
	return Current().lastSeenVersion;
}

void SetLastSeenVersion(const QString &version) {
	Current().lastSeenVersion = version;
	Save();
}

Twitch TwitchSettings() {
	return Current().twitch;
}

void SetTwitchSettings(Twitch settings) {
	Current().twitch = std::move(settings);
	Save();
}

QString LiveWallpaper() {
	return Current().liveWallpaper;
}

void SetLiveWallpaper(const QString &path) {
	Current().liveWallpaper = path;
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
