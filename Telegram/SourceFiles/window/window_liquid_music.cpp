/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_music.h"

#include "window/window_liquid_plugins.h"

#include "base/random.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_liquid_store.h"
#include "data/data_peer_id.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Window::Liquid {
namespace {

constexpr auto kService = "https://blinistiy.fun/api/aglergram/music";
constexpr auto kBot = "aglergrambot";
constexpr auto kTokenKey = "musicToken";
constexpr auto kLinkedKey = "musicLinked";
constexpr auto kReadEach = crl::time(10 * 1000);
constexpr auto kHeartbeat = crl::time(2 * 60 * 1000);
constexpr auto kListEach = crl::time(60 * 1000);

struct State {
	base::flat_map<PeerId, NowPlaying> list;
	rpl::event_stream<> updates;
	QString sentKey;
	crl::time sentAt = 0;
	UserId self = 0;
	base::weak_ptr<SessionController> controller;
	std::unique_ptr<base::Timer> readTimer;
	std::unique_ptr<base::Timer> listTimer;
};

[[nodiscard]] State &Current() {
	static auto result = State();
	return result;
}

[[nodiscard]] QNetworkAccessManager &Direct() {
	static auto result = [] {
		auto manager = std::make_unique<QNetworkAccessManager>();
		manager->setProxy(QNetworkProxy::NoProxy);
		return manager;
	}();
	return *result;
}

[[nodiscard]] QNetworkAccessManager &Proxied() {
	static auto result = QNetworkAccessManager();
	return result;
}

// Direct first, then through the app proxy, as for the updates.
void Request(
		QNetworkRequest request,
		QByteArray body,
		Fn<void(int, QByteArray)> done,
		bool direct = true) {
	request.setRawHeader("User-Agent", "aglergram");
	auto &manager = direct ? Direct() : Proxied();
	const auto reply = body.isNull()
		? manager.get(request)
		: manager.post(request, body);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (!status && direct) {
			Request(request, body, done, false);
			return;
		}
		if (done) {
			done(status, reply->readAll());
		}
	});
}

[[nodiscard]] QString Token() {
	return Data::LiquidStore::Value(
		QString::fromLatin1(kTokenKey)).toString();
}

[[nodiscard]] bool Linked() {
	return Data::LiquidStore::Value(
		QString::fromLatin1(kLinkedKey)).toBool();
}

#ifdef Q_OS_WIN

struct Search {
	QString title;
};

BOOL CALLBACK FindSpotify(HWND hwnd, LPARAM param) {
	wchar_t text[512] = { 0 };
	const auto length = GetWindowTextW(hwnd, text, 512);
	if (length <= 0) {
		return TRUE;
	}
	const auto title = QString::fromWCharArray(text, length);
	if (!title.contains(u" - "_q)) {
		return TRUE;
	}
	auto pid = DWORD(0);
	GetWindowThreadProcessId(hwnd, &pid);
	const auto process = OpenProcess(
		PROCESS_QUERY_LIMITED_INFORMATION,
		FALSE,
		pid);
	if (!process) {
		return TRUE;
	}
	wchar_t path[MAX_PATH] = { 0 };
	auto size = DWORD(MAX_PATH);
	const auto ok = QueryFullProcessImageNameW(process, 0, path, &size);
	CloseHandle(process);
	if (!ok) {
		return TRUE;
	}
	const auto exe = QString::fromWCharArray(path, int(size));
	if (!exe.endsWith(u"\\Spotify.exe"_q, Qt::CaseInsensitive)) {
		return TRUE;
	}
	reinterpret_cast<Search*>(param)->title = title;
	return FALSE;
}

#endif // Q_OS_WIN

// The Spotify window title is "Artist - Song" while it plays and just
// "Spotify ..." on pause, so a paused track is not shown.
[[nodiscard]] NowPlaying ReadSpotify() {
#ifdef Q_OS_WIN
	auto search = Search();
	EnumWindows(FindSpotify, reinterpret_cast<LPARAM>(&search));
	const auto index = search.title.indexOf(u" - "_q);
	if (index <= 0) {
		return {};
	}
	return {
		.title = search.title.mid(index + 3).trimmed(),
		.artist = search.title.left(index).trimmed(),
	};
#else // Q_OS_WIN
	return {};
#endif // Q_OS_WIN
}

void Post(const NowPlaying &track) {
	const auto token = Token();
	if (token.isEmpty()) {
		return;
	}
	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kService)));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	const auto body = QJsonDocument(QJsonObject{
		{ u"token"_q, token },
		{ u"title"_q, track.title },
		{ u"artist"_q, track.artist },
	}).toJson(QJsonDocument::Compact);
	Request(request, body, [=](int status, QByteArray data) {
		auto &state = Current();
		if (status == 403) {
			// Not linked yet (the bot did not get /start) or unlinked.
			if (Linked()) {
				Data::LiquidStore::SetValue(
					QString::fromLatin1(kLinkedKey),
					QJsonValue());
			}
			state.sentKey = QString();
			return;
		} else if (status != 200) {
			return;
		}
		const auto object = QJsonDocument::fromJson(data).object();
		const auto tg = object.value(u"tg"_q).toString().toULongLong();
		if (!Linked()) {
			Data::LiquidStore::SetValue(
				QString::fromLatin1(kLinkedKey),
				true);
			if (const auto strong = state.controller.get()) {
				strong->showToast((tg && state.self && UserId(tg) != state.self)
					? u"🎵 Подключено, но к другому аккаунту Telegram"_q
					: u"🎵 «Слушаю сейчас» подключено"_q);
			}
		}
	});
}

void Tick() {
	auto &state = Current();
	if (Token().isEmpty()) {
		return;
	}
	const auto track = PluginEnabled(u"music"_q)
		? ReadSpotify()
		: NowPlaying();
	const auto key = track.title.isEmpty()
		? QString()
		: (track.artist + '\n' + track.title);
	const auto now = crl::now();
	const auto heartbeat = !key.isEmpty()
		&& (now - state.sentAt >= kHeartbeat);
	if (key == state.sentKey && !heartbeat) {
		return;
	}
	// Linking is finished by the first successful post.
	if (key.isEmpty() && state.sentKey.isEmpty() && Linked()) {
		return;
	}
	state.sentKey = key;
	state.sentAt = now;
	Post(track);
}

void RefreshList() {
	if (!PluginEnabled(u"music"_q)) {
		auto &state = Current();
		if (!state.list.empty()) {
			state.list.clear();
			state.updates.fire({});
		}
		return;
	}
	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kService)));
	Request(request, QByteArray(), [=](int status, QByteArray data) {
		if (status != 200) {
			return;
		}
		auto fresh = base::flat_map<PeerId, NowPlaying>();
		const auto object = QJsonDocument::fromJson(data).object();
		for (const auto value : object.value(u"music"_q).toArray()) {
			const auto entry = value.toObject();
			const auto id = entry.value(u"tg"_q).toString().toULongLong();
			const auto title = entry.value(u"title"_q).toString();
			if (!id || title.isEmpty()) {
				continue;
			}
			fresh.emplace(peerFromUser(UserId(id)), NowPlaying{
				.title = title,
				.artist = entry.value(u"artist"_q).toString(),
			});
		}
		auto &state = Current();
		const auto changed = (fresh.size() != state.list.size())
			|| ranges::any_of(fresh, [&](const auto &pair) {
				const auto i = state.list.find(pair.first);
				return (i == end(state.list))
					|| (i->second.title != pair.second.title)
					|| (i->second.artist != pair.second.artist);
			});
		state.list = std::move(fresh);
		if (changed) {
			state.updates.fire({});
		}
	});
}

void Connect(not_null<SessionController*> controller) {
	// The token is the secret: only the bot and this computer know it.
	auto bytes = QByteArray(24, Qt::Uninitialized);
	base::RandomFill(bytes.data(), bytes.size());
	const auto token = QString::fromLatin1(bytes.toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	Data::LiquidStore::SetValue(QString::fromLatin1(kTokenKey), token);
	Data::LiquidStore::SetValue(QString::fromLatin1(kLinkedKey), QJsonValue());
	Current().sentKey = u"?"_q; // Post once more to finish the linking.
	UrlClickHandler::Open(
		u"https://t.me/"_q
			+ QString::fromLatin1(kBot)
			+ u"?start=m"_q
			+ token,
		QVariant::fromValue(ClickHandlerContext{
			.sessionWindow = base::make_weak(controller),
		}));
}

} // namespace

const NowPlaying *NowPlayingFor(PeerId peer) {
	const auto &list = Current().list;
	const auto i = list.find(peer);
	return (i != end(list)) ? &i->second : nullptr;
}

rpl::producer<> NowPlayingUpdates() {
	return Current().updates.events();
}

void StartMusic(not_null<SessionController*> controller) {
	auto &state = Current();
	state.controller = base::make_weak(controller);
	state.self = controller->session().userId();
	if (state.readTimer) {
		return;
	}
	state.readTimer = std::make_unique<base::Timer>([] { Tick(); });
	state.readTimer->callEach(kReadEach);
	state.listTimer = std::make_unique<base::Timer>([] { RefreshList(); });
	state.listTimer->callEach(kListEach);
	RefreshList();
	Tick();
}

void ShowMusicBox(not_null<SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🎵 Слушаю сейчас"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Как в Discord: пока у тебя играет Spotify, друзья с "
			"aglergram видят в твоём профиле строку «🎵 Слушает» с треком. "
			"В обычном Telegram её не видно, и на паузе её тоже нет.\n\n"
			"Подключение — через бота @aglergrambot: нажми «Подключить», "
			"в открывшемся чате нажми «Старт»."_q,
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			Linked()
				? u"✅ Подключено на этом компьютере"_q
				: Token().isEmpty()
				? u"Не подключено"_q
				: u"Ждём «Старт» в @aglergrambot…"_q,
			st::boxDividerLabel));
#ifndef Q_OS_WIN
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Пока работает только в Windows: треки других людей видно, "
			"а свои отсюда не отправляются."_q,
			st::boxDividerLabel));
#endif // Q_OS_WIN
		box->addButton(
			rpl::single(Linked() ? u"Переподключить"_q : u"Подключить"_q),
			[=] {
				box->closeBox();
				Connect(controller);
			});
		if (!Token().isEmpty()) {
			box->addLeftButton(rpl::single(u"Отключить"_q), [=] {
				// Tell the service we stopped, then forget the token.
				Post({});
				Data::LiquidStore::SetValue(
					QString::fromLatin1(kTokenKey),
					QJsonValue());
				Data::LiquidStore::SetValue(
					QString::fromLatin1(kLinkedKey),
					QJsonValue());
				Current().sentKey = QString();
				box->closeBox();
			});
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Window::Liquid
