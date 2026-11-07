/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_updates.h"

#include "base/call_delayed.h"
#include "base/timer.h"
#include "core/application.h"
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
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkAccessManager>
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
	const auto reply = Network().get(request);
	const auto weak = base::make_weak(controller);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			strong->showToast(u"Не удалось скачать обновление"_q);
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
	rpl::event_stream<> frames;
};

Wallpaper &CurrentWallpaper() {
	static auto result = Wallpaper();
	return result;
}

void ApplyWallpaper(const QString &path) {
	auto &state = CurrentWallpaper();
	if (state.path == path && (state.reader || path.isEmpty())) {
		return;
	}
	state.path = path;
	state.reader = nullptr;
	state.frame = QImage();
	if (path.isEmpty()) {
		state.frames.fire({});
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
				const auto size = original.scaled(
					QSize(1280, 1280),
					Qt::KeepAspectRatio);
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
	if (path.startsWith(':')) {
		auto file = QFile(path);
		if (file.open(QIODevice::ReadOnly)) {
			state.reader = ::Media::Clip::MakeReader(file.readAll(), callback);
		}
	} else if (QFileInfo::exists(path)) {
		state.reader = ::Media::Clip::MakeReader(path, callback);
	}
	state.frames.fire({});
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
	ApplyWallpaper(Data::LiquidStore::LiveWallpaper());

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

void CheckForUpdates(not_null<SessionController*> controller, bool manual) {
	const auto repository = LoadedPatches().repository;
	if (repository.isEmpty()) {
		return;
	}
	auto request = QNetworkRequest(QUrl(
		u"https://api.github.com/repos/"_q + repository + u"/releases/latest"_q));
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("User-Agent", "aglergram");
	const auto reply = Network().get(request);
	const auto weak = base::make_weak(controller);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
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
		if (tag.isEmpty() || url.isEmpty() || !Newer(tag, CurrentVersion())) {
			if (manual) {
				strong->showToast(reply->error() == QNetworkReply::NoError
					? (u"У тебя последняя версия "_q + CurrentVersion())
					: u"Не удалось проверить обновления"_q);
			}
			return;
		}
		ShowUpdateBox(
			strong,
			tag.mid(int(strlen(kTagPrefix))),
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
		const auto current = Data::LiquidStore::LiveWallpaper();
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			current.isEmpty()
				? u"Сейчас: обычный фон чата."_q
				: (current == QString::fromLatin1(kSnowLeopard))
				? u"Сейчас: «Снежный барс»."_q
				: (u"Сейчас: "_q + QFileInfo(current).fileName()),
			st::boxLabel));
		const auto add = [&](const QString &text, Fn<void()> callback) {
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(text),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback(std::move(callback));
		};
		add(u"🐆 Снежный барс"_q, [=] {
			Data::LiquidStore::SetLiveWallpaper(QString::fromLatin1(kSnowLeopard));
			ApplyWallpaper(QString::fromLatin1(kSnowLeopard));
			box->closeBox();
		});
		add(u"🎞 Своё видео или GIF…"_q, [=] {
			const auto weak = base::make_weak(controller);
			FileDialog::GetOpenPath(
				controller->widget().get(),
				u"Живые обои"_q,
				u"Видео (*.mp4 *.webm *.mov *.gif)"_q,
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					// Keep a copy in tdata, the original may be moved.
					const auto source = result.paths.front();
					const auto target = Data::LiquidStore::Folder()
						+ u"liquid_wallpaper."_q
						+ QFileInfo(source).suffix().toLower();
					QFile::remove(target);
					const auto path = QFile::copy(source, target)
						? target
						: source;
					Data::LiquidStore::SetLiveWallpaper(path);
					ApplyWallpaper(path);
				});
			box->closeBox();
		});
		add(u"Убрать живые обои"_q, [=] {
			Data::LiquidStore::SetLiveWallpaper(QString());
			ApplyWallpaper(QString());
			box->closeBox();
		});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Видео крутится по кругу без звука и подстраивается под окно, "
			"стекло размывает его в шапке и поле ввода."_q,
			st::boxDividerLabel));
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

} // namespace Window::Liquid
