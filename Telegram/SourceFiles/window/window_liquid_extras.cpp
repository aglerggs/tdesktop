/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_extras.h"

#include "window/window_liquid_features.h"
#include "window/window_liquid_updates.h"
#include "window/window_liquid_convert.h"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_chat_filters.h"
#include "data/data_document.h"
#include "data/data_drafts.h"
#include "data/data_folder.h"
#include "data/data_liquid_store.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_domain.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "main/main_session.h"
#include "settings/sections/settings_chat.h"
#include "storage/storage_domain.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QShortcut>

namespace Window::Liquid {
namespace {

constexpr auto kBookmarksKey = "bookmarks";
constexpr auto kFocusKey = "focus";
constexpr auto kLockScreensKey = "lockOnScreens";
constexpr auto kLockedFoldersKey = "lockedFolders";
constexpr auto kSkipSilenceKey = "skipSilence";
constexpr auto kMaxRows = 200;

[[nodiscard]] QString Shorten(QString text, int size) {
	text = text.simplified();
	return (text.size() > size)
		? (text.mid(0, size - 1) + QChar(0x2026))
		: text;
}

[[nodiscard]] QString ItemPreview(not_null<HistoryItem*> item) {
	auto text = item->originalText().text;
	if (text.trimmed().isEmpty()) {
		text = item->media() ? u"[медиа]"_q : u"[сообщение]"_q;
	}
	return Shorten(text, 120);
}

[[nodiscard]] QString FormatDate(TimeId date) {
	return QDateTime::fromSecsSinceEpoch(date).toString(u"dd.MM.yy HH:mm"_q);
}

// Bookmarks.

[[nodiscard]] QJsonArray Bookmarks() {
	return Data::LiquidStore::Value(QString::fromLatin1(kBookmarksKey))
		.toArray();
}

void SaveBookmarks(const QJsonArray &list) {
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kBookmarksKey),
		list.isEmpty() ? QJsonValue() : QJsonValue(list));
}

[[nodiscard]] int BookmarkIndex(not_null<HistoryItem*> item) {
	const auto list = Bookmarks();
	const auto key = Data::LiquidStore::PeerKey(
		&item->history()->session(),
		item->history()->peer->id);
	const auto msg = QString::number(item->id.bare);
	for (auto i = 0; i != list.size(); ++i) {
		const auto entry = list[i].toObject();
		if (entry.value(u"k"_q).toString() == key
			&& entry.value(u"msg"_q).toString() == msg) {
			return i;
		}
	}
	return -1;
}

// Focus profiles.

struct FocusProfile {
	QString name;
	QStringList peers;
	FilterId folder = 0;
};

struct FocusState {
	std::vector<FocusProfile> profiles;
	QString active;
};

[[nodiscard]] FocusState LoadFocus() {
	auto result = FocusState();
	const auto object = Data::LiquidStore::Value(
		QString::fromLatin1(kFocusKey)).toObject();
	result.active = object.value(u"active"_q).toString();
	for (const auto value : object.value(u"profiles"_q).toArray()) {
		const auto entry = value.toObject();
		auto profile = FocusProfile{
			.name = entry.value(u"name"_q).toString(),
			.folder = FilterId(entry.value(u"folder"_q).toInt()),
		};
		for (const auto peer : entry.value(u"peers"_q).toArray()) {
			profile.peers.push_back(peer.toString());
		}
		if (!profile.name.isEmpty()) {
			result.profiles.push_back(std::move(profile));
		}
	}
	return result;
}

void SaveFocus(const FocusState &state) {
	auto profiles = QJsonArray();
	for (const auto &profile : state.profiles) {
		profiles.push_back(QJsonObject{
			{ u"name"_q, profile.name },
			{ u"peers"_q, QJsonArray::fromStringList(profile.peers) },
			{ u"folder"_q, int(profile.folder) },
		});
	}
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kFocusKey),
		QJsonObject{
			{ u"active"_q, state.active },
			{ u"profiles"_q, profiles },
		});
}

[[nodiscard]] std::vector<not_null<History*>> MainChats(
		not_null<Main::Session*> session,
		int limit) {
	auto result = std::vector<not_null<History*>>();
	const auto list = session->data().chatsList()->indexed();
	for (const auto &row : *list) {
		if (const auto history = row->history()) {
			result.push_back(history);
			if (int(result.size()) >= limit) {
				break;
			}
		}
	}
	return result;
}

// Puts the profile chats in a "🎯 name" folder and opens it.
void ShowFocusFolder(
		not_null<SessionController*> controller,
		FocusProfile &profile) {
	const auto session = &controller->session();
	auto always = base::flat_set<not_null<History*>>();
	for (const auto &history : MainChats(session, 100000)) {
		const auto key = Data::LiquidStore::PeerKey(
			session,
			history->peer->id);
		if (profile.peers.contains(key)) {
			always.emplace(history);
		}
	}
	if (always.empty()) {
		return;
	}
	auto &filters = session->data().chatsFilters();
	auto id = profile.folder;
	auto maxId = FilterId(1);
	auto exists = false;
	for (const auto &filter : filters.list()) {
		maxId = std::max(maxId, filter.id());
		if (filter.id() == id) {
			exists = true;
		}
	}
	if (!exists) {
		id = maxId + 1;
		profile.folder = id;
	}
	const auto filter = Data::ChatFilter(
		id,
		Data::ChatFilterTitle{ .text = { u"🎯 "_q + profile.name } },
		u"🎯"_q,
		std::nullopt,
		Data::ChatFilter::Flags(),
		std::move(always),
		{},
		{});
	filters.set(filter);
	session->api().request(MTPmessages_UpdateDialogFilter(
		MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
		MTP_int(filter.id()),
		filter.tl()
	)).send();
	const auto weak = base::make_weak(controller);
	crl::on_main(controller, [=] {
		if (const auto strong = weak.get()) {
			strong->setActiveChatsFilter(id);
		}
	});
}

void ShowFocusEditBox(
		not_null<SessionController*> controller,
		int index) {
	const auto session = &controller->session();
	const auto state = LoadFocus();
	const auto existing = (index >= 0 && index < int(state.profiles.size()))
		? state.profiles[index]
		: FocusProfile();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(existing.name.isEmpty()
			? u"Новый профиль фокуса"_q
			: (u"Профиль: "_q + existing.name)));
		box->setWidth(st::boxWideWidth);
		const auto name = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Название, например «Учёба»"_q),
			existing.name));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Отметь чаты, которые остаются. Остальные молчат, "
			"пока профиль включён."_q,
			st::boxDividerLabel));
		auto checks = std::make_shared<
			std::vector<std::pair<QString, Ui::Checkbox*>>>();
		for (const auto &history : MainChats(session, 150)) {
			const auto key = Data::LiquidStore::PeerKey(
				session,
				history->peer->id);
			const auto check = box->addRow(object_ptr<Ui::Checkbox>(
				box,
				history->peer->name(),
				existing.peers.contains(key),
				st::defaultBoxCheckbox));
			checks->push_back({ key, check });
		}
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->setFocusCallback([=] { name->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			const auto title = name->getLastText().trimmed();
			if (title.isEmpty()) {
				name->showError();
				return;
			}
			auto now = LoadFocus();
			auto profile = existing;
			if (!profile.name.isEmpty() && profile.name != title
				&& now.active == profile.name) {
				now.active = title;
			}
			profile.name = title;
			profile.peers.clear();
			for (const auto &[key, check] : *checks) {
				if (check->checked()) {
					profile.peers.push_back(key);
				}
			}
			if (index >= 0 && index < int(now.profiles.size())) {
				now.profiles[index] = profile;
			} else {
				now.profiles.push_back(profile);
			}
			SaveFocus(now);
			box->closeBox();
			ShowFocusBox(controller);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

// Command palette.

struct Command {
	QString title;
	QString keywords;
	Fn<void()> callback;
};

[[nodiscard]] std::vector<Command> Commands(
		not_null<SessionController*> controller) {
	const auto session = &controller->session();
	auto result = std::vector<Command>();
	const auto add = [&](QString title, QString keywords, Fn<void()> done) {
		result.push_back({ std::move(title), keywords, std::move(done) });
	};
	add(u"⚙️ Настройки"_q, u"settings настройки"_q, [=] {
		controller->showSettings();
	});
	add(u"💬 Настройки чатов"_q, u"chat settings чаты тема"_q, [=] {
		controller->showSettings(::Settings::ChatId());
	});
	add(u"⭐ Избранное"_q, u"saved избранное сохранённые"_q, [=] {
		controller->showPeerHistory(session->userPeerId());
	});
	add(u"🗄 Архив"_q, u"archive архив"_q, [=] {
		controller->openFolder(
			session->data().folder(Data::Folder::kId));
	});
	add(u"🔖 Закладки"_q, u"bookmarks закладки звёздочки"_q, [=] {
		ShowBookmarksBox(controller);
	});
	add(u"📝 Все черновики"_q, u"drafts черновики"_q, [=] {
		ShowAllDraftsBox(controller);
	});
	add(u"🎯 Профили фокуса"_q, u"focus фокус учёба профили"_q, [=] {
		ShowFocusBox(controller);
	});
	add(u"⭕ Видео в кружок"_q, u"round кружок видео"_q, [=] {
		ConvertFromFile(controller, ConvertKind::Round);
	});
	add(u"🎞 Видео в GIF"_q, u"gif гиф видео"_q, [=] {
		ConvertFromFile(controller, ConvertKind::Gif);
	});
	add(u"⚡ Быстрые ответы"_q, u"quick быстрые ответы фразы кнопки"_q, [=] {
		ShowQuickRepliesBox(controller);
	});
	add(u"🌙 Сводка дня"_q, u"summary сводка день итоги"_q, [=] {
		ShowDaySummary(controller);
	});
	add(u"🙈 Скрытые чаты"_q, u"hidden скрытые чаты тайные"_q, [=] {
		ShowHiddenChatsBox(controller);
	});
	add(u"🔐 PIN на папки"_q, u"pin пин папки замок"_q, [=] {
		ShowLockedFoldersBox(controller);
	});
	add(u"✅ Прочитать все чаты"_q, u"read all прочитать"_q, [=] {
		ReadAllChats(controller);
	});
	add(u"📬 Папка «Непрочитанное»"_q, u"unread непрочитанное папка"_q, [=] {
		CreateUnreadFolder(controller);
	});
	add(u"🧩 Шаблоны текста"_q, u"templates шаблоны"_q, [=] {
		ShowTemplatesBox(controller);
	});
	add(u"🌇 Тема по закату"_q, u"sunset закат ночь тема"_q, [=] {
		ShowSunsetBox(controller);
	});
	add(u"🎞 Живые обои"_q, u"wallpaper обои фон живые"_q, [=] {
		ShowLiveWallpaperBox(controller);
	});
	add(u"🟣 Twitch"_q, u"twitch твич стрим эфир"_q, [=] {
		ShowTwitchBox(controller);
	});
	add(u"🆕 Что нового"_q, u"patch notes что нового патч"_q, [=] {
		ShowPatchHistory(controller);
	});
	add(u"⬇️ Проверить обновления"_q, u"update обновления"_q, [=] {
		CheckForUpdates(controller, true);
	});
	if (session->domain().local().hasLocalPasscode()) {
		add(u"🔒 Заблокировать"_q, u"lock блок пароль"_q, [] {
			Core::App().maybeLockByPasscode();
		});
	}
	return result;
}

[[nodiscard]] bool Matches(const QString &text, const QStringList &words) {
	const auto lower = text.toLower();
	for (const auto &word : words) {
		if (!lower.contains(word)) {
			return false;
		}
	}
	return true;
}

// Voice messages: jumps over long silent parts using the waveform.
void StartSkipSilence() {
	using Type = AudioMsgId::Type;
	struct Last {
		FullMsgId item;
		float64 target = 0.;
	};
	static auto last = Last();
	static auto lifetime = rpl::lifetime();
	Media::Player::instance()->updatedNotifier(
	) | rpl::filter([](const Media::Player::TrackState &state) {
		return (state.id.type() == Type::Voice)
			&& !Media::Player::IsPausedOrPausing(state.state)
			&& !Media::Player::IsStoppedOrStopping(state.state)
			&& (state.length > 0)
			&& SkipSilence();
	}) | rpl::on_next([](const Media::Player::TrackState &state) {
		const auto document = state.id.audio();
		const auto voice = document ? document->voice() : nullptr;
		if (!voice || voice->waveform.size() < 16 || voice->waveform[0] < 0) {
			return;
		}
		const auto &wave = voice->waveform;
		const auto count = int(wave.size());
		const auto seconds = std::max(
			float64(document->duration()) / 1000.,
			float64(state.length) / std::max(state.frequency, 1));
		if (seconds < 3.) {
			return;
		}
		auto loudest = 0;
		for (const auto value : wave) {
			loudest = std::max(loudest, int(value));
		}
		const auto quiet = std::max(2, loudest / 6);
		const auto progress = float64(state.position) / state.length;
		if (last.item == state.id.contextId() && progress < last.target) {
			return;
		}
		const auto index = std::clamp(int(progress * count), 0, count - 1);
		if (wave[index] > quiet) {
			return;
		}
		auto till = index;
		while (till < count && wave[till] <= quiet) {
			++till;
		}
		const auto silent = float64(till - index) * seconds / count;
		constexpr auto kMinSilence = 0.8;
		constexpr auto kKeep = 0.2;
		if (silent < kMinSilence || till >= count) {
			return;
		}
		const auto target = std::clamp(
			(float64(till) / count) - (kKeep / seconds),
			progress,
			1.);
		if (target - progress < 0.3 / seconds) {
			return;
		}
		last = { state.id.contextId(), target };
		Media::Player::instance()->finishSeeking(Type::Voice, target);
	}, lifetime);
}

} // namespace

void StartExtras(not_null<SessionController*> controller) {
	const auto widget = controller->widget();
	const auto shortcut = new QShortcut(
		QKeySequence(u"Ctrl+Shift+K"_q),
		widget,
		nullptr,
		nullptr,
		Qt::WindowShortcut);
	const auto weak = base::make_weak(controller);
	QObject::connect(shortcut, &QShortcut::activated, [=] {
		if (const auto strong = weak.get()) {
			ShowCommandPalette(strong);
		}
	});
	StartDaySummary(controller);

	static auto started = false;
	if (started) {
		return;
	}
	started = true;
	const auto lock = [] {
		if (LockOnScreensChange()
			&& !Core::App().passcodeLocked()
			&& Core::App().domain().local().hasLocalPasscode()) {
			Core::App().maybeLockByPasscode();
		}
	};
	QObject::connect(qApp, &QGuiApplication::screenAdded, lock);
	QObject::connect(qApp, &QGuiApplication::screenRemoved, lock);
	StartSkipSilence();
}

bool SkipSilence() {
	return Data::LiquidStore::Value(
		QString::fromLatin1(kSkipSilenceKey)).toBool(false);
}

void SetSkipSilence(bool enabled) {
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kSkipSilenceKey),
		enabled ? QJsonValue(true) : QJsonValue());
}

bool IsBookmarked(not_null<HistoryItem*> item) {
	return BookmarkIndex(item) >= 0;
}

void ToggleBookmark(
		not_null<SessionController*> controller,
		not_null<HistoryItem*> item) {
	auto list = Bookmarks();
	const auto index = BookmarkIndex(item);
	if (index >= 0) {
		list.removeAt(index);
		SaveBookmarks(list);
		controller->showToast(u"Закладка убрана"_q);
		return;
	}
	list.push_back(QJsonObject{
		{ u"k"_q, Data::LiquidStore::PeerKey(
			&item->history()->session(),
			item->history()->peer->id) },
		{ u"peer"_q, QString::number(item->history()->peer->id.value) },
		{ u"msg"_q, QString::number(item->id.bare) },
		{ u"date"_q, double(item->date()) },
		{ u"title"_q, item->history()->peer->name() },
		{ u"text"_q, ItemPreview(item) },
	});
	SaveBookmarks(list);
	controller->showToast(
		u"⭐ В закладках. Все закладки: Ctrl+Shift+K → «Закладки»"_q);
}

void ShowBookmarksBox(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	const auto prefix = QString::number(session->userId().bare) + ':';
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"🔖 Закладки"_q));
		box->setWidth(st::boxWideWidth);
		const auto search = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Поиск по закладкам"_q)));
		const auto rows = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto refresh = [=] {
			while (rows->count()) {
				delete rows->widgetAt(0);
			}
			const auto words = search->getLastText().toLower().split(
				' ',
				Qt::SkipEmptyParts);
			auto list = Bookmarks();
			auto shown = 0;
			for (auto i = int(list.size()); i != 0;) {
				const auto entry = list[--i].toObject();
				if (!entry.value(u"k"_q).toString().startsWith(prefix)) {
					continue;
				}
				const auto title = entry.value(u"title"_q).toString();
				const auto text = entry.value(u"text"_q).toString();
				if (!Matches(title + ' ' + text, words)) {
					continue;
				}
				const auto peer = PeerId(
					entry.value(u"peer"_q).toString().toULongLong());
				const auto msg = MsgId(
					entry.value(u"msg"_q).toString().toLongLong());
				const auto label = title
					+ u" · "_q
					+ FormatDate(TimeId(entry.value(u"date"_q).toDouble()))
					+ u"\n"_q
					+ Shorten(text, 90);
				const auto button = rows->add(
					object_ptr<Ui::SettingsButton>(
						rows,
						rpl::single(label),
						st::settingsButtonNoIcon));
				button->setClickedCallback([=] {
					box->closeBox();
					controller->showPeerHistory(
						peer,
						SectionShow::Way::Forward,
						msg);
				});
				if (++shown >= kMaxRows) {
					break;
				}
			}
			if (!shown) {
				rows->add(
					object_ptr<Ui::FlatLabel>(
						rows,
						(list.isEmpty() || words.isEmpty())
							? u"Закладок пока нет. Правый клик по сообщению → "
								"«В закладки»."_q
							: u"Ничего не найдено"_q,
						st::boxDividerLabel),
					st::boxRowPadding);
			}
			rows->resizeToWidth(box->width());
		};
		search->changes() | rpl::on_next(refresh, search->lifetime());
		refresh();
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->setFocusCallback([=] { search->setFocusFast(); });
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowAllDraftsBox(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	struct Found {
		not_null<History*> history;
		MsgId topicRootId = 0;
		QString text;
		TimeId date = 0;
	};
	auto found = std::vector<Found>();
	for (const auto &history : MainChats(session, 100000)) {
		for (const auto &[key, draft] : history->draftsMap()) {
			if (!draft || draft->textWithTags.text.trimmed().isEmpty()) {
				continue;
			}
			const auto topic = key.topicRootId();
			const auto same = ranges::find_if(found, [&](const Found &f) {
				return (f.history == history) && (f.topicRootId == topic);
			});
			if (same != end(found)) {
				continue;
			}
			found.push_back({
				history,
				topic,
				draft->textWithTags.text,
				draft->date,
			});
		}
	}
	ranges::sort(found, ranges::greater(), &Found::date);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"📝 Все черновики"_q));
		box->setWidth(st::boxWideWidth);
		if (found.empty()) {
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Недописанных сообщений нет."_q,
				st::boxLabel));
		}
		auto shown = 0;
		for (const auto &entry : found) {
			const auto history = entry.history;
			const auto topicRootId = entry.topicRootId;
			const auto label = history->peer->name()
				+ (entry.date ? (u" · "_q + FormatDate(entry.date)) : QString())
				+ u"\n"_q
				+ Shorten(entry.text, 90);
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(label),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				box->closeBox();
				if (topicRootId) {
					controller->showPeerHistory(
						history,
						SectionShow::Way::Forward,
						topicRootId);
				} else {
					controller->showPeerHistory(
						history,
						SectionShow::Way::Forward);
				}
			});
			if (++shown >= kMaxRows) {
				break;
			}
		}
		box->setMaxHeight(st::boxMaxListHeight * 2);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowCommandPalette(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	const auto commands = std::make_shared<std::vector<Command>>(
		Commands(controller));
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Перейти к…"_q));
		box->setWidth(st::boxWideWidth);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Чат, настройка или функция"_q)));
		const auto rows = box->addRow(
			object_ptr<Ui::VerticalLayout>(box),
			style::margins());
		const auto first = std::make_shared<Fn<void()>>();
		const auto refresh = [=] {
			while (rows->count()) {
				delete rows->widgetAt(0);
			}
			*first = nullptr;
			const auto words = field->getLastText().toLower().split(
				' ',
				Qt::SkipEmptyParts);
			const auto addRow = [&](const QString &text, Fn<void()> done) {
				const auto run = [=] {
					box->closeBox();
					done();
				};
				if (!*first) {
					*first = run;
				}
				const auto button = rows->add(
					object_ptr<Ui::SettingsButton>(
						rows,
						rpl::single(text),
						st::settingsButtonNoIcon));
				button->setClickedCallback(run);
			};
			auto shown = 0;
			for (const auto &command : *commands) {
				if (Matches(command.title + ' ' + command.keywords, words)) {
					addRow(command.title, command.callback);
					if (++shown >= 8) {
						break;
					}
				}
			}
			if (!words.isEmpty()) {
				auto chats = 0;
				for (const auto &history : MainChats(session, 100000)) {
					if (!Matches(history->peer->name(), words)) {
						continue;
					}
					addRow(u"💬 "_q + history->peer->name(), [=] {
						controller->showPeerHistory(history);
					});
					if (++chats >= 8) {
						break;
					}
				}
			}
			rows->resizeToWidth(box->width());
		};
		field->changes() | rpl::on_next(refresh, field->lifetime());
		field->submits() | rpl::on_next([=] {
			if (const auto run = *first) {
				run();
			}
		}, field->lifetime());
		refresh();
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowFocusBox(not_null<SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto state = LoadFocus();
		box->setTitle(rpl::single(u"🎯 Профили фокуса"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			state.active.isEmpty()
				? u"Сейчас профиль выключен, уведомления как обычно."_q
				: (u"Сейчас включён «"_q + state.active
					+ u"»: уведомляют только его чаты."_q),
			st::boxDividerLabel));
		for (auto i = 0; i != int(state.profiles.size()); ++i) {
			const auto profile = state.profiles[i];
			const auto active = (profile.name == state.active);
			const auto label = (active ? u"● "_q : u"○ "_q)
				+ profile.name
				+ u" · чатов: "_q
				+ QString::number(profile.peers.size());
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(label),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				auto now = LoadFocus();
				if (active) {
					now.active = QString();
					SaveFocus(now);
					controller->showToast(u"Фокус выключен"_q);
				} else {
					now.active = profile.name;
					if (i < int(now.profiles.size())) {
						ShowFocusFolder(controller, now.profiles[i]);
					}
					SaveFocus(now);
					controller->showToast(
						u"🎯 «"_q + profile.name + u"»: остальные чаты молчат"_q);
				}
				box->closeBox();
			});
			const auto edit = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(u"      изменить «"_q + profile.name + u"»"_q),
					st::settingsButtonNoIcon),
				style::margins());
			edit->setClickedCallback([=] {
				box->closeBox();
				ShowFocusEditBox(controller, i);
			});
			const auto remove = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(u"      удалить «"_q + profile.name + u"»"_q),
					st::settingsButtonNoIcon),
				style::margins());
			remove->setClickedCallback([=] {
				auto now = LoadFocus();
				if (now.active == profile.name) {
					now.active = QString();
				}
				if (i < int(now.profiles.size())) {
					now.profiles.erase(now.profiles.begin() + i);
				}
				SaveFocus(now);
				box->closeBox();
				ShowFocusBox(controller);
			});
		}
		box->addButton(rpl::single(u"Новый профиль"_q), [=] {
			box->closeBox();
			ShowFocusEditBox(controller, -1);
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

bool FocusBlocksNotification(not_null<History*> history) {
	static auto cache = std::optional<FocusState>();
	static auto subscribed = false;
	static auto lifetime = rpl::lifetime();
	if (!subscribed) {
		subscribed = true;
		Data::LiquidStore::ValueChanges(
		) | rpl::filter([](const QString &key) {
			return key == QString::fromLatin1(kFocusKey);
		}) | rpl::on_next([](const QString &) {
			cache = std::nullopt;
		}, lifetime);
	}
	if (!cache) {
		cache = LoadFocus();
	}
	if (cache->active.isEmpty()) {
		return false;
	}
	const auto i = ranges::find(
		cache->profiles,
		cache->active,
		&FocusProfile::name);
	if (i == end(cache->profiles)) {
		return false;
	}
	return !i->peers.contains(Data::LiquidStore::PeerKey(
		&history->session(),
		history->peer->id));
}

namespace {

[[nodiscard]] QString FoldersKey(not_null<Main::Session*> session) {
	return QString::fromLatin1(kLockedFoldersKey)
		+ ':'
		+ QString::number(session->userId().bare);
}

[[nodiscard]] base::flat_set<int> LockedFolders(
		not_null<Main::Session*> session) {
	auto result = base::flat_set<int>();
	for (const auto value
		: Data::LiquidStore::Value(FoldersKey(session)).toArray()) {
		result.emplace(value.toInt());
	}
	return result;
}

void SaveLockedFolders(
		not_null<Main::Session*> session,
		const base::flat_set<int> &ids) {
	auto list = QJsonArray();
	for (const auto id : ids) {
		list.push_back(id);
	}
	Data::LiquidStore::SetValue(
		FoldersKey(session),
		list.isEmpty() ? QJsonValue() : QJsonValue(list));
}

// The folder that was opened with the PIN, until another one is chosen.
int UnlockedFolder = 0;

// PasswordInput is not an RpWidget, so it goes into a wrapper row.
[[nodiscard]] Ui::PasswordInput *AddPasswordRow(
		not_null<Ui::GenericBox*> box,
		const QString &placeholder) {
	const auto wrap = box->addRow(object_ptr<Ui::RpWidget>(box));
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		wrap,
		st::defaultInputField,
		rpl::single(placeholder));
	wrap->resize(wrap->width(), field->height());
	wrap->widthValue() | rpl::on_next([=](int width) {
		field->resizeToWidth(width);
		field->moveToLeft(0, 0);
		wrap->resize(width, field->height());
	}, wrap->lifetime());
	return field;
}

} // namespace

void AskPin(
		not_null<SessionController*> controller,
		const QString &title,
		Fn<void()> unlocked) {
	if (!Data::LiquidStore::HasPin()) {
		ShowSetPinBox(controller, unlocked);
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		const auto field = AddPasswordRow(box, u"PIN"_q);
		const auto attempts = std::make_shared<int>(0);
		const auto submit = [=] {
			if (!Data::LiquidStore::CheckPin(field->getLastText())) {
				field->showError();
				if (++*attempts >= 5) {
					box->closeBox();
					controller->showToast(u"Неверный PIN"_q);
				}
				return;
			}
			box->closeBox();
			if (unlocked) {
				unlocked();
			}
		};
		QObject::connect(field, &Ui::MaskedInputField::submitted, submit);
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Открыть"_q), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowSetPinBox(
		not_null<SessionController*> controller,
		Fn<void()> done) {
	const auto had = Data::LiquidStore::HasPin();
	const auto setNew = [=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(had
				? u"Новый PIN"_q
				: u"Придумай PIN"_q));
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Он закрывает скрытые чаты и папки с замком. Хранится "
				"только на этом компьютере. Забудешь — скрытые чаты можно "
				"вернуть, только сбросив настройки aglergram."_q,
				st::boxDividerLabel));
			const auto first = AddPasswordRow(box, u"PIN, хотя бы 4 символа"_q);
			const auto second = AddPasswordRow(box, u"Ещё раз"_q);
			const auto submit = [=] {
				const auto pin = first->getLastText();
				if (pin.size() < 4) {
					first->showError();
					return;
				} else if (second->getLastText() != pin) {
					second->showError();
					return;
				}
				Data::LiquidStore::SetPin(pin);
				box->closeBox();
				controller->showToast(u"PIN сохранён 🔐"_q);
				if (done) {
					done();
				}
			};
			QObject::connect(
				first,
				&Ui::MaskedInputField::submitted,
				[=] { second->setFocus(); });
			QObject::connect(
				second,
				&Ui::MaskedInputField::submitted,
				submit);
			box->setFocusCallback([=] { first->setFocusFast(); });
			box->addButton(rpl::single(u"Сохранить"_q), submit);
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
	};
	if (had) {
		AskPin(controller, u"Сначала старый PIN"_q, setNew);
	} else {
		setNew();
	}
}

void HideChat(
		not_null<SessionController*> controller,
		not_null<History*> history) {
	const auto hide = [=] {
		const auto session = &history->session();
		Data::LiquidStore::SetHiddenChat(session, history->peer->id, true);
		history->updateChatListExistence();
		if (controller->activeChatCurrent().history() == history) {
			controller->clearSectionStack();
		}
		controller->showToast(u"🙈 Чат скрыт. Найти: Ctrl+Shift+K → "
			"«Скрытые чаты»"_q);
	};
	if (Data::LiquidStore::HasPin()) {
		hide();
	} else {
		ShowSetPinBox(controller, hide);
	}
}

void ShowHiddenChatsBox(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	AskPin(controller, u"🙈 Скрытые чаты"_q, [=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(u"🙈 Скрытые чаты"_q));
			box->setWidth(st::boxWideWidth);
			const auto list = Data::LiquidStore::HiddenChats(session);
			if (list.empty()) {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"Скрытых чатов нет. Правый клик по чату → "
					"«Скрыть чат»."_q,
					st::boxLabel));
			}
			for (const auto peerId : list) {
				const auto peer = session->data().peerLoaded(peerId);
				const auto name = peer ? peer->name() : u"Чат"_q;
				const auto open = box->addRow(
					object_ptr<Ui::SettingsButton>(
						box,
						rpl::single(u"💬 "_q + name),
						st::settingsButtonNoIcon),
					style::margins());
				open->setClickedCallback([=] {
					box->closeBox();
					controller->showPeerHistory(peerId);
				});
				const auto back = box->addRow(
					object_ptr<Ui::SettingsButton>(
						box,
						rpl::single(u"      вернуть в список"_q),
						st::settingsButtonNoIcon),
					style::margins());
				back->setClickedCallback([=] {
					Data::LiquidStore::SetHiddenChat(session, peerId, false);
					if (const auto history = session->data().historyLoaded(
							peerId)) {
						history->updateChatListExistence();
					}
					box->closeBox();
					controller->showToast(u"Чат снова в списке"_q);
				});
			}
			box->setMaxHeight(st::boxMaxListHeight * 2);
			box->addButton(rpl::single(u"Сменить PIN"_q), [=] {
				box->closeBox();
				ShowSetPinBox(controller);
			});
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}));
	});
}

bool AskFolderPin(
		not_null<SessionController*> controller,
		int filterId,
		Fn<void()> open) {
	if (!filterId) {
		UnlockedFolder = 0;
		return false;
	} else if (UnlockedFolder == filterId) {
		return false;
	} else if (!LockedFolders(&controller->session()).contains(filterId)
		|| !Data::LiquidStore::HasPin()) {
		UnlockedFolder = 0;
		return false;
	}
	AskPin(controller, u"🔐 Папка под PIN"_q, [=] {
		UnlockedFolder = filterId;
		open();
	});
	return true;
}

void ShowLockedFoldersBox(not_null<SessionController*> controller) {
	const auto session = &controller->session();
	const auto show = [=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(u"🔐 PIN на папки"_q));
			box->addRow(object_ptr<Ui::FlatLabel>(
				box,
				u"Отмеченные папки открываются только по PIN."_q,
				st::boxDividerLabel));
			const auto locked = LockedFolders(session);
			auto checks = std::make_shared<
				std::vector<std::pair<int, Ui::Checkbox*>>>();
			for (const auto &filter : session->data().chatsFilters().list()) {
				if (!filter.id()) {
					continue;
				}
				const auto check = box->addRow(object_ptr<Ui::Checkbox>(
					box,
					filter.title().text.text,
					locked.contains(filter.id()),
					st::defaultBoxCheckbox));
				checks->push_back({ filter.id(), check });
			}
			if (checks->empty()) {
				box->addRow(object_ptr<Ui::FlatLabel>(
					box,
					u"Папок пока нет."_q,
					st::boxLabel));
			}
			box->addButton(rpl::single(u"Сохранить"_q), [=] {
				auto now = base::flat_set<int>();
				for (const auto &[id, check] : *checks) {
					if (check->checked()) {
						now.emplace(id);
					}
				}
				SaveLockedFolders(session, now);
				if (!now.empty() && now.contains(
						controller->activeChatsFilterCurrent())) {
					UnlockedFolder = controller->activeChatsFilterCurrent();
				}
				box->closeBox();
				controller->showToast(now.empty()
					? u"Папки без PIN"_q
					: u"🔐 Готово"_q);
			});
			box->addButton(rpl::single(u"Сменить PIN"_q), [=] {
				box->closeBox();
				ShowSetPinBox(controller);
			});
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
	};
	if (Data::LiquidStore::HasPin()) {
		AskPin(controller, u"🔐 PIN на папки"_q, show);
	} else {
		ShowSetPinBox(controller, show);
	}
}

bool LockOnScreensChange() {
	return Data::LiquidStore::Value(
		QString::fromLatin1(kLockScreensKey)).toBool(false);
}

void SetLockOnScreensChange(bool enabled) {
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kLockScreensKey),
		enabled ? QJsonValue(true) : QJsonValue());
}

} // namespace Window::Liquid
