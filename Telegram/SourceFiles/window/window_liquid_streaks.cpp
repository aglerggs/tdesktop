/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_streaks.h"

#include "window/window_liquid_plugins.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_liquid_store.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>

namespace Window::Liquid {
namespace {

constexpr auto kStreaksKey = "streaks";
constexpr auto kMinShown = 3; // Like TikTok: the flame shows from day 3.
constexpr auto kBackfillChats = 40;
constexpr auto kBackfillPages = 3;
constexpr auto kBackfillLimit = 100;
constexpr auto kBackfillDelay = crl::time(450);

// Local calendar day number.
[[nodiscard]] int DayOf(TimeId date) {
	return int(QDateTime::fromSecsSinceEpoch(date).date().toJulianDay());
}

[[nodiscard]] int Today() {
	return DayOf(base::unixtime::now());
}

struct Entry {
	int day = 0; // The last day where both wrote and it was counted.
	int count = 0;
	int in = 0; // The last day with an incoming message.
	int out = 0; // The last day with an outgoing message.
	int checked = 0; // The day the history was last scanned.
};

[[nodiscard]] Entry ReadEntry(const QJsonObject &object) {
	return {
		.day = object.value(u"d"_q).toInt(),
		.count = object.value(u"c"_q).toInt(),
		.in = object.value(u"i"_q).toInt(),
		.out = object.value(u"o"_q).toInt(),
		.checked = object.value(u"k"_q).toInt(),
	};
}

[[nodiscard]] QJsonObject WriteEntry(const Entry &entry) {
	return QJsonObject{
		{ u"d"_q, entry.day },
		{ u"c"_q, entry.count },
		{ u"i"_q, entry.in },
		{ u"o"_q, entry.out },
		{ u"k"_q, entry.checked },
	};
}

[[nodiscard]] QJsonObject All() {
	return Data::LiquidStore::Value(
		QString::fromLatin1(kStreaksKey)).toObject();
}

[[nodiscard]] Entry Load(not_null<PeerData*> peer) {
	const auto key = Data::LiquidStore::PeerKey(&peer->session(), peer->id);
	return ReadEntry(All().value(key).toObject());
}

void Save(not_null<PeerData*> peer, const Entry &entry) {
	const auto key = Data::LiquidStore::PeerKey(&peer->session(), peer->id);
	auto all = All();
	const auto value = WriteEntry(entry);
	if (all.value(key).toObject() == value) {
		return;
	}
	all.insert(key, value);
	Data::LiquidStore::SetValue(QString::fromLatin1(kStreaksKey), all);
}

[[nodiscard]] bool Suitable(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user
		&& !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& !user->isInaccessible();
}

// Both wrote on `day`: extend or restart the streak.
void Count(Entry &entry, int day) {
	if (entry.day == day) {
		return;
	}
	entry.count = (entry.day == day - 1) ? (entry.count + 1) : 1;
	entry.day = day;
}

void Track(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (!item->isRegular()
		|| item->isService()
		|| !Suitable(peer)
		|| !PluginEnabled(u"streaks"_q)) {
		return;
	}
	const auto day = DayOf(item->date());
	if (day != Today()) {
		return;
	}
	auto entry = Load(peer);
	if (item->out()) {
		entry.out = day;
	} else {
		entry.in = day;
	}
	if (entry.in == day && entry.out == day) {
		Count(entry, day);
	}
	Save(peer, entry);
}

// Rebuild the streak from the last messages of the chat.
void ApplyScan(
		not_null<PeerData*> peer,
		const base::flat_set<int> &inDays,
		const base::flat_set<int> &outDays,
		bool complete) {
	const auto today = Today();
	auto entry = Load(peer);
	entry.checked = today;
	entry.in = inDays.empty() ? entry.in : std::max(entry.in, inDays.back());
	entry.out = outDays.empty()
		? entry.out
		: std::max(entry.out, outDays.back());
	const auto both = [&](int day) {
		return inDays.contains(day) && outDays.contains(day);
	};
	auto start = both(today) ? today : both(today - 1) ? (today - 1) : 0;
	if (!start) {
		entry.day = 0;
		entry.count = 0;
	} else {
		auto count = 0;
		for (auto day = start; both(day); --day) {
			++count;
		}
		// The scanned window can be shorter than the streak: keep the
		// longer remembered value when it ends on the same day.
		if (!complete && entry.day == start && entry.count > count) {
			count = entry.count;
		}
		entry.day = start;
		entry.count = count;
	}
	Save(peer, entry);
}

struct Scan {
	not_null<PeerData*> peer;
	base::flat_set<int> in;
	base::flat_set<int> out;
	int offsetId = 0;
	int pages = 0;
};

void ScanNext(
		not_null<Main::Session*> session,
		std::shared_ptr<std::vector<not_null<PeerData*>>> queue);

void ScanPage(
		not_null<Main::Session*> session,
		std::shared_ptr<Scan> scan,
		std::shared_ptr<std::vector<not_null<PeerData*>>> queue) {
	const auto peer = scan->peer;
	session->api().request(MTPmessages_GetHistory(
		peer->input(),
		MTP_int(scan->offsetId),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kBackfillLimit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		const auto list = result.match([](
				const MTPDmessages_messagesNotModified &) {
			return (const QVector<MTPMessage>*)nullptr;
		}, [](const auto &data) {
			return &data.vmessages().v;
		});
		auto lowest = 0;
		auto oldestDay = 0;
		if (list) {
			for (const auto &message : *list) {
				message.match([&](const MTPDmessage &data) {
					const auto day = DayOf(data.vdate().v);
					(data.is_out() ? scan->out : scan->in).insert(day);
					lowest = data.vid().v;
					oldestDay = day;
				}, [&](const auto &data) {
					lowest = data.vid().v;
				});
			}
		}
		++scan->pages;
		const auto today = Today();
		// Stop when a day without both of you is already behind us.
		auto gap = false;
		if (oldestDay) {
			for (auto day = today - 1; day > oldestDay; --day) {
				if (!scan->in.contains(day) || !scan->out.contains(day)) {
					gap = true;
					break;
				}
			}
		}
		const auto ended = !list
			|| list->size() < kBackfillLimit
			|| lowest <= 1;
		if (!ended && !gap && scan->pages < kBackfillPages) {
			scan->offsetId = lowest;
			base::call_delayed(kBackfillDelay, session, [=] {
				ScanPage(session, scan, queue);
			});
			return;
		}
		ApplyScan(peer, scan->in, scan->out, ended || gap);
		base::call_delayed(kBackfillDelay, session, [=] {
			ScanNext(session, queue);
		});
	}).fail([=](const MTP::Error &error) {
		const auto type = error.type();
		const auto wait = type.startsWith(u"FLOOD_WAIT_"_q)
			? std::clamp(type.mid(11).toInt(), 1, 300) * crl::time(1000)
			: kBackfillDelay;
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			base::call_delayed(wait, session, [=] {
				ScanPage(session, scan, queue);
			});
		} else {
			base::call_delayed(wait, session, [=] {
				ScanNext(session, queue);
			});
		}
	}).send();
}

void ScanNext(
		not_null<Main::Session*> session,
		std::shared_ptr<std::vector<not_null<PeerData*>>> queue) {
	if (queue->empty() || !PluginEnabled(u"streaks"_q)) {
		return;
	}
	const auto peer = queue->front();
	queue->erase(queue->begin());
	ScanPage(session, std::make_shared<Scan>(Scan{ .peer = peer }), queue);
}

void Backfill(not_null<Main::Session*> session) {
	if (!PluginEnabled(u"streaks"_q)) {
		return;
	}
	const auto today = Today();
	auto queue = std::make_shared<std::vector<not_null<PeerData*>>>();
	for (const auto &row : *session->data().chatsList()->indexed()) {
		const auto history = row->history();
		if (!history || !Suitable(history->peer)) {
			continue;
		}
		const auto last = history->lastMessage();
		// Only chats alive today or yesterday can hold a streak.
		if (!last || DayOf(last->date()) < today - 1) {
			continue;
		}
		if (Load(history->peer).checked == today) {
			continue;
		}
		queue->push_back(history->peer);
		if (int(queue->size()) >= kBackfillChats) {
			break;
		}
	}
	ScanNext(session, queue);
}

} // namespace

Streak StreakFor(not_null<PeerData*> peer) {
	if (!Suitable(peer) || !PluginEnabled(u"streaks"_q)) {
		return {};
	}
	const auto entry = Load(peer);
	const auto today = Today();
	if (entry.count < kMinShown) {
		return {};
	} else if (entry.day == today) {
		return { .count = entry.count };
	} else if (entry.day == today - 1) {
		return { .count = entry.count, .atRisk = true };
	}
	return {};
}

rpl::producer<> StreakUpdates() {
	return Data::LiquidStore::ValueChanges(
	) | rpl::filter([](const QString &key) {
		return (key == QString::fromLatin1(kStreaksKey))
			|| (key == u"plugins"_q);
	}) | rpl::to_empty;
}

void StartStreaks(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	static auto started = base::flat_set<not_null<Main::Session*>>();
	if (started.contains(session)) {
		return;
	}
	started.emplace(session);
	session->lifetime().add([=] {
		started.remove(session);
	});

	session->data().newItemAdded(
	) | rpl::on_next([](not_null<HistoryItem*> item) {
		Track(item);
	}, session->lifetime());

	// Give the chats list a moment to load, then scan; again each day.
	base::call_delayed(crl::time(8000), session, [=] {
		Backfill(session);
	});
	const auto daily = session->lifetime().make_state<base::Timer>([=] {
		Backfill(session);
		Data::LiquidStore::SetValue(u"streaksTick"_q, Today());
	});
	daily->callEach(30 * 60 * crl::time(1000));
}

} // namespace Window::Liquid
