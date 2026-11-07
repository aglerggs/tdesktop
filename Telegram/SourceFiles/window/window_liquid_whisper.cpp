/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_whisper.h"

#include "base/zlib_help.h"
#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_liquid_store.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonValue>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QThread>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Window::Liquid {
namespace {

constexpr auto kProgramUrl = "https://github.com/ggml-org/whisper.cpp/"
	"releases/download/v1.7.6/whisper-bin-x64.zip";
constexpr auto kModelUrl = "https://huggingface.co/ggerganov/whisper.cpp/"
	"resolve/main/ggml-%1.bin";
constexpr auto kModelKey = "whisperModel";
constexpr auto kSampleRate = 16000;
constexpr auto kMaxZip = 200 * 1024 * 1024;

struct Model {
	QString name;
	QString title;
};

[[nodiscard]] const std::vector<Model> &Models() {
	static const auto result = std::vector<Model>{
		{ u"base"_q, u"Быстрая, ≈150 МБ"_q },
		{ u"small"_q, u"Точнее, ≈490 МБ"_q },
	};
	return result;
}

[[nodiscard]] QString Folder() {
	return Data::LiquidStore::Folder() + u"whisper/"_q;
}

[[nodiscard]] QString ProgramPath() {
#ifdef Q_OS_WIN
	return Folder() + u"whisper-cli.exe"_q;
#else // Q_OS_WIN
	return Folder() + u"whisper-cli"_q;
#endif // Q_OS_WIN
}

[[nodiscard]] QString ModelName() {
	const auto value = Data::LiquidStore::Value(
		QString::fromLatin1(kModelKey)).toString();
	return value.isEmpty() ? u"base"_q : value;
}

[[nodiscard]] QString ModelPath(const QString &name) {
	return Folder() + u"ggml-"_q + name + u".bin"_q;
}

[[nodiscard]] bool Ready() {
	return QFile::exists(ProgramPath()) && QFile::exists(ModelPath(ModelName()));
}

[[nodiscard]] QNetworkAccessManager &Network() {
	static auto result = QNetworkAccessManager();
	return result;
}

struct State {
	bool downloading = false;
	base::flat_map<FullMsgId, QString> results;
	base::flat_set<FullMsgId> running;
	base::flat_map<FullMsgId, rpl::lifetime> waiting;
};

[[nodiscard]] State &Current() {
	static auto result = State();
	return result;
}

// Streams a file to disk, `.part` until it is complete.
void Download(
		const QString &url,
		const QString &path,
		Fn<void(int64, int64)> progress,
		Fn<void(bool)> done) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	const auto file = std::make_shared<QFile>(path + u".part"_q);
	if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		done(false);
		return;
	}
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("User-Agent", "aglergram");
	const auto reply = Network().get(request);
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		file->write(reply->readAll());
	});
	QObject::connect(
		reply,
		&QNetworkReply::downloadProgress,
		[=](qint64 received, qint64 total) {
			progress(received, total);
		});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		file->write(reply->readAll());
		file->close();
		const auto ok = (reply->error() == QNetworkReply::NoError)
			&& (file->size() > 0);
		if (ok) {
			QFile::remove(path);
			done(QFile::rename(file->fileName(), path));
		} else {
			file->remove();
			done(false);
		}
	});
}

// Takes the programs and libraries out of the release zip.
[[nodiscard]] bool UnpackProgram(const QString &zipPath) {
	auto input = QFile(zipPath);
	if (!input.open(QIODevice::ReadOnly) || input.size() > kMaxZip) {
		return false;
	}
	auto zip = zlib::FileToRead(input.readAll());
	input.close();
	auto found = false;
	if (zip.goToFirstFile() != UNZ_OK) {
		return false;
	}
	do {
		const auto name = zip.getCurrentFileName();
		const auto base = QFileInfo(name).fileName();
		const auto lower = base.toLower();
		if (!base.isEmpty()
			&& (lower.endsWith(u".exe"_q) || lower.endsWith(u".dll"_q))) {
			const auto content = zip.readCurrentFileContent(kMaxZip);
			if (zip.error() != UNZ_OK) {
				return false;
			}
			auto output = QFile(Folder() + base);
			if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				return false;
			}
			output.write(content);
			if (lower == u"whisper-cli.exe"_q) {
				found = true;
			}
		}
	} while (zip.goToNextFile() == UNZ_OK);
	return found;
}

void Install(
		not_null<SessionController*> controller,
		const QString &model,
		Fn<void()> done) {
	auto &state = Current();
	if (state.downloading) {
		controller->showToast(u"Whisper уже скачивается…"_q);
		return;
	}
	state.downloading = true;
	Data::LiquidStore::SetValue(QString::fromLatin1(kModelKey), model);
	const auto weak = base::make_weak(controller);
	const auto label = std::make_shared<QPointer<Ui::FlatLabel>>();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Скачиваю Whisper"_q));
		*label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Подготовка…"_q,
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Окно можно закрыть, загрузка продолжится."_q,
			st::boxDividerLabel));
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
	const auto show = [=](const QString &text) {
		if (const auto strong = label->data()) {
			strong->setText(text);
		}
	};
	const auto fail = [=](const QString &text) {
		Current().downloading = false;
		show(text);
		if (const auto strong = weak.get()) {
			strong->showToast(text);
		}
	};
	const auto progress = [=](const QString &what) {
		return [=](int64 received, int64 total) {
			const auto mb = [](int64 bytes) {
				return QString::number(bytes / (1024 * 1024));
			};
			show(what + u": "_q + mb(received)
				+ (total > 0 ? (u" из "_q + mb(total)) : QString())
				+ u" МБ"_q);
		};
	};
	const auto modelStep = [=] {
		if (QFile::exists(ModelPath(model))) {
			Current().downloading = false;
			show(u"Готово ✅"_q);
			done();
			return;
		}
		Download(
			QString::fromLatin1(kModelUrl).arg(model),
			ModelPath(model),
			progress(u"Модель"_q),
			[=](bool ok) {
				if (!ok) {
					fail(u"Не удалось скачать модель Whisper"_q);
					return;
				}
				Current().downloading = false;
				show(u"Готово ✅"_q);
				done();
			});
	};
	if (QFile::exists(ProgramPath())) {
		modelStep();
		return;
	}
	const auto zipPath = Folder() + u"whisper-bin-x64.zip"_q;
	Download(
		QString::fromLatin1(kProgramUrl),
		zipPath,
		progress(u"Программа"_q),
		[=](bool ok) {
			const auto unpacked = ok && UnpackProgram(zipPath);
			QFile::remove(zipPath);
			if (!unpacked) {
				fail(u"Не удалось скачать программу Whisper"_q);
				return;
			}
			modelStep();
		});
}

// Any audio Telegram can play -> 16 kHz mono 16-bit WAV for whisper.
[[nodiscard]] bool WriteWav(
		const Core::FileLocation &location,
		const QByteArray &bytes,
		const QString &path) {
	const auto pcm = audioDecodeMono(location, bytes, kSampleRate);
	if (pcm.size() < kSampleRate / 4) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto dataSize = uint32(pcm.size() * 2);
	auto header = QByteArray();
	const auto u32 = [&](uint32 value) {
		header.append(reinterpret_cast<const char*>(&value), 4);
	};
	const auto u16 = [&](uint16 value) {
		header.append(reinterpret_cast<const char*>(&value), 2);
	};
	header.append("RIFF");
	u32(36 + dataSize);
	header.append("WAVEfmt ");
	u32(16);
	u16(1);
	u16(1);
	u32(kSampleRate);
	u32(kSampleRate * 2);
	u16(2);
	u16(16);
	header.append("data");
	u32(dataSize);
	file.write(header);
	file.write(reinterpret_cast<const char*>(pcm.data()), dataSize);
	return true;
}

void ShowResult(
		not_null<SessionController*> controller,
		const QString &text) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🎙 Расшифровка"_q));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			text.isEmpty() ? u"Слов не разобрать."_q : text,
			st::boxLabel));
		label->setSelectable(true);
		box->addButton(rpl::single(u"Копировать"_q), [=] {
			QGuiApplication::clipboard()->setText(text);
			controller->showToast(u"Скопировано"_q);
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void Run(
		not_null<SessionController*> controller,
		FullMsgId id,
		Core::FileLocation location,
		QByteArray bytes) {
	const auto weak = base::make_weak(controller);
	const auto wav = Folder()
		+ u"voice_"_q
		+ QString::number(id.msg.bare)
		+ u".wav"_q;
	const auto fail = [=](const QString &text) {
		Current().running.remove(id);
		if (const auto strong = weak.get()) {
			strong->showToast(text);
		}
	};
	crl::async([=] {
		const auto ok = WriteWav(location, bytes, wav);
		crl::on_main([=] {
			if (!ok) {
				fail(u"Не удалось прочитать звук сообщения"_q);
				return;
			}
			const auto process = new QProcess();
			process->setWorkingDirectory(Folder());
#ifdef Q_OS_WIN
			process->setCreateProcessArgumentsModifier([](
					QProcess::CreateProcessArguments *args) {
				args->flags |= CREATE_NO_WINDOW;
			});
#endif // Q_OS_WIN
			QObject::connect(
				process,
				&QProcess::finished,
				[=](int code, QProcess::ExitStatus status) {
					process->deleteLater();
					QFile::remove(wav);
					auto text = QString::fromUtf8(
						process->readAllStandardOutput()).trimmed();
					text.replace(QRegularExpression(u"\\s*\\n\\s*"_q), u" "_q);
					if (status != QProcess::NormalExit || code != 0) {
						fail(u"Whisper не справился, попробуй ещё раз"_q);
						return;
					}
					Current().running.remove(id);
					Current().results[id] = text;
					if (const auto strong = weak.get()) {
						ShowResult(strong, text);
					}
				});
			QObject::connect(
				process,
				&QProcess::errorOccurred,
				[=](QProcess::ProcessError error) {
					if (error == QProcess::FailedToStart) {
						process->deleteLater();
						QFile::remove(wav);
						fail(u"Whisper не запустился"_q);
					}
				});
			const auto threads = std::clamp(
				QThread::idealThreadCount() - 1,
				1,
				8);
			process->start(ProgramPath(), {
				u"-m"_q, ModelPath(ModelName()),
				u"-f"_q, wav,
				u"-l"_q, u"auto"_q,
				u"-nt"_q,
				u"-np"_q,
				u"-t"_q, QString::number(threads),
			});
			if (const auto strong = weak.get()) {
				strong->showToast(u"🎙 Расшифровываю…"_q);
			}
		});
	});
}

void Start(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		return;
	}
	const auto id = item->fullId();
	auto &state = Current();
	if (const auto i = state.results.find(id); i != end(state.results)) {
		ShowResult(controller, i->second);
		return;
	} else if (state.running.contains(id)) {
		controller->showToast(u"Уже расшифровываю…"_q);
		return;
	}
	state.running.emplace(id);
	const auto view = document->createMediaView();
	const auto go = [=] {
		const auto location = document->location(true);
		Run(controller, id, location, view->bytes());
		crl::on_main([=] { Current().waiting.remove(id); });
	};
	if (view->loaded()) {
		go();
		return;
	}
	document->save(id, QString());
	auto &lifetime = state.waiting[id];
	document->session().downloaderTaskFinished(
	) | rpl::filter([=] {
		return view->loaded();
	}) | rpl::take(1) | rpl::on_next(go, lifetime);
}

} // namespace

bool CanTranscribeLocally(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return document
		&& (document->isVoiceMessage() || document->isVideoMessage());
}

void TranscribeLocally(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item) {
	if (!CanTranscribeLocally(item)) {
		return;
	}
	if (Ready()) {
		Start(controller, item);
		return;
	}
	const auto session = &item->history()->session();
	const auto id = item->fullId();
	const auto weak = base::make_weak(controller);
	const auto install = [=](const QString &model) {
		Install(controller, model, [=] {
			const auto strong = weak.get();
			const auto item = session->data().message(id);
			if (strong && item) {
				Start(strong, item);
			}
		});
	};
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🎙 Голосовые в текст"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Расшифровка идёт прямо на этом компьютере через Whisper, "
			"без Premium и без отправки звука куда-либо. Один раз нужно "
			"скачать программу и модель распознавания."_q,
			st::boxLabel));
		for (const auto &model : Models()) {
			const auto name = model.name;
			box->addButton(rpl::single(model.title), [=] {
				box->closeBox();
				install(name);
			});
		}
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Window::Liquid
