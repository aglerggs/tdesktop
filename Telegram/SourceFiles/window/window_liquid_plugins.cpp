/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_plugins.h"

#include "window/window_liquid_convert.h"
#include "window/window_liquid_extras.h"
#include "window/window_liquid_text_modes.h"
#include "window/window_liquid_trail.h"
#include "window/window_liquid_updates.h"
#include "window/window_liquid_music.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include <QtCore/QRegularExpression>
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_liquid_store.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio_voice_fx.h"
#include "ui/effects/liquid_round_frame.h"
#include "ui/widgets/checkbox.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "mainwindow.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

namespace Window::Liquid {
namespace {

constexpr auto kPluginsKey = "plugins";
constexpr auto kHistoryLimit = 100;
constexpr auto kHistoryDelay = crl::time(250);
constexpr auto kMaxSavedMessages = 200000;

// ChatSaver.

struct SavedMessage {
	int id = 0;
	TimeId date = 0;
	QString from;
	bool out = false;
	QString text;
	QString media;
};

struct SaveState {
	not_null<History*> history;
	QString path;
	bool html = false;
	int offsetId = 0;
	std::vector<SavedMessage> messages;
	bool cancelled = false;
	QPointer<Ui::FlatLabel> label;
	QPointer<Ui::GenericBox> box;
	base::weak_ptr<SessionController> controller;
};

[[nodiscard]] QString MediaTitle(const MTPMessageMedia &media) {
	switch (media.type()) {
	case mtpc_messageMediaPhoto: return u"[фото]"_q;
	case mtpc_messageMediaDocument: return u"[файл]"_q;
	case mtpc_messageMediaGeo:
	case mtpc_messageMediaGeoLive:
	case mtpc_messageMediaVenue: return u"[место]"_q;
	case mtpc_messageMediaContact: return u"[контакт]"_q;
	case mtpc_messageMediaPoll: return u"[опрос]"_q;
	case mtpc_messageMediaDice: return u"[кубик]"_q;
	case mtpc_messageMediaWebPage:
	case mtpc_messageMediaEmpty: return QString();
	}
	return u"[медиа]"_q;
}

[[nodiscard]] QString Escape(QString text) {
	return text
		.replace('&', u"&amp;"_q)
		.replace('<', u"&lt;"_q)
		.replace('>', u"&gt;"_q)
		.replace('\n', u"<br>"_q);
}

void WriteSaved(const std::shared_ptr<SaveState> &state) {
	auto file = QFile(state->path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		if (const auto strong = state->controller.get()) {
			strong->showToast(u"Не удалось записать файл"_q);
		}
		return;
	}
	auto &list = state->messages;
	ranges::sort(list, ranges::less(), &SavedMessage::id);
	const auto title = state->history->peer->name();
	const auto now = QDateTime::currentDateTime().toString(u"dd.MM.yyyy HH:mm"_q);
	const auto when = [](TimeId date) {
		return QDateTime::fromSecsSinceEpoch(date).toString(
			u"dd.MM.yyyy HH:mm"_q);
	};
	auto out = QTextStream(&file);
	out.setEncoding(QStringConverter::Utf8);
	if (!state->html) {
		out << "Чат: " << title << "\n"
			<< "Сохранено в aglergram " << now
			<< ", сообщений: " << list.size() << "\n\n";
		for (const auto &message : list) {
			out << '[' << when(message.date) << "] " << message.from << ": ";
			if (!message.media.isEmpty()) {
				out << message.media << (message.text.isEmpty() ? "" : " ");
			}
			out << message.text << "\n";
		}
	} else {
		out << "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\">"
			"<title>" << Escape(title) << "</title><style>"
			"body{margin:0;background:#0e1621;color:#f2f5f8;"
			"font:15px/1.45 system-ui,Segoe UI,Roboto,sans-serif}"
			".wrap{max-width:760px;margin:0 auto;padding:24px 16px}"
			"h1{font-size:22px;margin:0 0 4px}.sub{color:#8a9aab;margin-bottom:20px}"
			".m{max-width:78%;margin:6px 0;padding:8px 12px 6px;border-radius:14px;"
			"background:#182533;clear:both;float:left}"
			".m.o{background:#2b5278;float:right}"
			".f{color:#6ab2f2;font-weight:600;font-size:13px}"
			".d{color:#8a9aab;font-size:12px;text-align:right}"
			".x{color:#c6d2de;font-style:italic}.c{clear:both}"
			"</style><div class=\"wrap\"><h1>" << Escape(title) << "</h1>"
			"<div class=\"sub\">Сохранено в aglergram " << now
			<< " · сообщений: " << list.size() << "</div>";
		for (const auto &message : list) {
			out << "<div class=\"m" << (message.out ? " o" : "") << "\">"
				<< "<div class=\"f\">" << Escape(message.from) << "</div>";
			if (!message.media.isEmpty()) {
				out << "<span class=\"x\">" << Escape(message.media) << "</span> ";
			}
			out << Escape(message.text)
				<< "<div class=\"d\">" << when(message.date) << "</div></div>";
		}
		out << "<div class=\"c\"></div></div></html>";
	}
	out.flush();
	file.close();
	if (const auto box = state->box.data()) {
		box->closeBox();
	}
	if (const auto strong = state->controller.get()) {
		strong->showToast(u"💾 Сохранено сообщений: "_q
			+ QString::number(list.size()));
	}
	File::ShowInFolder(state->path);
}

void RequestSaved(const std::shared_ptr<SaveState> &state) {
	if (state->cancelled) {
		return;
	}
	const auto history = state->history;
	const auto session = &history->session();
	session->api().request(MTPmessages_GetHistory(
		history->peer->input(),
		MTP_int(state->offsetId),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kHistoryLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (state->cancelled) {
			return;
		}
		const auto list = result.match([](
				const MTPDmessages_messagesNotModified &) {
			return (const QVector<MTPMessage>*)nullptr;
		}, [&](const auto &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			return &data.vmessages().v;
		});
		auto added = 0;
		auto lowest = state->offsetId;
		if (list) {
			for (const auto &message : *list) {
				message.match([&](const MTPDmessage &data) {
					auto saved = SavedMessage{
						.id = data.vid().v,
						.date = data.vdate().v,
						.out = data.is_out(),
						.text = qs(data.vmessage()),
						.media = data.vmedia()
							? MediaTitle(*data.vmedia())
							: QString(),
					};
					const auto fromId = data.vfrom_id()
						? peerFromMTP(*data.vfrom_id())
						: PeerId();
					const auto from = fromId
						? session->data().peerLoaded(fromId)
						: saved.out
						? (PeerData*)session->user().get()
						: history->peer.get();
					saved.from = from ? from->name() : u"?"_q;
					state->messages.push_back(std::move(saved));
					lowest = data.vid().v;
					++added;
				}, [&](const MTPDmessageService &data) {
					lowest = data.vid().v;
				}, [&](const MTPDmessageEmpty &data) {
					lowest = data.vid().v;
				});
			}
		}
		if (const auto label = state->label.data()) {
			label->setText(u"Загружено сообщений: "_q
				+ QString::number(state->messages.size()));
		}
		const auto done = !list
			|| list->isEmpty()
			|| lowest <= 1
			|| lowest == state->offsetId
			|| int(state->messages.size()) >= kMaxSavedMessages;
		if (done) {
			WriteSaved(state);
			return;
		}
		state->offsetId = lowest;
		base::call_delayed(kHistoryDelay, session, [=] {
			RequestSaved(state);
		});
	}).fail([=](const MTP::Error &error) {
		if (state->cancelled) {
			return;
		}
		const auto type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = type.mid(11).toInt();
			base::call_delayed(
				crl::time(std::clamp(seconds, 1, 120)) * 1000,
				session,
				[=] { RequestSaved(state); });
			return;
		}
		if (!state->messages.empty()) {
			WriteSaved(state);
		} else if (const auto strong = state->controller.get()) {
			strong->showToast(u"Не удалось загрузить чат: "_q + type);
		}
	}).send();
}

// Forward Pro.

[[nodiscard]] std::vector<not_null<Main::Session*>> OtherSessions(
		not_null<Main::Session*> current) {
	auto result = std::vector<not_null<Main::Session*>>();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account->sessionExists() && &account->session() != current) {
			result.push_back(&account->session());
		}
	}
	return result;
}

[[nodiscard]] std::vector<not_null<History*>> SessionChats(
		not_null<Main::Session*> session) {
	auto result = std::vector<not_null<History*>>();
	for (const auto &row : *session->data().chatsList()->indexed()) {
		if (const auto history = row->history()) {
			result.push_back(history);
		}
	}
	return result;
}

// A local copy of the media, empty when it is not downloaded yet.
[[nodiscard]] QString LocalMediaPath(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return QString();
	} else if (const auto document = media->document()) {
		const auto path = document->filepath(true);
		if (!path.isEmpty()) {
			return path;
		}
		const auto view = document->createMediaView();
		const auto bytes = view->bytes();
		if (bytes.isEmpty()) {
			document->save(item->fullId(), QString());
			return QString();
		}
		auto name = document->filename();
		if (name.isEmpty()) {
			name = u"file"_q;
		}
		const auto target = QDir::tempPath()
			+ u"/aglergram_fwd_"_q
			+ QString::number(base::RandomValue<uint32>(), 16)
			+ '_'
			+ name;
		auto file = QFile(target);
		if (!file.open(QIODevice::WriteOnly)) {
			return QString();
		}
		file.write(bytes);
		return target;
	} else if (const auto photo = media->photo()) {
		const auto view = photo->createMediaView();
		const auto image = view->image(Data::PhotoSize::Large);
		if (!image) {
			view->wanted(Data::PhotoSize::Large, item->fullId());
			return QString();
		}
		const auto target = QDir::tempPath()
			+ u"/aglergram_fwd_"_q
			+ QString::number(base::RandomValue<uint32>(), 16)
			+ u".jpg"_q;
		return image->original().save(target, "JPG", 92) ? target : QString();
	}
	return QString();
}

void SendCopy(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item,
		not_null<History*> to) {
	const auto session = &to->session();
	const auto original = item->originalText();
	auto text = TextWithTags{
		original.text,
		TextUtilities::ConvertEntitiesToTextTags(original.entities),
	};
	const auto hasMedia = item->media()
		&& (item->media()->document() || item->media()->photo());
	auto action = Api::SendAction(to);
	if (hasMedia) {
		const auto path = LocalMediaPath(item);
		if (path.isEmpty()) {
			controller->showToast(u"Медиа ещё не скачано — подожди пару "
				"секунд и попробуй снова"_q);
			return;
		}
		auto list = Storage::PrepareMediaList(
			QStringList{ path },
			st::sendMediaPreviewSize,
			session->user()->isPremium());
		if (list.error != Ui::PreparedList::Error::None
			|| list.files.empty()) {
			controller->showToast(u"Не получилось подготовить файл"_q);
			return;
		}
		list.files.front().caption = std::move(text);
		session->api().sendFiles(
			std::move(list),
			SendMediaType::File,
			nullptr,
			action);
	} else if (!text.text.isEmpty()) {
		auto message = Api::MessageToSend(action);
		message.textWithTags = std::move(text);
		session->api().sendMessage(std::move(message));
	} else {
		controller->showToast(u"Такое сообщение скопировать нельзя"_q);
		return;
	}
	controller->showToast(u"↪️ Отправлено от "_q
		+ session->user()->name()
		+ u" в «"_q
		+ to->peer->name()
		+ u"»"_q);
}

void ChooseChat(
		not_null<SessionController*> controller,
		not_null<Main::Session*> session,
		FullMsgId itemId) {
	const auto own = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Куда отправить от "_q
			+ session->user()->name()));
		box->setWidth(st::boxWideWidth);
		const auto search = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Поиск чата"_q)));
		const auto rows = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto refresh = [=] {
			while (rows->count()) {
				delete rows->widgetAt(0);
			}
			const auto query = search->getLastText().trimmed().toLower();
			auto shown = 0;
			for (const auto &history : SessionChats(session)) {
				const auto name = history->peer->name();
				if (!query.isEmpty() && !name.toLower().contains(query)) {
					continue;
				}
				const auto button = rows->add(
					object_ptr<Ui::SettingsButton>(
						rows,
						rpl::single(u"💬 "_q + name),
						st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					box->closeBox();
					if (const auto item = own->data().message(itemId)) {
						SendCopy(controller, item, history);
					}
				});
				if (++shown >= 40) {
					break;
				}
			}
			rows->resizeToWidth(box->width());
		};
		search->changes() | rpl::on_next(refresh, search->lifetime());
		refresh();
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->setFocusCallback([=] { search->setFocusFast(); });
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

struct PluginInfo {
	QString id;
	QString title;
	QString about;
	Fn<void(not_null<SessionController*>)> settings;
};

void ApplyName(not_null<PeerData*> peer) {
	peer->refreshLocalName();
}

} // namespace

void StartPlugins() {
	using Media::Capture::VoiceEffect;
	const auto stored = Data::LiquidStore::Value(u"voiceEffect"_q).toInt();
	const auto effect = (stored > 0 && stored <= int(VoiceEffect::Cave)
		&& PluginEnabled(u"voicefx"_q))
		? VoiceEffect(stored)
		: VoiceEffect::None;
	Media::Capture::SetCurrentVoiceEffect(effect);

	using Ui::LiquidRound::Frame;
	const auto ring = Data::LiquidStore::Value(u"roundFrame"_q).toInt();
	Ui::LiquidRound::SetCurrent((ring > 0 && ring <= int(Frame::Love)
		&& PluginEnabled(u"roundframe"_q))
		? Frame(ring)
		: Frame::None);
}

void ShowRoundFrameBox(not_null<SessionController*> controller) {
	using Ui::LiquidRound::Frame;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"⭕ Рамка на кружках"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Светящаяся анимированная рамка записывается прямо в кружок: "
			"её видят все, в любом Telegram. Видно и в превью, пока "
			"записываешь."_q,
			st::boxDividerLabel));
		const auto group = std::make_shared<Ui::RadioenumGroup<Frame>>(
			Ui::LiquidRound::Current());
		const auto add = [&](const QString &label, Frame frame) {
			box->addRow(object_ptr<Ui::Radioenum<Frame>>(
				box,
				group,
				frame,
				label,
				st::defaultBoxCheckbox));
		};
		add(u"Без рамки"_q, Frame::None);
		add(u"🌈 Неон"_q, Frame::Neon);
		add(u"🔥 Огонь"_q, Frame::Fire);
		add(u"❄️ Лёд"_q, Frame::Ice);
		add(u"✨ Золото"_q, Frame::Gold);
		add(u"💗 Love"_q, Frame::Love);
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto frame = group->current();
			Data::LiquidStore::SetValue(
				u"roundFrame"_q,
				(frame == Frame::None)
					? QJsonValue()
					: QJsonValue(int(frame)));
			if (frame != Frame::None) {
				SetPluginEnabled(u"roundframe"_q, true);
			}
			StartPlugins();
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowVoiceFxBox(not_null<SessionController*> controller) {
	using Media::Capture::VoiceEffect;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🎙 Эффект голоса"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Голосовые записываются уже с эффектом: собеседник слышит "
			"изменённый голос в любом Telegram. Перед отправкой можно "
			"прослушать запись."_q,
			st::boxDividerLabel));
		const auto group = std::make_shared<Ui::RadioenumGroup<VoiceEffect>>(
			Media::Capture::CurrentVoiceEffect());
		const auto add = [&](const QString &label, VoiceEffect effect) {
			box->addRow(object_ptr<Ui::Radioenum<VoiceEffect>>(
				box,
				group,
				effect,
				label,
				st::defaultBoxCheckbox));
		};
		add(u"Без эффекта"_q, VoiceEffect::None);
		add(u"🤖 Робот"_q, VoiceEffect::Robot);
		add(u"🐿 Бурундук"_q, VoiceEffect::Chipmunk);
		add(u"🐻 Низкий голос"_q, VoiceEffect::Deep);
		add(u"🏔 Эхо"_q, VoiceEffect::Echo);
		add(u"🦇 Пещера"_q, VoiceEffect::Cave);
		add(u"📻 Рация"_q, VoiceEffect::Radio);
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto effect = group->current();
			Data::LiquidStore::SetValue(
				u"voiceEffect"_q,
				(effect == VoiceEffect::None)
					? QJsonValue()
					: QJsonValue(int(effect)));
			if (effect != VoiceEffect::None) {
				SetPluginEnabled(u"voicefx"_q, true);
			}
			StartPlugins();
			controller->showToast((effect == VoiceEffect::None)
				? u"🎙 Голос без эффекта"_q
				: u"🎙 Следующие голосовые — с эффектом"_q);
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowLocalRenameBox(
		not_null<SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto session = &peer->session();
	const auto peerId = peer->id;
	auto real = Data::LiquidStore::RealName(session, peerId);
	if (real.isEmpty()) {
		real = peer->name();
	}
	const auto local = Data::LiquidStore::LocalName(session, peerId);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"✏️ Переименовать для себя"_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Своё название"_q),
			local.isEmpty() ? real : local));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Настоящее название: «"_q + real + u"».\nНовое видно только "
				"тебе в aglergram на этом компьютере, остальные видят "
				"настоящее."_q,
			st::boxDividerLabel));
		box->setFocusCallback([=] {
			field->setFocusFast();
			field->selectAll();
		});
		const auto save = [=] {
			const auto text = field->getLastText().trimmed();
			Data::LiquidStore::SetLocalName(
				session,
				peerId,
				(text == real) ? QString() : text);
			if (const auto peer = session->data().peerLoaded(peerId)) {
				ApplyName(peer);
			}
			box->closeBox();
		};
		field->submits() | rpl::on_next(save, field->lifetime());
		box->addButton(rpl::single(u"Сохранить"_q), save);
		if (!local.isEmpty()) {
			box->addLeftButton(rpl::single(u"Вернуть настоящее"_q), [=] {
				Data::LiquidStore::SetLocalName(session, peerId, QString());
				if (const auto peer = session->data().peerLoaded(peerId)) {
					ApplyName(peer);
				}
				box->closeBox();
			});
		}
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

bool PluginEnabled(const QString &id) {
	return Data::LiquidStore::Value(QString::fromLatin1(kPluginsKey))
		.toObject()
		.value(id)
		.toBool(true);
}

void SetPluginEnabled(const QString &id, bool enabled) {
	auto object = Data::LiquidStore::Value(
		QString::fromLatin1(kPluginsKey)).toObject();
	if (enabled) {
		object.remove(id);
	} else {
		object.insert(id, false);
	}
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kPluginsKey),
		object.isEmpty() ? QJsonValue() : QJsonValue(object));
}

rpl::producer<bool> PluginEnabledValue(const QString &id) {
	return rpl::single(
		PluginEnabled(id)
	) | rpl::then(Data::LiquidStore::ValueChanges(
	) | rpl::filter([](const QString &key) {
		return key == QString::fromLatin1(kPluginsKey);
	}) | rpl::map([=] {
		return PluginEnabled(id);
	})) | rpl::distinct_until_changed();
}

void ShowPluginsBox(not_null<SessionController*> controller) {
	const auto plugins = std::vector<PluginInfo>{
		{
			u"textmodes"_q,
			u"✨ Режимы текста и команды"_q,
			u"Правый клик по полю ввода: пикми, UwU, КАПС… и .roll, "
				".coin, .heart"_q,
			[](auto c) { ShowTextCommandsBox(c); },
		},
		{
			u"editfx"_q,
			u"🪄 Красивые правки"_q,
			u"Когда сообщение меняют, по нему проходит световая волна: "
				"старый текст тает, новый проявляется"_q,
			nullptr,
		},
		{
			u"voicefx"_q,
			u"🎙 Эффекты голоса"_q,
			u"Робот, бурундук, низкий голос, эхо, пещера, рация — голосовое "
				"уходит уже изменённым, слышат все"_q,
			[](auto c) { ShowVoiceFxBox(c); },
		},
		{
			u"roundframe"_q,
			u"⭕ Рамка на кружках"_q,
			u"Неон, огонь, лёд, золото — светящаяся рамка вшита в кружок, "
				"видят все"_q,
			[](auto c) { ShowRoundFrameBox(c); },
		},
		{
			u"music"_q,
			u"🎵 Слушаю сейчас"_q,
			u"Как в Discord: трек из Spotify строкой «Слушает» в профиле. "
				"Видят только друзья с aglergram"_q,
			[](auto c) { ShowMusicBox(c); },
		},
		{
			u"rename"_q,
			u"✏️ Свои названия"_q,
			u"Меню чата → «Переименовать для себя»: своё имя каналу, группе "
				"или человеку, видно только тебе"_q,
			nullptr,
		},
		{
			u"streaks"_q,
			u"🔥 Огоньки"_q,
			u"Как в TikTok: если вы с человеком пишете друг другу каждый "
				"день, у чата горит 🔥 и число дней. ⌛ — сегодня ещё не "
				"переписывались, огонёк сгорит в полночь"_q,
			nullptr,
		},
		{
			u"chatsaver"_q,
			u"💾 ChatSaver"_q,
			u"Сохранение чата в файл TXT или HTML: меню чата → "
				"«Сохранить чат в файл»"_q,
			nullptr,
		},
		{
			u"forwardpro"_q,
			u"↪️ Forward Pro"_q,
			u"Отправить копию сообщения с другого своего аккаунта: "
				"правый клик по сообщению"_q,
			nullptr,
		},
		{
			u"quickreplies"_q,
			u"⚡ Быстрые ответы"_q,
			u"Свои фразы кнопками над полем ввода"_q,
			[](auto c) { ShowQuickRepliesBox(c); },
		},
		{
			u"whisper"_q,
			u"🎙 Whisper"_q,
			u"Голосовые и кружки в текст прямо на компьютере"_q,
			nullptr,
		},
		{
			u"convert"_q,
			u"⭕ Кружки и GIF"_q,
			u"Отправить видео кружком или GIF"_q,
			nullptr,
		},
		{
			u"bookmarks"_q,
			u"⭐ Закладки"_q,
			u"Звёздочка на сообщении и общий список"_q,
			[](auto c) { ShowBookmarksBox(c); },
		},
		{
			u"hidden"_q,
			u"🙈 Скрытые чаты"_q,
			u"Чаты под PIN, без уведомлений"_q,
			[](auto c) { ShowHiddenChatsBox(c); },
		},
		{
			u"twitch"_q,
			u"🟣 Twitch LIVE"_q,
			u"Значок LIVE у друзей, пока они стримят"_q,
			[](auto c) { ShowTwitchBox(c); },
		},
	};
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🧩 Плагины"_q));
		box->setWidth(st::boxWideWidth);
		for (const auto &plugin : plugins) {
			const auto id = plugin.id;
			const auto toggle = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(plugin.title),
					st::settingsButtonNoIcon),
				style::margins());
			toggle->toggleOn(rpl::single(PluginEnabled(id)));
			toggle->toggledChanges() | rpl::on_next([=](bool enabled) {
				SetPluginEnabled(id, enabled);
				StartPlugins();
			}, toggle->lifetime());
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				plugin.about,
				st::boxDividerLabel));
			if (const auto settings = plugin.settings) {
				const auto open = box->addRow(
					object_ptr<Ui::SettingsButton>(
						box,
						rpl::single(u"      настроить"_q),
						st::settingsButtonNoIcon),
					style::margins());
				open->setClickedCallback([=] {
					box->closeBox();
					settings(controller);
				});
			}
		}
		const auto extra = std::vector<std::pair<QString, Fn<void()>>>{
			{ u"🖱 След за курсором"_q, [=] { ShowCursorTrailBox(controller); } },
			{ u"🌌 Живые обои"_q, [=] { ShowLiveWallpaperBox(controller); } },
			{ u"🌙 Сводка дня"_q, [=] { ShowDaySummary(controller); } },
		};
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Оформление"_q,
			st::boxDividerLabel));
		for (const auto &[title, callback] : extra) {
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(title),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				box->closeBox();
				callback();
			});
		}
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void SaveChatToFile(
		not_null<SessionController*> controller,
		not_null<History*> history) {
	const auto start = [=](bool html) {
		auto name = history->peer->name();
		name.replace(QRegularExpression(u"[\\\\/:*?\"<>|]"_q), u"_"_q);
		const auto filter = html
			? u"HTML (*.html)"_q
			: u"Текст (*.txt)"_q;
		FileDialog::GetWritePath(
			Core::App().getFileDialogParent(),
			u"Сохранить чат"_q,
			filter,
			name + (html ? u".html"_q : u".txt"_q),
			[=](QString &&path) {
				if (path.isEmpty()) {
					return;
				}
				const auto state = std::make_shared<SaveState>(SaveState{
					.history = history,
					.path = path,
					.html = html,
					.controller = base::make_weak(controller),
				});
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					box->setTitle(rpl::single(u"💾 Сохранение чата"_q));
					state->box = box.get();
					state->label = box->addRow(object_ptr<Ui::FlatLabel>(
						box,
						u"Загружено сообщений: 0"_q,
						st::boxLabel));
					box->addRow(object_ptr<Ui::FlatLabel>(
						box,
						u"Большие чаты грузятся несколько минут — Telegram "
						"отдаёт историю порциями."_q,
						st::boxDividerLabel));
					box->addButton(rpl::single(u"Сохранить, что есть"_q), [=] {
						state->cancelled = true;
						WriteSaved(state);
					});
					box->addButton(tr::lng_cancel(), [=] {
						state->cancelled = true;
						box->closeBox();
					});
				}));
				RequestSaved(state);
			});
	};
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"💾 Сохранить чат в файл"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Вся история «"_q + history->peer->name()
				+ u"» в один файл. HTML выглядит как переписка, TXT удобно "
				"искать."_q,
			st::boxLabel));
		box->addButton(rpl::single(u"HTML"_q), [=] {
			box->closeBox();
			start(true);
		});
		box->addButton(rpl::single(u"TXT"_q), [=] {
			box->closeBox();
			start(false);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

bool CanForwardFromOtherAccount(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item) {
	return PluginEnabled(u"forwardpro"_q)
		&& !item->isService()
		&& !OtherSessions(&controller->session()).empty();
}

void ForwardFromOtherAccount(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto sessions = OtherSessions(&controller->session());
	const auto itemId = item->fullId();
	if (sessions.size() == 1) {
		ChooseChat(controller, sessions.front(), itemId);
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"↪️ С какого аккаунта"_q));
		for (const auto &session : sessions) {
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(u"👤 "_q + session->user()->name()),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				box->closeBox();
				ChooseChat(controller, session, itemId);
			});
		}
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Window::Liquid
