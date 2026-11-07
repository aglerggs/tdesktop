/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_saved_deleted.h"

#include "base/unixtime.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item_helpers.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Data::SavedDeleted {
namespace {

constexpr auto kMaxEntriesPerChat = 2000;

QString Folder;
bool EnabledValue = true;
base::flat_set<FullMsgId> Marked;

[[nodiscard]] QString SettingsPath() {
	return Folder + u"saved_deleted.json"_q;
}

[[nodiscard]] QString ChatPath(not_null<Main::Session*> session, PeerId peer) {
	return Folder
		+ u"saved_deleted/"_q
		+ QString::number(session->userId().bare)
		+ '/'
		+ QString::number(peer.value)
		+ u".json"_q;
}

[[nodiscard]] QJsonObject ReadJson(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

void WriteJson(const QString &path, const QJsonObject &object) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

[[nodiscard]] QString ItemText(not_null<HistoryItem*> item) {
	const auto &original = item->originalText().text;
	const auto described = item->notificationText().text;
	if (!item->media()) {
		return original;
	}
	// For media keep the description ("Photo", "Video"...) and caption.
	return described.isEmpty() ? original : described;
}

[[nodiscard]] QJsonObject Serialize(const Entry &entry) {
	auto result = QJsonObject();
	result.insert(u"id"_q, double(entry.id.bare));
	result.insert(u"date"_q, double(entry.date));
	result.insert(u"changed"_q, double(entry.changed));
	result.insert(u"author"_q, entry.author);
	result.insert(u"text"_q, entry.text);
	if (entry.fromId) {
		result.insert(u"from"_q, QString::number(entry.fromId));
	}
	if (entry.out) {
		result.insert(u"out"_q, true);
	}
	return result;
}

[[nodiscard]] Entry Deserialize(const QJsonObject &object) {
	return {
		.id = MsgId(int64(object.value(u"id"_q).toDouble())),
		.date = TimeId(object.value(u"date"_q).toDouble()),
		.changed = TimeId(object.value(u"changed"_q).toDouble()),
		.author = object.value(u"author"_q).toString(),
		.text = object.value(u"text"_q).toString(),
		.fromId = object.value(u"from"_q).toString().toULongLong(),
		.out = object.value(u"out"_q).toBool(),
	};
}

base::flat_map<QString, std::vector<Entry>> DeletedCache;

[[nodiscard]] QString CacheKey(not_null<Main::Session*> session, PeerId peer) {
	return QString::number(session->userId().bare)
		+ ':'
		+ QString::number(peer.value);
}

void Append(
		not_null<Main::Session*> session,
		PeerId peer,
		const QString &key,
		const Entry &entry) {
	const auto path = ChatPath(session, peer);
	auto object = ReadJson(path);
	auto list = object.value(key).toArray();
	list.append(Serialize(entry));
	while (list.size() > kMaxEntriesPerChat) {
		list.removeFirst();
	}
	object.insert(key, list);
	WriteJson(path, object);
	DeletedCache.remove(CacheKey(session, peer));
}

[[nodiscard]] std::vector<Entry> Load(
		not_null<Main::Session*> session,
		PeerId peer,
		const QString &key,
		MsgId onlyId = 0) {
	auto result = std::vector<Entry>();
	const auto list = ReadJson(ChatPath(session, peer)).value(key).toArray();
	for (const auto &value : list) {
		auto entry = Deserialize(value.toObject());
		if (!onlyId || entry.id == onlyId) {
			result.push_back(std::move(entry));
		}
	}
	return result;
}

[[nodiscard]] QString FormatTime(TimeId time) {
	return QLocale().toString(
		base::unixtime::parse(time),
		QLocale::ShortFormat);
}

void ShowEntriesBox(
		not_null<Window::SessionController*> controller,
		const QString &title,
		std::vector<Entry> entries,
		bool edits) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		box->setWidth(st::boxWideWidth);
		if (entries.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Пока ничего не сохранено."_q,
				st::boxLabel));
		}
		for (auto i = entries.rbegin(); i != entries.rend(); ++i) {
			const auto header = edits
				? (u"Версия от "_q + FormatTime(i->date))
				: (i->author
					+ u" · отправлено "_q
					+ FormatTime(i->date)
					+ u" · удалено "_q
					+ FormatTime(i->changed));
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				header,
				st::boxDividerLabel));
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				i->text.isEmpty() ? u"(без текста)"_q : i->text,
				st::boxLabel));
			Ui::AddSkip(box->verticalLayout());
		}
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace

void LoadSettings(const QString &folder) {
	Folder = folder;
	if (!Folder.isEmpty() && !Folder.endsWith('/')) {
		Folder += '/';
	}
	const auto object = ReadJson(SettingsPath());
	EnabledValue = object.value(u"enabled"_q).toBool(true);
}

bool Enabled() {
	return EnabledValue && !Folder.isEmpty();
}

void SetEnabled(bool enabled) {
	EnabledValue = enabled;
	auto object = QJsonObject();
	object.insert(u"enabled"_q, enabled);
	WriteJson(SettingsPath(), object);
}

bool ShouldKeep(not_null<HistoryItem*> item) {
	if (!Enabled()
		|| !item->isRegular()
		|| item->isService()
		|| item->isSponsored()
		|| item->out()
		|| item->ttlDestroyAt()) {
		return false;
	}
	// Self-destructing and view-once media disappear as they should.
	if (const auto media = item->media()) {
		if (media->ttlSeconds() || media->ttlSecondsSingleView()) {
			return false;
		}
	}
	return true;
}

void Keep(not_null<HistoryItem*> item) {
	const auto now = base::unixtime::now();
	Append(&item->history()->session(), item->history()->peer->id, u"deleted"_q, {
		.id = item->id,
		.date = item->date(),
		.changed = now,
		.author = item->from()->name(),
		.text = ItemText(item),
		.fromId = item->from()->id.value,
		.out = item->out(),
	});
	Marked.emplace(item->fullId());
}

bool IsMarked(not_null<const HistoryItem*> item) {
	return !Marked.empty() && Marked.contains(item->fullId());
}

void RememberEdit(
		not_null<HistoryItem*> item,
		const QString &previousText,
		TimeId previousDate) {
	if (!Enabled() || !item->isRegular() || item->ttlDestroyAt()) {
		return;
	}
	Append(&item->history()->session(), item->history()->peer->id, u"edits"_q, {
		.id = item->id,
		.date = previousDate,
		.changed = base::unixtime::now(),
		.author = item->from()->name(),
		.text = previousText,
	});
}

std::vector<Entry> Deleted(not_null<PeerData*> peer) {
	return Load(&peer->session(), peer->id, u"deleted"_q);
}

QVector<MTPMessage> WithDeleted(
		not_null<History*> history,
		const QVector<MTPMessage> &slice) {
	if (!Enabled() || slice.isEmpty()) {
		return slice;
	}
	const auto peer = history->peer;
	const auto session = &history->session();
	const auto key = CacheKey(session, peer->id);
	auto i = DeletedCache.find(key);
	if (i == end(DeletedCache)) {
		i = DeletedCache.emplace(key, Deleted(peer)).first;
	}
	const auto &entries = i->second;
	if (entries.empty()) {
		return slice;
	}
	auto minId = std::numeric_limits<int64>::max();
	auto maxId = int64(0);
	auto present = base::flat_set<int64>();
	for (const auto &message : slice) {
		const auto id = IdFromMessage(message).bare;
		minId = std::min(minId, id);
		maxId = std::max(maxId, id);
		present.emplace(id);
	}
	auto added = std::vector<MTPMessage>();
	for (const auto &entry : entries) {
		const auto id = entry.id.bare;
		if (id < minId
			|| id > maxId
			|| present.contains(id)
			|| history->owner().message(peer->id, entry.id)) {
			continue;
		}
		present.emplace(id);
		using Flag = MTPDmessage::Flag;
		const auto fromId = entry.fromId
			? PeerId(entry.fromId)
			: entry.out
			? session->userPeerId()
			: peer->id;
		const auto flags = Flag::f_from_id
			| (entry.out ? Flag::f_out : Flag());
		added.push_back(MTP_message(
			MTP_flags(flags),
			MTP_int(entry.id.bare),
			peerToMTP(fromId),
			MTPint(), // from_boosts_applied
			MTPstring(), // from_rank
			peerToMTP(peer->id),
			MTPPeer(), // saved_peer_id
			MTPMessageFwdHeader(),
			MTPlong(), // via_bot_id
			MTPlong(), // via_business_bot_id
			MTPPeer(), // guestchat_via_from
			MTPMessageReplyHeader(),
			MTP_int(entry.date),
			MTP_string(entry.text),
			MTPMessageMedia(),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTPint(), // views
			MTPint(), // forwards
			MTPMessageReplies(),
			MTPint(), // edit_date
			MTPstring(), // post_author
			MTPlong(), // grouped_id
			MTPMessageReactions(),
			MTPVector<MTPRestrictionReason>(),
			MTPint(), // ttl_period
			MTPint(), // quick_reply_shortcut_id
			MTPlong(), // effect
			MTPFactCheck(),
			MTPint(), // report_delivery_until_date
			MTPlong(), // paid_message_stars
			MTPSuggestedPost(),
			MTPint(), // schedule_repeat_period
			MTPstring(), // summary_from_language
			MTPRichMessage()));
		Marked.emplace(FullMsgId(peer->id, entry.id));
	}
	if (added.empty()) {
		return slice;
	}
	// Keep the order the server used: newest first or oldest first.
	const auto descending = (slice.size() < 2)
		|| (IdFromMessage(slice.front()) >= IdFromMessage(slice.back()));
	auto result = slice;
	for (const auto &message : added) {
		result.push_back(message);
	}
	std::stable_sort(result.begin(), result.end(), [&](
			const MTPMessage &a,
			const MTPMessage &b) {
		return descending
			? (IdFromMessage(a) > IdFromMessage(b))
			: (IdFromMessage(a) < IdFromMessage(b));
	});
	return result;
}

bool HasDeleted(not_null<PeerData*> peer) {
	return Enabled() && !Deleted(peer).empty();
}

std::vector<Entry> Edits(not_null<HistoryItem*> item) {
	return Load(
		&item->history()->session(),
		item->history()->peer->id,
		u"edits"_q,
		item->id);
}

bool HasEdits(not_null<HistoryItem*> item) {
	return Enabled() && item->isRegular() && !Edits(item).empty();
}

void ShowDeletedBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	ShowEntriesBox(
		controller,
		u"Удалённые сообщения"_q,
		Deleted(peer),
		false);
}

void ShowEditsBox(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	ShowEntriesBox(controller, u"История правок"_q, Edits(item), true);
}

} // namespace Data::SavedDeleted
