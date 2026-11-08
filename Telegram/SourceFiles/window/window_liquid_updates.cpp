/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_updates.h"

#include "window/window_liquid_aura.h"

#include "base/call_delayed.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "data/data_liquid_store.h"
#include "media/clip/media_clip_reader.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "mainwindow.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Window::Liquid {
namespace {

constexpr auto kPatchesPath = ":/gui/liquid/patches.json";
constexpr auto kSnowLeopard = ":/gui/art/liquid/snow_leopard.mp4";
constexpr auto kTagPrefix = "aglergram-v";
constexpr auto kCheckEach = crl::time(6 * 3600 * 1000);

struct PatchItem {
	QString icon;
	QString title;
	QString text;
};

struct Patch {
	QString version;
	QString title;
	std::vector<PatchItem> items;
};

struct Patches {
	QString repository;
	std::vector<Patch> list;
};

const Patches &LoadedPatches() {
	static const auto result = [] {
		auto result = Patches();
		auto file = QFile(QString::fromLatin1(kPatchesPath));
		if (!file.open(QIODevice::ReadOnly)) {
			return result;
		}
		const auto object = QJsonDocument::fromJson(file.readAll()).object();
		result.repository = object.value(u"repository"_q).toString();
		for (const auto value : object.value(u"patches"_q).toArray()) {
			const auto patch = value.toObject();
			auto entry = Patch{
				.version = patch.value(u"version"_q).toString(),
				.title = patch.value(u"title"_q).toString(),
			};
			for (const auto item : patch.value(u"items"_q).toArray()) {
				const auto fields = item.toObject();
				entry.items.push_back({
					.icon = fields.value(u"icon"_q).toString(),
					.title = fields.value(u"title"_q).toString(),
					.text = fields.value(u"text"_q).toString(),
				});
			}
			result.list.push_back(std::move(entry));
		}
		return result;
	}();
	return result;
}

// "0.12b" -> { 0, 12, 'b' }, compared part by part.
[[nodiscard]] std::vector<int> VersionParts(QString version) {
	auto result = std::vector<int>();
	if (version.startsWith(QString::fromLatin1(kTagPrefix))) {
		version = version.mid(int(strlen(kTagPrefix)));
	}
	for (const auto &part : version.split('.')) {
		auto digits = QString();
		auto letter = 0;
		for (const auto ch : part) {
			if (ch.isDigit()) {
				digits += ch;
			} else if (ch.isLetter()) {
				letter = ch.toLower().unicode() - 'a' + 1;
			}
		}
		result.push_back(digits.toInt());
		result.push_back(letter);
	}
	return result;
}

[[nodiscard]] bool Newer(const QString &a, const QString &b) {
	return VersionParts(a) > VersionParts(b);
}

void FillPatch(not_null<Ui::VerticalLayout*> layout, const Patch &patch) {
	layout->add(object_ptr<Ui::FlatLabel>(
		layout,
		u"Версия "_q + patch.version + u" — "_q + patch.title,
		st::boxTitle), st::boxRowPadding);
	for (const auto &item : patch.items) {
		layout->add(object_ptr<Ui::FlatLabel>(
			layout,
			rpl::single(Ui::Text::Bold(item.icon + ' ' + item.title)),
			st::boxLabel), st::boxRowPadding);
		layout->add(object_ptr<Ui::FlatLabel>(
			layout,
			item.text,
			st::boxDividerLabel), st::boxRowPadding);
	}
	Ui::AddSkip(layout);
}

void ShowPatchesBox(
		not_null<SessionController*> controller,
		const QString &title,
		std::vector<Patch> patches) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		box->setWidth(st::boxWideWidth);
		for (const auto &patch : patches) {
			FillPatch(box->verticalLayout(), patch);
		}
		box->addButton(rpl::single(u"Круто"_q), [=] { box->closeBox(); });
	}));
}

// Self-update.

QNetworkAccessManager &Network() {
	static auto result = QNetworkAccessManager();
	return result;
}

// GitHub is reached directly first: the Telegram proxy set in the app
// (SOCKS of tg-ws-proxy, for one) often carries only Telegram traffic.
QNetworkAccessManager &DirectNetwork() {
	static auto result = [] {
		auto manager = std::make_unique<QNetworkAccessManager>();
		manager->setProxy(QNetworkProxy::NoProxy);
		return manager;
	}();
	return *result;
}

// Direct, then through the app proxy if the direct way failed.
void Fetch(
		QNetworkRequest request,
		Fn<void(QNetworkReply*)> done,
		bool direct = true) {
	request.setRawHeader("User-Agent", "aglergram");
	const auto reply = (direct ? DirectNetwork() : Network()).get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto failed = (reply->error() != QNetworkReply::NoError)
			&& (status == 0);
		if (failed && direct) {
			LOG(("aglergram: direct request failed (%1), via proxy: %2"
				).arg(reply->errorString()
				).arg(request.url().toString()));
			Fetch(request, done, false);
			return;
		}
		done(reply);
	});
}

[[nodiscard]] QString ExePath() {
	return QCoreApplication::applicationFilePath();
}

void RemoveOldExe() {
	QFile::remove(ExePath() + u".old"_q);
	QFile::remove(ExePath() + u".new"_q);
}

void InstallAndRestart(
		not_null<SessionController*> controller,
		const QByteArray &bytes) {
	const auto exe = ExePath();
	const auto fresh = exe + u".new"_q;
	auto file = QFile(fresh);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| file.write(bytes) != bytes.size()) {
		controller->showToast(u"Не удалось сохранить обновление"_q);
		return;
	}
	file.close();
	// A running exe can be renamed on Windows, but not overwritten.
	QFile::remove(exe + u".old"_q);
	if (!QFile::rename(exe, exe + u".old"_q)) {
		controller->showToast(u"Не удалось заменить файл программы"_q);
		return;
	}
	if (!QFile::rename(fresh, exe)) {
		QFile::rename(exe + u".old"_q, exe);
		controller->showToast(u"Не удалось заменить файл программы"_q);
		return;
	}
	Core::Restart();
}

void DownloadUpdate(
		not_null<SessionController*> controller,
		const QString &url) {
	controller->showToast(u"Скачиваю обновление…"_q);
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	const auto weak = base::make_weak(controller);
	Fetch(request, [=](QNetworkReply *reply) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			LOG(("aglergram: update download failed: %1"
				).arg(reply->errorString()));
			strong->showToast(u"Не удалось скачать обновление: "_q
				+ reply->errorString());
			return;
		}
		const auto bytes = reply->readAll();
		if (bytes.size() < 1024 * 1024 || !bytes.startsWith("MZ")) {
			strong->showToast(u"Скачанный файл повреждён"_q);
			return;
		}
		InstallAndRestart(strong, bytes);
	});
}

void ShowUpdateBox(
		not_null<SessionController*> controller,
		const QString &version,
		const QString &notes,
		const QString &url) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Вышла версия "_q + version));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			notes.isEmpty() ? u"Новая версия aglergram."_q : notes,
			st::boxLabel));
		box->addButton(rpl::single(u"Обновить"_q), [=] {
			box->closeBox();
			DownloadUpdate(controller, url);
		});
		box->addButton(rpl::single(u"Позже"_q), [=] { box->closeBox(); });
	}));
}

// Live wallpaper.

struct Wallpaper {
	::Media::Clip::ReaderPointer reader;
	::Media::Clip::FrameRequest request;
	QImage frame;
	QString path;
	int attempts = 0;
	std::unique_ptr<base::Timer> drawn;
	rpl::event_stream<> frames;
};

Wallpaper &CurrentWallpaper() {
	static auto result = Wallpaper();
	return result;
}

[[nodiscard]] bool IsPicture(const QString &path) {
	static const auto kSuffixes = QStringList{
		u"png"_q, u"jpg"_q, u"jpeg"_q, u"webp"_q, u"bmp"_q,
	};
	return kSuffixes.contains(QFileInfo(path).suffix().toLower());
}

void ShowPicture(const QString &path) {
	auto &state = CurrentWallpaper();
	auto image = QImage(path);
	if (image.isNull()) {
		LOG(("aglergram: wallpaper picture not loaded: %1").arg(path));
	} else {
		constexpr auto kMaxSide = 2560;
		if (std::max(image.width(), image.height()) > kMaxSide) {
			image = image.scaled(
				kMaxSide,
				kMaxSide,
				Qt::KeepAspectRatio,
				Qt::SmoothTransformation);
		}
		state.frame = std::move(image);
	}
	state.frames.fire({});
}

// Drawn wallpapers: a frame is painted in the background ~24 times a
// second, the main thread only takes the ready picture.
void StartDrawnWallpaper(const QString &path) {
	auto &state = CurrentWallpaper();
	const auto started = crl::now();
	const auto busy = std::make_shared<bool>(false);
	state.drawn = std::make_unique<base::Timer>([=] {
		if (*busy || CurrentWallpaper().path != path) {
			return;
		}
		*busy = true;
		const auto ms = crl::now() - started;
		crl::async([=] {
			auto frame = RenderAuraFrame(path, ms);
			crl::on_main([=, frame = std::move(frame)]() mutable {
				*busy = false;
				auto &state = CurrentWallpaper();
				if (state.path == path && !frame.isNull()) {
					state.frame = std::move(frame);
					state.frames.fire({});
				}
			});
		});
	});
	state.drawn->callEach(crl::time(42));
}

void ApplyWallpaper(const QString &path, bool retry = false) {
	auto &state = CurrentWallpaper();
	if (!retry && state.path == path && (state.reader || path.isEmpty())) {
		return;
	}
	if (!retry) {
		state.attempts = 0;
	}
	state.path = path;
	state.reader = nullptr;
	state.frame = QImage();
	state.drawn = nullptr;
	if (path.isEmpty()) {
		state.frames.fire({});
		return;
	} else if (IsAuraPath(path)) {
		StartDrawnWallpaper(path);
		return;
	} else if (IsPicture(path)) {
		ShowPicture(path);
		return;
	}
	const auto callback = [](::Media::Clip::Notification notification) {
		auto &state = CurrentWallpaper();
		const auto reader = state.reader.get();
		if (!reader) {
			return;
		}
		using Notification = ::Media::Clip::Notification;
		if (notification == Notification::Reinit) {
			if (reader->state() == ::Media::Clip::State::Error) {
				state.reader.setBad();
				state.frame = QImage();
				state.frames.fire({});
			} else if (reader->ready() && !reader->started()) {
				const auto original = QSize(reader->width(), reader->height());
				// Never upscale here: a second smooth scale on paint made
				// small videos blurry. Only big ones are reduced.
				const auto limit = QSize(1920, 1920);
				const auto size = (original.width() > limit.width()
					|| original.height() > limit.height())
					? original.scaled(limit, Qt::KeepAspectRatio)
					: original;
				state.request.frame = size;
				state.request.outer = size;
				state.request.factor = 1;
				reader->start(state.request);
			}
		} else if (reader->started()) {
			state.frame = reader->current(state.request, crl::now());
			state.frames.fire({});
		}
	};
	// Read into memory: no file kept locked, any path characters work.
	auto file = QFile(path);
	if (file.open(QIODevice::ReadOnly)) {
		state.reader = ::Media::Clip::MakeReader(
			file.readAll(),
			callback,
			::Media::Clip::Reader::LargeFrames());
	} else {
		LOG(("aglergram: live wallpaper not found: %1").arg(path));
	}
	state.frames.fire({});
}

// The first start of the reader right at the launch sometimes gives no
// frames, so it is started again until the first frame comes.
void WatchWallpaper() {
	static auto timer = base::Timer([] {
		auto &state = CurrentWallpaper();
		if (state.path.isEmpty()
			|| !state.frame.isNull()
			|| IsAuraPath(state.path)) {
			return;
		}
		if (++state.attempts > 6) {
			return;
		}
		LOG(("aglergram: live wallpaper restart %1").arg(state.attempts));
		ApplyWallpaper(state.path, true);
	});
	timer.callEach(4 * crl::time(1000));
}

// Own videos live in their own folder with unique names, so choosing a
// new one never deletes the file that is playing right now.
// Recent own wallpapers: copies in tdata, newest first, so switching
// between them needs no new upload.
constexpr auto kRecentWallpapersKey = "recentWallpapers";
constexpr auto kMaxRecentWallpapers = 12;

[[nodiscard]] QString RecentFolder() {
	return Data::LiquidStore::Folder() + u"liquid_wallpapers/"_q;
}

[[nodiscard]] QStringList RecentWallpapers() {
	auto result = QStringList();
	const auto list = Data::LiquidStore::Value(
		QString::fromLatin1(kRecentWallpapersKey)).toArray();
	for (const auto value : list) {
		const auto path = value.toString();
		if (QFileInfo::exists(path) && !result.contains(path)) {
			result.push_back(path);
		}
	}
	return result;
}

void SaveRecentWallpapers(QStringList list) {
	// Files that fell out of the list are not needed anymore.
	while (list.size() > kMaxRecentWallpapers) {
		const auto dropped = list.takeLast();
		if (dropped.startsWith(RecentFolder())
			&& dropped != Data::LiquidStore::LiveWallpaper()) {
			QFile::remove(dropped);
		}
	}
	auto array = QJsonArray();
	for (const auto &path : list) {
		array.push_back(path);
	}
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kRecentWallpapersKey),
		array.isEmpty() ? QJsonValue() : QJsonValue(array));
}

void ForgetRecentWallpaper(const QString &path) {
	auto list = RecentWallpapers();
	list.removeAll(path);
	SaveRecentWallpapers(list);
	if (path.startsWith(RecentFolder())
		&& path != Data::LiquidStore::LiveWallpaper()) {
		QFile::remove(path);
	}
}

// Copies a chosen file into tdata (the original may be moved) and puts
// it on top of the recent ones.
[[nodiscard]] QString AddRecentWallpaper(const QString &source) {
	QDir().mkpath(RecentFolder());
	const auto target = RecentFolder()
		+ QString::number(QDateTime::currentMSecsSinceEpoch())
		+ '_'
		+ QFileInfo(source).fileName();
	const auto path = QFile::copy(source, target) ? target : source;
	auto list = RecentWallpapers();
	list.removeAll(path);
	list.push_front(path);
	SaveRecentWallpapers(list);
	return path;
}

// The file name without the time prefix added on copying.
[[nodiscard]] QString RecentTitle(const QString &path) {
	auto name = QFileInfo(path).fileName();
	const auto underscore = name.indexOf('_');
	if (underscore > 0 && name.left(underscore).toLongLong() > 0) {
		name = name.mid(underscore + 1);
	}
	return name;
}

// Twitch live statuses.

struct TwitchState {
	base::flat_map<PeerId, TwitchStream> live;
	base::flat_set<PeerId> announced;
	rpl::event_stream<> updates;
	bool first = true;
	base::weak_ptr<SessionController> controller;
};

TwitchState &CurrentTwitch() {
	static auto result = TwitchState();
	return result;
}

void AnnounceStreams(const base::flat_map<PeerId, TwitchStream> &fresh) {
	auto &state = CurrentTwitch();
	const auto controller = state.controller.get();
	const auto notify = Data::LiquidStore::TwitchSettings().notify;
	for (const auto &[peerId, stream] : fresh) {
		if (state.announced.contains(peerId)) {
			continue;
		}
		state.announced.emplace(peerId);
		if (state.first || !notify || !controller) {
			continue;
		}
		// Only people the user actually talks to.
		const auto peer = controller->session().data().peerLoaded(peerId);
		if (!peer || peer->isSelf()) {
			continue;
		}
		const auto history = controller->session().data().historyLoaded(peer);
		if (!history || history->isEmpty()) {
			continue;
		}
		controller->showToast(
			u"🔴 "_q + peer->shortName() + u" начал стрим: "_q + stream.title);
	}
	for (auto i = state.announced.begin(); i != state.announced.end();) {
		if (!fresh.contains(*i)) {
			i = state.announced.erase(i);
		} else {
			++i;
		}
	}
	state.first = false;
}

// Backups.

[[nodiscard]] QStringList BackupFiles() {
	const auto folder = Data::LiquidStore::Folder();
	auto result = QStringList{
		u"liquid_store.json"_q,
		u"liquid_glass.json"_q,
		u"saved_deleted.json"_q,
	};
	for (const auto &sub : { u"saved_deleted"_q, u"liquid_ocr"_q }) {
		auto it = QDirIterator(
			folder + sub,
			QDir::Files,
			QDirIterator::Subdirectories);
		while (it.hasNext()) {
			result.push_back(QDir(folder).relativeFilePath(it.next()));
		}
	}
	return result;
}

} // namespace

QString CurrentVersion() {
	const auto &list = LoadedPatches().list;
	return list.empty() ? u"0.0"_q : list.back().version;
}

void StartUpdates(not_null<SessionController*> controller) {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;
	RemoveOldExe();
	{
		// An own wallpaper chosen before «Недавние» existed goes there too.
		const auto path = Data::LiquidStore::LiveWallpaper();
		if (!path.isEmpty()
			&& !path.startsWith(':')
			&& !IsAuraPath(path)
			&& QFileInfo::exists(path)
			&& !RecentWallpapers().contains(path)) {
			auto list = RecentWallpapers();
			list.push_front(path);
			SaveRecentWallpapers(list);
		}
	}
	ApplyWallpaper(Data::LiquidStore::LiveWallpaper());
	WatchWallpaper();
	CurrentTwitch().controller = base::make_weak(controller);
	RefreshTwitch();
	static auto twitchTimer = base::Timer([] { RefreshTwitch(); });
	twitchTimer.callEach(60 * crl::time(1000));

	const auto current = CurrentVersion();
	const auto seen = Data::LiquidStore::LastSeenVersion();
	if (seen != current) {
		auto unseen = std::vector<Patch>();
		for (const auto &patch : LoadedPatches().list) {
			if (seen.isEmpty() ? (patch.version == current) : Newer(patch.version, seen)) {
				unseen.push_back(patch);
			}
		}
		Data::LiquidStore::SetLastSeenVersion(current);
		if (!unseen.empty()) {
			std::reverse(begin(unseen), end(unseen));
			const auto weak = base::make_weak(controller);
			// Let the startup splash finish first.
			base::call_delayed(6500, controller, [=] {
				if (const auto strong = weak.get()) {
					ShowPatchesBox(strong, u"Что нового"_q, unseen);
				}
			});
		}
	}

	const auto weak = base::make_weak(controller);
	static auto timer = base::Timer([=] {
		if (const auto strong = weak.get()) {
			CheckForUpdates(strong, false);
		}
	});
	timer.callEach(kCheckEach);
	base::call_delayed(20 * 1000, controller, [=] {
		if (const auto strong = weak.get()) {
			CheckForUpdates(strong, false);
		}
	});
}

void ShowPatchHistory(not_null<SessionController*> controller) {
	auto list = LoadedPatches().list;
	std::reverse(begin(list), end(list));
	ShowPatchesBox(controller, u"История обновлений"_q, std::move(list));
}

namespace {

void ShowFoundUpdate(
		not_null<SessionController*> controller,
		bool manual,
		const QString &tag,
		const QString &notes,
		const QString &url) {
	if (tag.isEmpty() || url.isEmpty() || !Newer(tag, CurrentVersion())) {
		if (manual) {
			controller->showToast(u"У тебя последняя версия "_q + CurrentVersion());
		}
		return;
	}
	ShowUpdateBox(
		controller,
		tag.mid(int(strlen(kTagPrefix))),
		notes,
		url);
}

// Without the API (it has a limit of requests per hour): the page of the
// latest release redirects to its tag, the file name is always the same.
void CheckByReleasePage(
		not_null<SessionController*> controller,
		bool manual,
		const QString &repository) {
	auto request = QNetworkRequest(QUrl(
		u"https://github.com/"_q + repository + u"/releases/latest"_q));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	const auto weak = base::make_weak(controller);
	Fetch(request, [=](QNetworkReply *reply) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto location = reply->attribute(
			QNetworkRequest::RedirectionTargetAttribute).toUrl().toString();
		const auto marker = u"/releases/tag/"_q;
		const auto index = location.indexOf(marker);
		if (index < 0) {
			LOG(("aglergram: update check failed: %1"
				).arg(reply->errorString()));
			if (manual) {
				strong->showToast(u"Не удалось проверить обновления: "_q
					+ reply->errorString());
			}
			return;
		}
		const auto tag = location.mid(index + marker.size()).section('/', 0, 0);
		const auto url = u"https://github.com/"_q
			+ repository
			+ u"/releases/download/"_q
			+ tag
			+ u"/aglergram.exe"_q;
		ShowFoundUpdate(strong, manual, tag, QString(), url);
	});
}

} // namespace

void CheckForUpdates(not_null<SessionController*> controller, bool manual) {
	const auto repository = LoadedPatches().repository;
	if (repository.isEmpty()) {
		return;
	}
	auto request = QNetworkRequest(QUrl(
		u"https://api.github.com/repos/"_q + repository + u"/releases/latest"_q));
	request.setRawHeader("Accept", "application/vnd.github+json");
	const auto weak = base::make_weak(controller);
	Fetch(request, [=](QNetworkReply *reply) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto object = QJsonDocument::fromJson(reply->readAll()).object();
		const auto tag = object.value(u"tag_name"_q).toString();
		auto url = QString();
		for (const auto value : object.value(u"assets"_q).toArray()) {
			const auto asset = value.toObject();
			if (asset.value(u"name"_q).toString().endsWith(u".exe"_q)) {
				url = asset.value(u"browser_download_url"_q).toString();
			}
		}
		if (reply->error() != QNetworkReply::NoError || tag.isEmpty()) {
			// Rate limit or the API is unreachable: the releases page.
			LOG(("aglergram: releases API failed (%1), trying the page"
				).arg(reply->errorString()));
			CheckByReleasePage(strong, manual, repository);
			return;
		}
		ShowFoundUpdate(
			strong,
			manual,
			tag,
			object.value(u"body"_q).toString(),
			url);
	});
}

void ExportBackup(not_null<SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	FileDialog::GetWritePath(
		controller->widget().get(),
		u"Резервная копия"_q,
		u"Резервная копия aglergram (*.liquidbackup)"_q,
		u"aglergram.liquidbackup"_q,
		[=](QString &&path) {
			const auto folder = Data::LiquidStore::Folder();
			auto files = QJsonObject();
			for (const auto &relative : BackupFiles()) {
				auto file = QFile(folder + relative);
				if (file.open(QIODevice::ReadOnly)) {
					files.insert(
						relative,
						QString::fromLatin1(file.readAll().toBase64()));
				}
			}
			const auto object = QJsonObject{
				{ u"version"_q, CurrentVersion() },
				{ u"files"_q, files },
			};
			auto out = QFile(path);
			const auto ok = out.open(QIODevice::WriteOnly | QIODevice::Truncate)
				&& out.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
			if (const auto strong = weak.get()) {
				strong->showToast(ok
					? (u"Сохранено файлов: "_q + QString::number(files.size()))
					: u"Не удалось сохранить копию"_q);
			}
		});
}

void ImportBackup(not_null<SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget().get(),
		u"Восстановить из копии"_q,
		u"Резервная копия aglergram (*.liquidbackup)"_q,
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			auto in = QFile(result.paths.front());
			if (!in.open(QIODevice::ReadOnly)) {
				return;
			}
			const auto object = QJsonDocument::fromJson(in.readAll()).object();
			const auto files = object.value(u"files"_q).toObject();
			const auto folder = Data::LiquidStore::Folder();
			auto restored = 0;
			for (auto i = files.begin(); i != files.end(); ++i) {
				const auto relative = i.key();
				if (relative.contains(u".."_q) || QDir::isAbsolutePath(relative)) {
					continue;
				}
				const auto path = folder + relative;
				QDir().mkpath(QFileInfo(path).absolutePath());
				auto file = QFile(path);
				if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
					file.write(QByteArray::fromBase64(i.value().toString().toLatin1()));
					++restored;
				}
			}
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->setTitle(rpl::single(u"Копия восстановлена"_q));
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"Восстановлено файлов: "_q + QString::number(restored)
						+ u". Перезапусти Telegram, чтобы всё подхватилось."_q,
					st::boxLabel));
				box->addButton(rpl::single(u"Перезапустить"_q), [] {
					Core::Restart();
				});
				box->addButton(rpl::single(u"Позже"_q), [=] { box->closeBox(); });
			}));
		});
}

void ShowLiveWallpaperBox(not_null<SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Живые обои"_q));
		box->setWidth(st::boxWideWidth);
		const auto current = Data::LiquidStore::LiveWallpaper();
		const auto choose = [=](const QString &path) {
			Data::LiquidStore::SetLiveWallpaper(path);
			// A choice by hand always starts the wallpaper anew.
			ApplyWallpaper(path, true);
			box->closeBox();
		};
		const auto mark = [=](const QString &path, const QString &text) {
			return ((path == current) ? u"✓ "_q : QString()) + text;
		};
		const auto add = [&](const QString &text, Fn<void()> callback) {
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(text),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback(std::move(callback));
		};
		const auto header = [&](const QString &text) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				text,
				st::boxDividerLabel));
		};

		header(u"Встроенные"_q);
		const auto leopard = QString::fromLatin1(kSnowLeopard);
		add(mark(leopard, u"🐆 Снежный барс"_q), [=] { choose(leopard); });
		for (const auto &preset : AuraPresets()) {
			const auto path = AuraPath(preset.id);
			add(mark(path, preset.title), [=] { choose(path); });
		}

		const auto recent = RecentWallpapers();
		if (!recent.isEmpty()) {
			header(u"Недавние — свои, хранятся только на этом компьютере"_q);
			for (const auto &path : recent) {
				const auto icon = IsPicture(path) ? u"🖼 "_q : u"🎞 "_q;
				add(mark(path, icon + RecentTitle(path)), [=] {
					auto list = RecentWallpapers();
					list.removeAll(path);
					list.push_front(path);
					SaveRecentWallpapers(list);
					choose(path);
				});
				add(u"      убрать из недавних"_q, [=] {
					if (Data::LiquidStore::LiveWallpaper() == path) {
						Data::LiquidStore::SetLiveWallpaper(QString());
						ApplyWallpaper(QString());
					}
					ForgetRecentWallpaper(path);
					box->closeBox();
					ShowLiveWallpaperBox(controller);
				});
			}
		}

		header(u"Своё"_q);
		add(u"➕ Добавить видео, GIF или картинку…"_q, [=] {
			FileDialog::GetOpenPath(
				controller->widget().get(),
				u"Живые обои"_q,
				u"Видео и картинки "
				"(*.mp4 *.webm *.mov *.gif *.png *.jpg *.jpeg *.webp *.bmp)"_q,
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					const auto path = AddRecentWallpaper(result.paths.front());
					Data::LiquidStore::SetLiveWallpaper(path);
					ApplyWallpaper(path, true);
				});
			box->closeBox();
		});
		add(mark(QString(), u"Без живых обоев"_q), [=] { choose(QString()); });
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Видео крутится по кругу без звука, стекло размывает его в шапке "
			"и поле ввода. Добавленные обои остаются в «Недавних» — менять "
			"их можно без повторной загрузки."_q,
			st::boxDividerLabel));
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	}));
}

bool LiveWallpaperActive() {
	return !CurrentWallpaper().frame.isNull();
}

bool PaintLiveWallpaper(QPainter &p, QSize fill, QRect clip) {
	const auto &frame = CurrentWallpaper().frame;
	if (frame.isNull() || fill.isEmpty()) {
		return false;
	}
	const auto source = QSizeF(frame.size());
	const auto scale = std::max(
		fill.width() / source.width(),
		fill.height() / source.height());
	const auto size = source * scale;
	const auto target = QRectF(
		QPointF(
			(fill.width() - size.width()) / 2.,
			(fill.height() - size.height()) / 2.),
		size);
	p.save();
	p.setClipRect(clip, Qt::IntersectClip);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.drawImage(target, frame);
	// A light veil keeps the text readable over any video.
	p.fillRect(
		clip,
		Window::Theme::IsNightMode()
			? QColor(0, 0, 0, 56)
			: QColor(255, 255, 255, 36));
	p.restore();
	return true;
}

rpl::producer<> LiveWallpaperFrames() {
	return CurrentWallpaper().frames.events();
}

const TwitchStream *TwitchStreamFor(PeerId peer) {
	const auto &live = CurrentTwitch().live;
	if (live.empty() || !peer.is<UserId>()) {
		return nullptr;
	}
	const auto i = live.find(peer);
	return (i != end(live)) ? &i->second : nullptr;
}

rpl::producer<> TwitchUpdates() {
	return CurrentTwitch().updates.events();
}

// Live statuses come from the site: people link Twitch and Telegram
// there and turn the showing on themselves.
constexpr auto kTwitchSite = "https://blinistiy.fun";

[[nodiscard]] QString TwitchServer() {
	const auto custom = Data::LiquidStore::TwitchSettings().server.trimmed();
	return custom.isEmpty()
		? (QString::fromLatin1(kTwitchSite) + u"/api/aglergram"_q)
		: custom;
}

void RefreshTwitch() {
	auto server = TwitchServer();
	while (server.endsWith('/')) {
		server.chop(1);
	}
	if (!server.startsWith(u"http"_q)) {
		server = u"https://"_q + server;
	}
	auto request = QNetworkRequest(QUrl(server + u"/live"_q));
	Fetch(request, [=](QNetworkReply *reply) {
		if (reply->error() != QNetworkReply::NoError) {
			return;
		}
		const auto object = QJsonDocument::fromJson(reply->readAll()).object();
		auto fresh = base::flat_map<PeerId, TwitchStream>();
		for (const auto value : object.value(u"live"_q).toArray()) {
			const auto entry = value.toObject();
			const auto id = entry.value(u"tg"_q).toString().toULongLong();
			if (!id) {
				continue;
			}
			fresh.emplace(peerFromUser(UserId(id)), TwitchStream{
				.login = entry.value(u"login"_q).toString(),
				.name = entry.value(u"name"_q).toString(),
				.title = entry.value(u"title"_q).toString(),
				.game = entry.value(u"game"_q).toString(),
				.viewers = entry.value(u"viewers"_q).toInt(),
			});
		}
		AnnounceStreams(fresh);
		auto &state = CurrentTwitch();
		const auto changed = (fresh.size() != state.live.size())
			|| ranges::any_of(fresh, [&](const auto &pair) {
				const auto i = state.live.find(pair.first);
				return (i == end(state.live))
					|| (i->second.title != pair.second.title)
					|| (i->second.viewers != pair.second.viewers);
			});
		state.live = std::move(fresh);
		if (changed) {
			state.updates.fire({});
		}
	});
}

void ShowTwitchBox(not_null<SessionController*> controller) {
	const auto settings = Data::LiquidStore::TwitchSettings();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Twitch в aglergram"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Друзья, которые стримят на Twitch, будут с красным кольцом "
			"и значком LIVE в списке чатов, а в профиле — название стрима "
			"и игра.\n\nЧтобы так показывался и твой эфир, открой "
			"blinistiy.fun/aglergram: войди через Twitch, подключи "
			"Telegram и включи «Показывать мой эфир»."_q,
			st::boxLabel));
		const auto notify = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Сообщать, когда друг начинает стрим"_q,
			settings.notify,
			st::defaultBoxCheckbox));
		const auto save = [=] {
			auto now = Data::LiquidStore::TwitchSettings();
			now.notify = notify->checked();
			Data::LiquidStore::SetTwitchSettings(now);
			RefreshTwitch();
		};
		box->addButton(rpl::single(u"Показывать мой эфир"_q), [=] {
			save();
			box->closeBox();
			UrlClickHandler::Open(
				QString::fromLatin1(kTwitchSite) + u"/aglergram"_q,
				QVariant::fromValue(ClickHandlerContext{
					.sessionWindow = base::make_weak(controller),
				}));
		});
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			save();
			box->closeBox();
		});
	}));
}

} // namespace Window::Liquid
