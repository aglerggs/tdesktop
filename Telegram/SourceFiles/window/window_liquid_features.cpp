/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_features.h"

#include "window/window_liquid_updates.h"
#include "window/window_liquid_extras.h"
#include "window/window_liquid_whisper.h"
#include "window/window_liquid_convert.h"
#include "window/window_liquid_plugins.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "data/data_liquid_store.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "lang/translate_provider.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "platform/platform_text_recognition.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtGui/QShortcut>
#include <QtWidgets/QTextEdit>

namespace Window::Liquid {
namespace {

constexpr auto kDownloadBot = "aglerloadbot";
constexpr auto kOcrScanLimit = 300;

[[nodiscard]] QString Preview(not_null<HistoryItem*> item) {
	auto text = item->originalText().text.simplified();
	if (text.isEmpty()) {
		text = item->media() ? u"[медиа]"_q : u"[сообщение]"_q;
	}
	return (text.size() > 120) ? (text.mid(0, 117) + u"..."_q) : text;
}

[[nodiscard]] Main::Session *FindSession(uint64 userId) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account->sessionExists()
			&& account->session().userId().bare == userId) {
			return &account->session();
		}
	}
	return nullptr;
}

[[nodiscard]] SessionController *FindController(
		not_null<Main::Session*> session) {
	if (const auto window = Core::App().windowFor(
			SeparateId(not_null(&session->account())))) {
		return window->sessionController();
	}
	return nullptr;
}

// Reminders.

void ShowReminder(const Data::LiquidStore::Reminder &reminder) {
	const auto session = FindSession(reminder.userId);
	if (!session) {
		return;
	}
	const auto controller = FindController(session);
	if (!controller) {
		return;
	}
	const auto peerId = reminder.peer;
	const auto msgId = reminder.msg;
	const auto title = reminder.title;
	const auto preview = reminder.preview;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Напоминание"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			title,
			st::boxDividerLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			preview,
			st::boxLabel));
		box->addButton(rpl::single(u"Открыть"_q), [=] {
			box->closeBox();
			if (const auto peer = session->data().peerLoaded(peerId)) {
				controller->showPeerHistory(
					peer,
					SectionShow::Way::Forward,
					msgId);
			}
		});
		box->addButton(rpl::single(u"Закрыть"_q), [=] {
			box->closeBox();
		});
	}));
	QApplication::alert(controller->widget());
}

void CheckReminders() {
	for (const auto &reminder : Data::LiquidStore::TakeDueReminders(
			base::unixtime::now())) {
		ShowReminder(reminder);
	}
}

void AddReminder(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item,
		TimeId at,
		const QString &when) {
	Data::LiquidStore::AddReminder({
		.userId = item->history()->session().userId().bare,
		.peer = item->history()->peer->id,
		.msg = item->id,
		.at = at,
		.title = item->history()->peer->name(),
		.preview = Preview(item),
	});
	controller->showToast(u"Напомню "_q + when);
}

// Night theme by the sunset.

[[nodiscard]] std::optional<float64> SunEventUtcHours(
		QDate date,
		float64 latitude,
		float64 longitude,
		bool sunrise) {
	constexpr auto kZenith = 90.833;
	const auto rad = M_PI / 180.;
	const auto day = date.dayOfYear();
	const auto lngHour = longitude / 15.;
	const auto t = day + ((sunrise ? 6. : 18.) - lngHour) / 24.;
	const auto m = 0.9856 * t - 3.289;
	auto l = m + 1.916 * std::sin(m * rad) + 0.020 * std::sin(2 * m * rad)
		+ 282.634;
	l = std::fmod(l + 360., 360.);
	auto ra = std::atan(0.91764 * std::tan(l * rad)) / rad;
	ra = std::fmod(ra + 360., 360.);
	ra += std::floor(l / 90.) * 90. - std::floor(ra / 90.) * 90.;
	ra /= 15.;
	const auto sinDec = 0.39782 * std::sin(l * rad);
	const auto cosDec = std::cos(std::asin(sinDec));
	const auto cosH = (std::cos(kZenith * rad)
		- sinDec * std::sin(latitude * rad))
		/ (cosDec * std::cos(latitude * rad));
	if (cosH > 1. || cosH < -1.) {
		return std::nullopt;
	}
	auto h = sunrise
		? (360. - std::acos(cosH) / rad)
		: (std::acos(cosH) / rad);
	h /= 15.;
	const auto localT = h + ra - 0.06571 * t - 6.622;
	return std::fmod(localT - lngHour + 48., 24.);
}

[[nodiscard]] bool IsNightNow(const Data::LiquidStore::Sunset &settings) {
	const auto now = QDateTime::currentDateTimeUtc();
	const auto hours = now.time().hour()
		+ now.time().minute() / 60.
		+ now.time().second() / 3600.;
	const auto rise = SunEventUtcHours(
		now.date(),
		settings.latitude,
		settings.longitude,
		true);
	const auto set = SunEventUtcHours(
		now.date(),
		settings.latitude,
		settings.longitude,
		false);
	if (!rise || !set) {
		// Polar day or night: winter months are dark.
		const auto month = now.date().month();
		const auto north = (settings.latitude >= 0.);
		const auto winter = (month <= 2 || month >= 11);
		return north ? winter : !winter;
	}
	return (*rise < *set)
		? (hours < *rise || hours >= *set)
		: (hours >= *set && hours < *rise);
}

void CrossfadeWindow(not_null<QWidget*> window) {
	const auto shot = Ui::GrabWidget(window);
	const auto overlay = Ui::CreateChild<Ui::RpWidget>(window.get());
	overlay->setGeometry(window->rect());
	overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
	const auto animation = overlay->lifetime().make_state<
		Ui::Animations::Simple>();
	overlay->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(overlay);
		p.setOpacity(animation->value(0.));
		p.drawPixmap(0, 0, shot);
	}, overlay->lifetime());
	animation->start([=] {
		overlay->update();
		if (!animation->animating()) {
			overlay->deleteLater();
		}
	}, 1., 0., crl::time(900), anim::easeOutCubic);
	overlay->show();
	overlay->raise();
}

void CheckSunset() {
	const auto settings = Data::LiquidStore::SunsetSettings();
	if (!settings.enabled) {
		return;
	}
	const auto night = IsNightNow(settings);
	if (night == Window::Theme::IsNightMode()) {
		return;
	}
	if (const auto window = Core::App().activePrimaryWindow()) {
		CrossfadeWindow(window->widget());
	}
	Window::Theme::ToggleNightMode();
}

// Notes.

[[nodiscard]] object_ptr<Ui::InputField> MakeMultiline(
		not_null<QWidget*> parent,
		const QString &placeholder,
		const QString &value) {
	auto result = object_ptr<Ui::InputField>(
		parent,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(placeholder),
		value);
	result->setMaxLength(4096);
	return result;
}

// Statistics.

struct Stats {
	int mine = 0;
	int theirs = 0;
	std::array<int, 24> hours = {};
	crl::time myResponse = 0;
	int myResponses = 0;
	crl::time theirResponse = 0;
	int theirResponses = 0;
	std::vector<std::pair<QString, int>> words;
	int total = 0;
};

[[nodiscard]] Stats CountStats(not_null<History*> history) {
	auto result = Stats();
	auto counts = base::flat_map<QString, int>();
	const auto splitter = QRegularExpression(u"[^\\w]+"_q);
	auto lastOut = std::optional<bool>();
	auto lastDate = TimeId();
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (item->isService()) {
				continue;
			}
			++result.total;
			const auto out = item->out();
			(out ? result.mine : result.theirs) += 1;
			const auto date = base::unixtime::parse(item->date());
			++result.hours[date.time().hour()];
			if (lastOut && *lastOut != out && lastDate) {
				const auto delta = item->date() - lastDate;
				if (delta > 0 && delta < 12 * 3600) {
					if (out) {
						result.myResponse += delta;
						++result.myResponses;
					} else {
						result.theirResponse += delta;
						++result.theirResponses;
					}
				}
			}
			lastOut = out;
			lastDate = item->date();
			const auto words = item->originalText().text.toLower().split(
				splitter,
				Qt::SkipEmptyParts);
			for (const auto &word : words) {
				if (word.size() >= 4) {
					++counts[word];
				}
			}
		}
	}
	for (const auto &[word, count] : counts) {
		result.words.emplace_back(word, count);
	}
	ranges::sort(result.words, ranges::greater(), [](const auto &pair) {
		return pair.second;
	});
	if (result.words.size() > 10) {
		result.words.resize(10);
	}
	return result;
}

[[nodiscard]] QString FormatDuration(int seconds) {
	if (seconds < 60) {
		return QString::number(seconds) + u" сек"_q;
	} else if (seconds < 3600) {
		return QString::number(seconds / 60) + u" мин"_q;
	}
	return QString::number(seconds / 3600) + u" ч "_q
		+ QString::number((seconds % 3600) / 60) + u" мин"_q;
}

// Quote as a picture.

[[nodiscard]] QImage RenderQuote(
		const std::vector<not_null<HistoryItem*>> &items) {
	constexpr auto kWidth = 760;
	constexpr auto kPadding = 36;
	constexpr auto kAvatar = 44;
	constexpr auto kBubblePadding = 14;
	const auto textWidth = kWidth - 2 * kPadding - kAvatar - 12
		- 2 * kBubblePadding;
	auto nameFont = st::semiboldFont->f;
	nameFont.setPixelSize(15);
	auto textFont = st::normalFont->f;
	textFont.setPixelSize(16);
	const auto nameMetrics = QFontMetrics(nameFont);
	const auto textMetrics = QFontMetrics(textFont);

	struct Layout {
		not_null<HistoryItem*> item;
		QString name;
		QString text;
		QRect textRect;
		int height = 0;
	};
	auto layouts = std::vector<Layout>();
	auto total = kPadding;
	for (const auto &item : items) {
		const auto from = item->from();
		const auto text = item->originalText().text.isEmpty()
			? Preview(item)
			: item->originalText().text;
		const auto rect = textMetrics.boundingRect(
			QRect(0, 0, textWidth, 100000),
			Qt::TextWordWrap,
			text);
		const auto height = std::max(
			kAvatar,
			kBubblePadding * 2 + nameMetrics.height() + 4 + rect.height());
		layouts.push_back({ item, from->name(), text, rect, height });
		total += height + 12;
	}
	total += kPadding - 12 + 22;

	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		QSize(kWidth, total) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	auto p = QPainter(&result);
	auto hq = PainterHighQualityEnabler(p);
	auto bg = QLinearGradient(0, 0, kWidth, total);
	bg.setColorAt(0., QColor(0x1d, 0x2b, 0x5a));
	bg.setColorAt(0.6, QColor(0x3a, 0x1f, 0x5c));
	bg.setColorAt(1., QColor(0x12, 0x3a, 0x4a));
	p.fillRect(QRect(0, 0, kWidth, total), bg);

	auto y = kPadding;
	for (auto &layout : layouts) {
		const auto from = layout.item->from();
		auto userpic = Ui::PeerUserpicView();
		from->paintUserpic(p, userpic, kPadding, y, kAvatar, true);
		const auto bubble = QRectF(
			kPadding + kAvatar + 12,
			y,
			std::max(
				layout.textRect.width(),
				nameMetrics.horizontalAdvance(layout.name))
				+ 2 * kBubblePadding,
			layout.height);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 34));
		p.drawRoundedRect(bubble, 18, 18);
		auto gloss = QLinearGradient(bubble.topLeft(), bubble.bottomLeft());
		gloss.setColorAt(0., QColor(255, 255, 255, 40));
		gloss.setColorAt(0.5, QColor(255, 255, 255, 0));
		p.setBrush(gloss);
		p.drawRoundedRect(bubble, 18, 18);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(255, 255, 255, 90), 1.));
		p.drawRoundedRect(bubble.marginsRemoved({ .5, .5, .5, .5 }), 18, 18);

		p.setFont(nameFont);
		p.setPen(QColor(0x9f, 0xc4, 0xff));
		p.drawText(
			QPointF(
				bubble.x() + kBubblePadding,
				bubble.y() + kBubblePadding + nameMetrics.ascent()),
			layout.name);
		p.setFont(textFont);
		p.setPen(QColor(255, 255, 255));
		p.drawText(
			QRectF(
				bubble.x() + kBubblePadding,
				bubble.y() + kBubblePadding + nameMetrics.height() + 4,
				textWidth,
				layout.textRect.height() + 4),
			Qt::TextWordWrap,
			layout.text);
		y += layout.height + 12;
	}
	auto small = st::normalFont->f;
	small.setPixelSize(12);
	p.setFont(small);
	p.setPen(QColor(255, 255, 255, 120));
	p.drawText(
		QRect(0, total - kPadding + 6, kWidth - kPadding, 20),
		Qt::AlignRight | Qt::AlignVCenter,
		u"aglergram"_q);
	p.end();
	return result;
}

// Download via the bot.

void SendToBot(
		not_null<SessionController*> controller,
		not_null<PeerData*> bot,
		const QString &url) {
	const auto history = bot->owner().history(bot);
	auto message = Api::MessageToSend(Api::SendAction(history));
	message.textWithTags = { url };
	bot->session().api().sendMessage(std::move(message));
	controller->showPeerHistory(bot, SectionShow::Way::Forward);
}

void DownloadViaBot(
		not_null<SessionController*> controller,
		const QString &url) {
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(kDownloadBot),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		const auto &data = result.data();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(data.vpeer()));
		if (const auto strong = weak.get(); strong && peer) {
			SendToBot(strong, peer, url);
		}
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Не удалось найти @aglerloadbot"_q);
		}
	}).send();
}

// Photo text search.

struct OcrState {
	base::flat_map<MsgId, QString> texts;
	std::vector<std::pair<MsgId, std::shared_ptr<Data::PhotoMedia>>> waiting;
	int running = 0;
	int scanned = 0;
	base::Timer timer;
	rpl::event_stream<> updated;
	bool saving = false;
};

} // namespace

void Start(not_null<SessionController*> controller) {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;
	StartUpdates(controller);
	static auto timer = base::Timer([] {
		CheckReminders();
		CheckSunset();
	});
	timer.callEach(20 * crl::time(1000));
	crl::on_main([] {
		CheckReminders();
		CheckSunset();
	});
}

void ShowNoteBox(
		not_null<SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto session = &peer->session();
	const auto peerId = peer->id;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Заметка: "_q + peer->name()));
		const auto field = box->addRow(MakeMultiline(
			box,
			u"Кто это, о чём договорились, что не забыть…"_q,
			Data::LiquidStore::Note(session, peerId)));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Видна только тебе, хранится на этом компьютере."_q,
			st::boxDividerLabel));
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			Data::LiquidStore::SetNote(
				session,
				peerId,
				field->getLastText());
			box->closeBox();
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void ShowStatsBox(
		not_null<SessionController*> controller,
		not_null<History*> history) {
	const auto stats = std::make_shared<Stats>(CountStats(history));
	const auto name = history->peer->name();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Статистика переписки"_q));
		box->setWidth(st::boxWideWidth);
		const auto chart = box->addRow(
			object_ptr<Ui::RpWidget>(box),
			style::margins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				0));
		chart->resize(st::boxWideWidth, 330);
		chart->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(chart);
			auto hq = PainterHighQualityEnabler(p);
			const auto w = chart->width();
			auto big = st::semiboldFont->f;
			big.setPixelSize(24);
			auto small = st::normalFont->f;
			small.setPixelSize(12);
			const auto card = [&](int x, int cw, QString value, QString label, QColor color) {
				const auto r = QRectF(x, 0, cw, 70);
				auto c = color;
				c.setAlphaF(0.16);
				p.setPen(Qt::NoPen);
				p.setBrush(c);
				p.drawRoundedRect(r, 14, 14);
				p.setPen(st::windowFg);
				p.setFont(big);
				p.drawText(r.adjusted(14, 8, -8, -28), Qt::AlignLeft | Qt::AlignVCenter, value);
				p.setFont(small);
				p.setPen(st::windowSubTextFg);
				p.drawText(r.adjusted(14, 42, -8, -8), Qt::AlignLeft | Qt::AlignVCenter, label);
			};
			const auto cw = (w - 16) / 3;
			card(0, cw, QString::number(stats->mine), u"мои сообщения"_q, QColor(0x3D, 0x8B, 0xFD));
			card(cw + 8, cw, QString::number(stats->theirs), u"сообщения: "_q + name, QColor(0xAF, 0x52, 0xDE));
			card(2 * (cw + 8), cw, stats->myResponses ? FormatDuration(int(stats->myResponse / stats->myResponses)) : u"—"_q, u"твой средний ответ"_q, QColor(0x34, 0xC7, 0x59));

			// Activity by hour.
			const auto top = 92;
			const auto height = 110;
			const auto maxHour = std::max(1, *ranges::max_element(stats->hours));
			const auto bw = w / 24.;
			for (auto h = 0; h != 24; ++h) {
				const auto value = stats->hours[h] / float64(maxHour);
				const auto bh = std::max(2., height * value);
				const auto bar = QRectF(h * bw + 2, top + height - bh, bw - 4, bh);
				auto color = QColor(0x3D, 0x8B, 0xFD);
				color.setAlphaF(0.35 + 0.65 * value);
				p.setPen(Qt::NoPen);
				p.setBrush(color);
				p.drawRoundedRect(bar, 3, 3);
				if (h % 3 == 0) {
					p.setFont(small);
					p.setPen(st::windowSubTextFg);
					p.drawText(QRectF(h * bw, top + height + 4, bw * 3, 16), Qt::AlignLeft, QString::number(h) + u":00"_q);
				}
			}
			p.setFont(small);
			p.setPen(st::windowSubTextFg);
			p.drawText(QRectF(0, top - 18, w, 16), Qt::AlignLeft, u"Активность по часам"_q);

			// Top words.
			auto y = top + height + 34;
			p.drawText(QRectF(0, y, w, 16), Qt::AlignLeft, u"Частые слова"_q);
			y += 20;
			auto x = 0.;
			auto wordFont = st::normalFont->f;
			wordFont.setPixelSize(13);
			const auto metrics = QFontMetrics(wordFont);
			p.setFont(wordFont);
			for (const auto &[word, count] : stats->words) {
				const auto text = word + u"  "_q + QString::number(count);
				const auto tw = metrics.horizontalAdvance(text) + 20;
				if (x + tw > w) {
					x = 0;
					y += 30;
				}
				const auto chip = QRectF(x, y, tw, 24);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawRoundedRect(chip, 12, 12);
				p.setPen(st::windowFg);
				p.drawText(chip, Qt::AlignCenter, text);
				x += tw + 6;
			}
		}, chart->lifetime());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"По загруженным сообщениям: "_q
				+ QString::number(stats->total)
				+ u". Пролистай чат выше, чтобы учесть больше."_q,
			st::boxDividerLabel));
		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
	}));
}

void ShowPhotoTextSearch(
		not_null<SessionController*> controller,
		not_null<History*> history) {
	if (!Platform::TextRecognition::IsAvailable()) {
		controller->showToast(
			u"Распознавание текста недоступно в этой системе"_q);
		return;
	}
	const auto session = &history->session();
	const auto peer = history->peer;
	const auto state = std::make_shared<OcrState>();
	state->texts = Data::LiquidStore::RecognizedTexts(session, peer->id);

	// Collect photos from the loaded part of the chat, newest first.
	for (auto b = history->blocks.rbegin(); b != history->blocks.rend(); ++b) {
		for (auto v = (*b)->messages.rbegin(); v != (*b)->messages.rend(); ++v) {
			const auto item = (*v)->data();
			const auto media = item->media();
			const auto photo = media ? media->photo() : nullptr;
			if (!photo || state->texts.contains(item->id)) {
				continue;
			} else if (int(state->waiting.size()) >= kOcrScanLimit) {
				break;
			}
			auto view = photo->createMediaView();
			view->wanted(Data::PhotoSize::Large, item->fullId());
			state->waiting.emplace_back(item->id, std::move(view));
		}
	}
	const auto save = [=] {
		Data::LiquidStore::SaveRecognizedTexts(session, peer->id, state->texts);
	};
	state->timer.setCallback([=] {
		for (auto i = begin(state->waiting); i != end(state->waiting);) {
			const auto image = i->second->image(Data::PhotoSize::Large);
			if (!image || state->running >= 2) {
				++i;
				continue;
			}
			const auto id = i->first;
			auto copy = image->original();
			i = state->waiting.erase(i);
			++state->running;
			const auto weak = std::weak_ptr<OcrState>(state);
			crl::async([=, copy = std::move(copy)] {
				const auto result = Platform::TextRecognition::RecognizeText(
					copy);
				auto text = QString();
				for (const auto &line : result.items) {
					text += line.text + ' ';
				}
				crl::on_main([=] {
					if (const auto strong = weak.lock()) {
						--strong->running;
						++strong->scanned;
						strong->texts[id] = text.trimmed();
						save();
						strong->updated.fire({});
					}
				});
			});
		}
		if (state->waiting.empty() && !state->running) {
			state->timer.cancel();
		}
	});
	state->timer.callEach(400);

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Поиск по тексту на фото"_q));
		box->setWidth(st::boxWideWidth);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Что написано на фото?"_q)));
		const auto status = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			QString(),
			st::boxDividerLabel));
		const auto results = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto refresh = [=] {
			while (results->count()) {
				delete results->widgetAt(0);
			}
			status->setText(u"Распознано фото: "_q
				+ QString::number(state->texts.size())
				+ (state->waiting.empty() && !state->running
					? QString()
					: (u", в очереди: "_q
						+ QString::number(state->waiting.size()
							+ state->running))));
			const auto query = field->getLastText().trimmed().toLower();
			if (query.size() < 2) {
				return;
			}
			auto found = 0;
			for (auto i = state->texts.rbegin(); i != state->texts.rend(); ++i) {
				const auto &[id, text] = *i;
				const auto index = text.toLower().indexOf(query);
				if (index < 0) {
					continue;
				}
				const auto from = std::max(0, int(index) - 40);
				const auto snippet = (from ? u"…"_q : QString())
					+ text.mid(from, 120);
				const auto button = results->add(
					object_ptr<Ui::SettingsButton>(
						results,
						rpl::single(snippet),
						st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					box->closeBox();
					controller->showPeerHistory(
						peer,
						SectionShow::Way::Forward,
						id);
				});
				if (++found >= 30) {
					break;
				}
			}
			if (!found) {
				results->add(object_ptr<Ui::FlatLabel>(
					results,
					u"Ничего не найдено"_q,
					st::boxDividerLabel),
					st::boxRowPadding);
			}
			results->resizeToWidth(box->width());
		};
		field->changes() | rpl::on_next(refresh, field->lifetime());
		state->updated.events() | rpl::on_next(refresh, box->lifetime());
		refresh();
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Готово"_q), [=] { box->closeBox(); });
		box->boxClosing() | rpl::on_next([=] {
			state->timer.cancel();
		}, box->lifetime());
		box->lifetime().add([state] {});
	}));
}

void OpenSideBySide(
		not_null<SessionController*> controller,
		not_null<Data::Thread*> thread) {
	const auto id = SeparateId(thread);
	controller->showInNewWindow(id);
	const auto main = controller->widget();
	crl::on_main(main, [=] {
		const auto screen = main->screen();
		if (!screen) {
			return;
		}
		const auto area = screen->availableGeometry();
		const auto half = area.width() / 2;
		if (main->isMaximized()) {
			main->showNormal();
		}
		main->setGeometry(QRect(area.x(), area.y(), half, area.height()));
		if (const auto window = Core::App().separateWindowFor(id)) {
			const auto widget = window->widget();
			widget->setGeometry(QRect(
				area.x() + half,
				area.y(),
				area.width() - half,
				area.height()));
			widget->activateWindow();
		}
	});
}

void FillMessageMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<SessionController*> controller,
		HistoryItem *item,
		std::vector<not_null<HistoryItem*>> selected,
		const QString &link) {
	const auto url = link.trimmed();
	if (url.startsWith(u"http://"_q, Qt::CaseInsensitive)
		|| url.startsWith(u"https://"_q, Qt::CaseInsensitive)) {
		menu->addAction(u"Скачать через @aglerloadbot"_q, [=] {
			DownloadViaBot(controller, url);
		}, &st::menuIconDownload);
	}
	if (selected.empty() && item && item->isRegular()) {
		selected.push_back(item);
	}
	if (!selected.empty()) {
		const auto items = selected;
		menu->addAction(u"Цитата картинкой"_q, [=] {
			auto sorted = items;
			ranges::sort(sorted, ranges::less(), [](not_null<HistoryItem*> i) {
				return i->date();
			});
			if (sorted.size() > 12) {
				sorted.erase(sorted.begin() + 12, sorted.end());
			}
			QGuiApplication::clipboard()->setImage(RenderQuote(sorted));
			controller->showToast(
				u"Картинка скопирована, вставь её в любой чат"_q);
		}, &st::menuIconPhoto);
	}
	if (!item || !item->isRegular()) {
		return;
	}
	const auto session = &item->history()->session();
	const auto itemId = item->fullId();
	if (CanForwardFromOtherAccount(controller, item)) {
		menu->addAction(u"↪️ Отправить с другого аккаунта"_q, [=] {
			if (const auto strong = session->data().message(itemId)) {
				ForwardFromOtherAccount(controller, strong);
			}
		}, &st::menuIconForward);
	}
	if (PluginEnabled(u"convert"_q) && CanConvertMessage(item)) {
		menu->addAction(u"⭕ Отправить кружком"_q, [=] {
			if (const auto strong = session->data().message(itemId)) {
				ConvertMessage(controller, strong, ConvertKind::Round);
			}
		}, &st::menuIconVideoChat);
		menu->addAction(u"🎞 Отправить как GIF"_q, [=] {
			if (const auto strong = session->data().message(itemId)) {
				ConvertMessage(controller, strong, ConvertKind::Gif);
			}
		}, &st::menuIconGif);
	}
	if (PluginEnabled(u"whisper"_q) && CanTranscribeLocally(item)) {
		menu->addAction(u"🎙 В текст (Whisper)"_q, [=] {
			if (const auto strong = session->data().message(itemId)) {
				TranscribeLocally(controller, strong);
			}
		}, &st::menuIconTranslate);
	}
	if (PluginEnabled(u"bookmarks"_q)) menu->addAction(
		IsBookmarked(item) ? u"Убрать из закладок"_q : u"В закладки ⭐"_q,
		[=] {
			if (const auto strong = session->data().message(itemId)) {
				ToggleBookmark(controller, strong);
			}
		},
		&st::menuIconFave);
	auto submenu = std::make_unique<Ui::PopupMenu>(
		menu->parentWidget(),
		menu->st());
	const auto add = [&](const QString &text, Fn<TimeId()> at, QString when) {
		submenu->addAction(text, [=] {
			if (const auto strong = session->data().message(itemId)) {
				AddReminder(controller, strong, at(), when);
			}
		});
	};
	add(u"Через 1 час"_q, [] {
		return base::unixtime::now() + 3600;
	}, u"через час"_q);
	add(u"Через 3 часа"_q, [] {
		return base::unixtime::now() + 3 * 3600;
	}, u"через 3 часа"_q);
	add(u"Сегодня в 20:00"_q, [] {
		auto when = QDateTime(QDate::currentDate(), QTime(20, 0));
		if (when <= QDateTime::currentDateTime()) {
			when = when.addDays(1);
		}
		return TimeId(when.toSecsSinceEpoch());
	}, u"в 20:00"_q);
	add(u"Завтра в 9:00"_q, [] {
		return TimeId(QDateTime(
			QDate::currentDate().addDays(1),
			QTime(9, 0)).toSecsSinceEpoch());
	}, u"завтра в 9:00"_q);
	add(u"Через неделю"_q, [] {
		return base::unixtime::now() + 7 * 86400;
	}, u"через неделю"_q);
	menu->addAction(
		u"Напомнить"_q,
		std::move(submenu),
		&st::menuIconSchedule);
}

void SetupFieldFeatures(
		not_null<Ui::InputField*> field,
		Fn<SessionController*()> controller) {
	// `;name` followed by a space expands into the template text.
	field->changes() | rpl::on_next([=] {
		const auto &templates = Data::LiquidStore::Templates();
		if (templates.empty()) {
			return;
		}
		const auto edit = field->rawTextEdit();
		auto cursor = edit->textCursor();
		if (cursor.hasSelection()) {
			return;
		}
		const auto position = cursor.position();
		const auto block = cursor.block();
		const auto before = block.text().left(position - block.position());
		static const auto regex = QRegularExpression(u";(\\S+)\\s$"_q);
		const auto match = regex.match(before);
		if (!match.hasMatch()) {
			return;
		}
		const auto name = match.captured(1);
		const auto i = ranges::find(templates, name, &Data::LiquidStore::Template::name);
		if (i == end(templates)) {
			return;
		}
		const auto text = i->text;
		crl::on_main(field, [=] {
			auto cursor = edit->textCursor();
			cursor.movePosition(
				QTextCursor::Left,
				QTextCursor::KeepAnchor,
				match.capturedLength(0));
			cursor.insertText(text);
			edit->setTextCursor(cursor);
		});
	}, field->lifetime());

	// Ctrl+Shift+T translates the text, pressing it again brings it back.
	struct TranslateState {
		std::unique_ptr<Ui::TranslateProvider> provider;
		TextWithTags original;
		bool translated = false;
	};
	const auto state = field->lifetime().make_state<TranslateState>();
	const auto shortcut = new QShortcut(
		QKeySequence(u"Ctrl+Shift+T"_q),
		field,
		nullptr,
		nullptr,
		Qt::WidgetWithChildrenShortcut);
	QObject::connect(shortcut, &QShortcut::activated, field, [=] {
		const auto window = controller();
		if (!window) {
			return;
		}
		if (state->translated) {
			field->setTextWithTags(state->original);
			field->setCursorPosition(field->getLastText().size());
			state->translated = false;
			return;
		}
		const auto current = field->getTextWithTags();
		if (current.text.trimmed().isEmpty()) {
			return;
		}
		if (!state->provider) {
			state->provider = Ui::CreateTranslateProvider(&window->session());
		}
		const auto weak = base::make_weak(window);
		const auto to = LanguageId::FromName(
			Data::LiquidStore::TranslateTo());
		state->provider->request(
			{ .text = { current.text, {} } },
			to,
			crl::guard(field, [=](Ui::TranslateProviderResult result) {
				if (!result.text) {
					if (const auto strong = weak.get()) {
						strong->showToast(u"Не удалось перевести"_q);
					}
					return;
				}
				state->original = current;
				state->translated = true;
				field->setTextWithTags({ result.text->text, {} });
				field->setCursorPosition(field->getLastText().size());
				if (const auto strong = weak.get()) {
					strong->showToast(
						u"Переведено, Ctrl+Shift+T — вернуть оригинал"_q);
				}
			}));
	});
}

void CreateUnreadFolder(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	auto &filters = session->data().chatsFilters();
	auto maxId = FilterId(1);
	for (const auto &filter : filters.list()) {
		if (filter.title().text.text == u"Непрочитанное"_q) {
			controller->showToast(u"Папка «Непрочитанное» уже есть"_q);
			return;
		}
		maxId = std::max(maxId, filter.id());
	}
	using Flag = Data::ChatFilter::Flag;
	const auto filter = Data::ChatFilter(
		maxId + 1,
		Data::ChatFilterTitle{ .text = { u"Непрочитанное"_q } },
		u"📬"_q,
		std::nullopt,
		(Flag::Contacts
			| Flag::NonContacts
			| Flag::Groups
			| Flag::Channels
			| Flag::Bots
			| Flag::NoRead
			| Flag::NoArchived),
		{},
		{},
		{});
	filters.set(filter);
	session->api().request(MTPmessages_UpdateDialogFilter(
		MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
		MTP_int(filter.id()),
		filter.tl()
	)).send();
	controller->showToast(
		u"Готово: папка «Непрочитанное» сама собирает чаты с новыми сообщениями"_q);
}

void ReadAllChats(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	auto count = 0;
	const auto list = session->data().chatsList()->indexed();
	for (const auto &row : *list) {
		if (const auto history = row->history()) {
			if (history->unreadCount() > 0 || history->unreadMark()) {
				session->data().histories().readInbox(history);
				++count;
			}
		}
	}
	controller->showToast(count
		? (u"Прочитано чатов: "_q + QString::number(count))
		: u"Непрочитанных чатов нет"_q);
}

void ShowTemplatesBox(not_null<SessionController*> controller) {
	auto initial = QString();
	for (const auto &entry : Data::LiquidStore::Templates()) {
		initial += entry.name + u" = "_q + entry.text + '\n';
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Шаблоны текста"_q));
		const auto field = box->addRow(MakeMultiline(
			box,
			u"адрес = ул. Пушкина, д. 1"_q,
			initial.trimmed()));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Каждая строка: имя = текст. В поле сообщения напиши ;имя "
			"и пробел — подставится текст."_q,
			st::boxDividerLabel));
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			auto list = std::vector<Data::LiquidStore::Template>();
			for (const auto &line : field->getLastText().split('\n')) {
				const auto index = line.indexOf('=');
				if (index <= 0) {
					continue;
				}
				auto name = line.left(index).trimmed();
				if (name.startsWith(';')) {
					name = name.mid(1);
				}
				const auto text = line.mid(index + 1).trimmed();
				if (!name.isEmpty() && !name.contains(' ') && !text.isEmpty()) {
					list.push_back({ name, text });
				}
			}
			Data::LiquidStore::SetTemplates(std::move(list));
			box->closeBox();
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

void ShowSunsetBox(not_null<SessionController*> controller) {
	const auto settings = Data::LiquidStore::SunsetSettings();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Ночная тема по закату"_q));
		const auto enabled = box->addRow(object_ptr<Ui::Checkbox>(
			box,
			u"Включать ночную тему после заката"_q,
			settings.enabled,
			st::defaultBoxCheckbox));
		const auto latitude = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Широта"_q),
			QString::number(settings.latitude, 'f', 4)));
		const auto longitude = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Долгота"_q),
			QString::number(settings.longitude, 'f', 4)));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Координаты твоего города, например Москва — 55.75 и 37.62. "
			"Тема плавно переключится на закате и на рассвете."_q,
			st::boxDividerLabel));
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			auto result = Data::LiquidStore::Sunset{
				.enabled = enabled->checked(),
				.latitude = std::clamp(
					QString(latitude->getLastText()).replace(',', '.').toDouble(),
					-89.,
					89.),
				.longitude = std::clamp(
					QString(longitude->getLastText()).replace(',', '.').toDouble(),
					-180.,
					180.),
			};
			Data::LiquidStore::SetSunsetSettings(result);
			box->closeBox();
			CheckSunset();
		});
		box->addButton(rpl::single(u"Отмена"_q), [=] { box->closeBox(); });
	}));
}

} // namespace Window::Liquid
