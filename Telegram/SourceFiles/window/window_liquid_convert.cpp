/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_convert.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/timer.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_liquid_store.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_unread_things.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/media_video_encode.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "mainwindow.h"
#include "window/window_session_controller.h"
#include "window/window_liquid_text_modes.h"
#include "window/window_liquid_plugins.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>

namespace Window::Liquid {
namespace {

constexpr auto kQuickRepliesKey = "quickReplies";
constexpr auto kSummaryKey = "daySummary";
constexpr auto kSummaryDayKey = "daySummaryShown";
constexpr auto kMaxQuickReplies = 8;
constexpr auto kRoundSize = 400;
constexpr auto kRoundMaxSeconds = 60;
constexpr auto kGifMaxSeconds = 120;
constexpr auto kGifShorterSide = 480;
constexpr auto kSummaryHour = 21;

[[nodiscard]] QString TempPath(const QString &name) {
	return QDir::tempPath()
		+ u"/aglergram_"_q
		+ name
		+ '_'
		+ QString::number(base::RandomValue<uint32>(), 16)
		+ u".mp4"_q;
}

void RemoveLater(const QString &path) {
	// The upload reads the file a bit later, keep it for a while.
	base::call_delayed(10 * 60 * crl::time(1000), [=] {
		QFile::remove(path);
	});
}

void Send(
		not_null<SessionController*> controller,
		PeerId peerId,
		ConvertKind kind,
		const Media::Encode::TranscodeResult &result) {
	const auto session = &controller->session();
	const auto history = session->data().history(peerId);
	auto action = Api::SendAction(history);
	if (kind == ConvertKind::Round) {
		auto file = QFile(result.path);
		if (!file.open(QIODevice::ReadOnly)) {
			controller->showToast(u"Не получилось прочитать кружок"_q);
			return;
		}
		auto bytes = file.readAll();
		file.close();
		QFile::remove(result.path);
		session->api().sendVoiceMessage(
			std::move(bytes),
			VoiceWaveform(),
			result.duration,
			true,
			action);
		controller->showToast(u"⭕ Кружок отправлен"_q);
	} else {
		auto list = Storage::PrepareMediaList(
			QStringList{ result.path },
			st::sendMediaPreviewSize,
			session->user()->isPremium());
		if (list.error != Ui::PreparedList::Error::None
			|| list.files.empty()) {
			QFile::remove(result.path);
			controller->showToast(u"Не получилось подготовить GIF"_q);
			return;
		}
		session->api().sendFiles(
			std::move(list),
			SendMediaType::File,
			nullptr,
			action);
		RemoveLater(result.path);
		controller->showToast(u"🎞 GIF отправлен"_q);
	}
}

void Transcode(
		not_null<SessionController*> controller,
		PeerId peerId,
		ConvertKind kind,
		QString path,
		crl::time from,
		crl::time length,
		Fn<void()> cleanup) {
	const auto weak = base::make_weak(controller);
	controller->showToast(kind == ConvertKind::Round
		? u"⭕ Делаю кружок…"_q
		: u"🎞 Делаю GIF…"_q);
	crl::async([=] {
		const auto info = Media::Encode::ProbeSource(path);
		auto result = Media::Encode::TranscodeResult();
		if (!info.empty()) {
			auto source = Media::Encode::VideoSource();
			source.path = path;
			const auto maxLength = crl::time(1000) * ((kind == ConvertKind::Round)
				? kRoundMaxSeconds
				: kGifMaxSeconds);
			const auto duration = info.duration > 0 ? info.duration : length;
			source.from = std::clamp(from, crl::time(0), std::max(duration - 500, crl::time(0)));
			source.till = std::min(
				source.from + std::min(length, maxLength),
				duration > 0 ? duration : (source.from + maxLength));
			if (kind == ConvertKind::Round) {
				const auto size = info.display.isEmpty()
					? info.coded
					: info.display;
				const auto side = std::min(size.width(), size.height());
				source.crop = QRect(
					(size.width() - side) / 2,
					(size.height() - side) / 2,
					side,
					side);
				source.exactSize = QSize(kRoundSize, kRoundSize);
			} else {
				source.removeAudio = true;
				source.targetShorterSide = kGifShorterSide;
			}
			result = Media::Encode::TranscodeVideo(source);
		}
		crl::on_main([=] {
			if (cleanup) {
				cleanup();
			}
			const auto strong = weak.get();
			if (!strong) {
				if (!result.empty()) {
					QFile::remove(result.path);
				}
				return;
			} else if (result.empty()) {
				strong->showToast(u"Не получилось перекодировать видео"_q);
				return;
			}
			Send(strong, peerId, kind, result);
		});
	});
}

void AskRange(
		not_null<SessionController*> controller,
		ConvertKind kind,
		Fn<void(crl::time from, crl::time length)> done) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(kind == ConvertKind::Round
			? u"⭕ Видео в кружок"_q
			: u"🎞 Видео в GIF"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			(kind == ConvertKind::Round)
				? u"Кружок вырежется из середины кадра, до 60 секунд."_q
				: u"GIF — без звука, до 2 минут."_q,
			st::boxDividerLabel));
		const auto start = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"С какой секунды (0 — с начала)"_q),
			u"0"_q));
		const auto length = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Сколько секунд"_q),
			QString::number((kind == ConvertKind::Round)
				? kRoundMaxSeconds
				: 15)));
		const auto submit = [=] {
			const auto seconds = [](const QString &text) {
				return text.trimmed().replace(',', '.').toDouble();
			};
			const auto from = seconds(start->getLastText());
			const auto len = seconds(length->getLastText());
			if (from < 0.) {
				start->showError();
				return;
			} else if (len <= 0.) {
				length->showError();
				return;
			}
			box->closeBox();
			done(crl::time(from * 1000), crl::time(len * 1000));
		};
		start->submits() | rpl::on_next(submit, start->lifetime());
		length->submits() | rpl::on_next(submit, length->lifetime());
		box->setFocusCallback([=] { start->setFocusFast(); });
		box->addButton(rpl::single(u"Отправить"_q), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

[[nodiscard]] PeerId OpenPeer(not_null<SessionController*> controller) {
	const auto key = controller->activeChatCurrent();
	const auto history = key.history();
	return history ? history->peer->id : PeerId();
}

// Quick replies strip.

class QuickStrip final : public Ui::RpWidget {
public:
	QuickStrip(QWidget *parent, Fn<void(QString)> send);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Pill {
		QString text;
		QRect rect;
		bool mode = false;
	};
	void refresh();
	void layout(int width);
	[[nodiscard]] int pillAt(QPoint point) const;

	Fn<void(QString)> _send;
	QStringList _phrases;
	std::vector<Pill> _pills;
	int _over = -1;
	int _pressed = -1;

};

constexpr auto kStripHeight = 34;
constexpr auto kPillHeight = 26;
constexpr auto kPillPadding = 12;
constexpr auto kPillSkip = 6;

QuickStrip::QuickStrip(QWidget *parent, Fn<void(QString)> send)
: RpWidget(parent)
, _send(std::move(send)) {
	setMouseTracking(true);
	refresh();
	Data::LiquidStore::ValueChanges(
	) | rpl::filter([](const QString &key) {
		return key == QString::fromLatin1(kQuickRepliesKey)
			|| key == u"plugins"_q;
	}) | rpl::on_next([=] {
		refresh();
	}, lifetime());
	TextModeValue() | rpl::skip(1) | rpl::on_next([=] {
		refresh();
	}, lifetime());
}

void QuickStrip::refresh() {
	_phrases = PluginEnabled(u"quickreplies"_q)
		? QuickReplies()
		: QStringList();
	resizeToWidth(width());
	update();
}

int QuickStrip::resizeGetHeight(int newWidth) {
	layout(newWidth);
	return (_phrases.isEmpty() && CurrentTextMode() == TextMode::None)
		? 0
		: style::ConvertScale(kStripHeight);
}

void QuickStrip::layout(int width) {
	_pills.clear();
	const auto &font = st::normalFont;
	const auto height = style::ConvertScale(kPillHeight);
	const auto padding = style::ConvertScale(kPillPadding);
	const auto skip = style::ConvertScale(kPillSkip);
	const auto top = (style::ConvertScale(kStripHeight) - height) / 2;
	auto left = skip * 2;
	if (const auto mode = CurrentTextMode(); mode != TextMode::None) {
		const auto text = TextModeTitle(mode) + u"  ✕"_q;
		const auto pill = font->width(text) + padding * 2;
		_pills.push_back({ text, QRect(left, top, pill, height), true });
		left += pill + skip;
	}
	for (const auto &phrase : _phrases) {
		auto text = phrase;
		const auto maxText = style::ConvertScale(180);
		if (font->width(text) > maxText) {
			text = font->elided(text, maxText);
		}
		const auto pill = font->width(text) + padding * 2;
		if (left + pill > width - skip * 2) {
			break;
		}
		_pills.push_back({ phrase, QRect(left, top, pill, height) });
		left += pill + skip;
	}
}

int QuickStrip::pillAt(QPoint point) const {
	for (auto i = 0; i != int(_pills.size()); ++i) {
		if (_pills[i].rect.contains(point)) {
			return i;
		}
	}
	return -1;
}

void QuickStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	PainterHighQualityEnabler hq(p);
	const auto &font = st::normalFont;
	p.setFont(font);
	const auto padding = style::ConvertScale(kPillPadding);
	for (auto i = 0; i != int(_pills.size()); ++i) {
		const auto &pill = _pills[i];
		auto fill = pill.mode ? st::activeButtonBg->c : st::windowBgOver->c;
		fill.setAlphaF((i == _over) ? 0.95 : 0.7);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		const auto radius = pill.rect.height() / 2.;
		p.drawRoundedRect(pill.rect, radius, radius);
		p.setPen(pill.mode ? st::activeButtonFg : st::windowFg);
		const auto text = font->elided(
			pill.text,
			pill.rect.width() - padding * 2);
		p.drawText(
			pill.rect.x() + padding,
			pill.rect.y() + (pill.rect.height() - font->height) / 2
				+ font->ascent,
			text);
	}
}

void QuickStrip::mouseMoveEvent(QMouseEvent *e) {
	const auto over = pillAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void QuickStrip::mousePressEvent(QMouseEvent *e) {
	_pressed = pillAt(e->pos());
}

void QuickStrip::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == pillAt(e->pos())) {
		if (_pills[pressed].mode) {
			SetTextMode(TextMode::None);
		} else if (_send) {
			_send(_pills[pressed].text);
		}
	}
}

void QuickStrip::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		setCursor(style::cur_default);
		update();
	}
}

[[nodiscard]] TimeId StartOfToday() {
	return TimeId(QDateTime(QDate::currentDate(), QTime(0, 0))
		.toSecsSinceEpoch());
}

} // namespace

void ConvertFromFile(
		not_null<SessionController*> controller,
		ConvertKind kind) {
	const auto peerId = OpenPeer(controller);
	if (!peerId) {
		controller->showToast(u"Сначала открой чат, куда отправить"_q);
		return;
	}
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		controller->widget()->window(),
		(kind == ConvertKind::Round)
			? u"Видео для кружка"_q
			: u"Видео для GIF"_q,
		u"Видео (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.gif);;"_q
			+ FileDialog::AllFilesFilter(),
		[=](FileDialog::OpenResult &&result) {
			const auto strong = weak.get();
			if (!strong || result.paths.isEmpty()) {
				return;
			}
			const auto path = result.paths.front();
			AskRange(strong, kind, [=](crl::time from, crl::time length) {
				if (const auto strong = weak.get()) {
					Transcode(strong, peerId, kind, path, from, length, nullptr);
				}
			});
		});
}

bool CanConvertMessage(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return document
		&& (document->isVideoFile() || document->isAnimation())
		&& !document->isVideoMessage();
}

void ConvertMessage(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item,
		ConvertKind kind) {
	const auto peerId = OpenPeer(controller);
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		return;
	} else if (!peerId) {
		controller->showToast(u"Сначала открой чат, куда отправить"_q);
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto view = document->createMediaView();
	const auto itemId = item->fullId();
	const auto go = [=](crl::time from, crl::time length) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto path = document->filepath(true);
		auto cleanup = Fn<void()>();
		if (path.isEmpty()) {
			const auto bytes = view->bytes();
			if (bytes.isEmpty()) {
				strong->showToast(u"Видео ещё не скачалось, попробуй "
					"через пару секунд"_q);
				document->save(itemId, QString());
				return;
			}
			path = TempPath(u"src"_q);
			auto file = QFile(path);
			if (!file.open(QIODevice::WriteOnly)) {
				return;
			}
			file.write(bytes);
			file.close();
			cleanup = [=] { QFile::remove(path); };
		}
		Transcode(strong, peerId, kind, path, from, length, cleanup);
	};
	if (!view->loaded() && document->filepath(true).isEmpty()) {
		document->save(itemId, QString());
	}
	AskRange(controller, kind, go);
}

QStringList QuickReplies() {
	auto result = QStringList();
	for (const auto value : Data::LiquidStore::Value(
			QString::fromLatin1(kQuickRepliesKey)).toArray()) {
		const auto text = value.toString().trimmed();
		if (!text.isEmpty()) {
			result.push_back(text);
		}
	}
	return result;
}

void ShowQuickRepliesBox(not_null<SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"⚡ Быстрые ответы"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Каждая строка — кнопка над полем ввода, клик сразу "
			"отправляет фразу. До 8 штук. Пусто — кнопок не будет."_q,
			st::boxDividerLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Ок\nСкоро буду\nПозже отвечу"_q),
			QuickReplies().join('\n')));
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			auto list = QJsonArray();
			for (const auto &line : field->getLastText().split('\n')) {
				const auto text = line.trimmed();
				if (!text.isEmpty() && list.size() < kMaxQuickReplies) {
					list.push_back(text.left(200));
				}
			}
			Data::LiquidStore::SetValue(
				QString::fromLatin1(kQuickRepliesKey),
				list.isEmpty() ? QJsonValue() : QJsonValue(list));
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

not_null<Ui::RpWidget*> CreateQuickRepliesStrip(
		not_null<Ui::RpWidget*> parent,
		Fn<void(QString)> send) {
	return Ui::CreateChild<QuickStrip>(parent.get(), std::move(send));
}

void ShowDaySummary(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	const auto today = StartOfToday();
	struct Row {
		not_null<History*> history;
		int unread = 0;
		int mentions = 0;
		bool wroteToday = false;
		TimeId date = 0;
	};
	auto rows = std::vector<Row>();
	auto unreadTotal = 0;
	auto mentionsTotal = 0;
	auto wroteTotal = 0;
	const auto list = session->data().chatsList()->indexed();
	for (const auto &row : *list) {
		const auto history = row->history();
		if (!history || history->peer->isSelf()) {
			continue;
		}
		const auto last = history->lastMessage();
		const auto wrote = last
			&& !last->out()
			&& (last->date() >= today);
		const auto unread = (history->unreadCount() > 0 && !history->muted())
			? history->unreadCount()
			: 0;
		const auto mentions = history->unreadMentions().count(0);
		if (!wrote && !unread && !mentions) {
			continue;
		}
		unreadTotal += unread;
		mentionsTotal += mentions;
		wroteTotal += wrote ? 1 : 0;
		rows.push_back({
			history,
			unread,
			mentions,
			wrote,
			last ? last->date() : TimeId(),
		});
	}
	ranges::sort(rows, [](const Row &a, const Row &b) {
		if ((a.mentions > 0) != (b.mentions > 0)) {
			return a.mentions > 0;
		}
		return a.date > b.date;
	});
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🌙 Сводка дня"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Сегодня писали: "_q + QString::number(wroteTotal)
				+ u" · непрочитанных: "_q + QString::number(unreadTotal)
				+ u" · упоминаний: "_q + QString::number(mentionsTotal),
			st::boxLabel));
		if (rows.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Всё прочитано, ничего не пропущено ✨"_q,
				st::boxDividerLabel));
		}
		auto shown = 0;
		for (const auto &row : rows) {
			const auto history = row.history;
			auto label = (row.mentions ? u"@ "_q : u"💬 "_q)
				+ history->peer->name();
			if (row.unread) {
				label += u" · "_q + QString::number(row.unread) + u" новых"_q;
			}
			if (row.mentions) {
				label += u" · упоминаний: "_q + QString::number(row.mentions);
			}
			if (const auto last = history->lastMessage()) {
				auto text = last->notificationText().text.simplified();
				if (text.size() > 70) {
					text = text.left(69) + QChar(0x2026);
				}
				if (!text.isEmpty()) {
					label += u"\n"_q + text;
				}
			}
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(label),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				box->closeBox();
				controller->showPeerHistory(history);
			});
			if (++shown >= 60) {
				break;
			}
		}
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

bool DaySummaryEnabled() {
	return Data::LiquidStore::Value(
		QString::fromLatin1(kSummaryKey)).toBool(false);
}

void SetDaySummaryEnabled(bool enabled) {
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kSummaryKey),
		enabled ? QJsonValue(true) : QJsonValue());
}

void StartDaySummary(not_null<SessionController*> controller) {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;
	const auto weak = base::make_weak(controller);
	static auto timer = base::Timer([=] {
		const auto strong = weak.get();
		if (!strong || !DaySummaryEnabled()) {
			return;
		}
		const auto now = QDateTime::currentDateTime();
		if (now.time().hour() < kSummaryHour) {
			return;
		}
		const auto day = now.date().toString(Qt::ISODate);
		const auto key = QString::fromLatin1(kSummaryDayKey);
		if (Data::LiquidStore::Value(key).toString() == day) {
			return;
		}
		Data::LiquidStore::SetValue(key, day);
		ShowDaySummary(strong);
	});
	timer.callEach(60 * crl::time(1000));
}

} // namespace Window::Liquid
