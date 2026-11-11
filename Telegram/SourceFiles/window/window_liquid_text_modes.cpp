/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_text_modes.h"

#include "base/random.h"
#include "data/data_liquid_store.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_liquid_plugins.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace Window::Liquid {
namespace {

constexpr auto kModeKey = "textMode";
constexpr auto kCommandsKey = "textCommands";

struct ModeInfo {
	TextMode mode = TextMode::None;
	QString command;
	QString title;
};

[[nodiscard]] const std::vector<ModeInfo> &Modes() {
	static const auto result = std::vector<ModeInfo>{
		{ TextMode::Kawaii, u".kawaii"_q, u"💖 Пикми"_q },
		{ TextMode::Uwu, u".uwu"_q, u"🐾 UwU"_q },
		{ TextMode::Caps, u".caps"_q, u"📢 КАПС"_q },
		{ TextMode::Mock, u".mock"_q, u"🤪 вОт ТаК"_q },
		{ TextMode::Zalgo, u".zalgo"_q, u"👁 Проклятый текст"_q },
		{ TextMode::Reverse, u".reverse"_q, u"🔄 Задом наперёд"_q },
		{ TextMode::Translit, u".translit"_q, u"🔤 Транслит"_q },
		{ TextMode::Script, u".script"_q, u"𝓢𝓬𝓻𝓲𝓹𝓽"_q },
		{ TextMode::Bold, u".bold"_q, u"𝐁𝐨𝐥𝐝"_q },
		{ TextMode::Double, u".double"_q, u"𝔻𝕠𝕦𝕓𝕝𝕖"_q },
		{ TextMode::SmallCaps, u".small"_q, u"ꜱᴍᴀʟʟ ᴄᴀᴘꜱ"_q },
		{ TextMode::Bubble, u".bubble"_q, u"ⓑⓤⓑⓑⓛⓔ"_q },
		{ TextMode::Wide, u".wide"_q, u"Ｗｉｄｅ"_q },
		{ TextMode::Upside, u".flip"_q, u"ʇxǝʇ ʌǝɹɥ 🙃"_q },
		{ TextMode::Strike, u".strike"_q, u"З̶а̶ч̶ё̶р̶к̶н̶у̶т̶ы̶й̶"_q },
	};
	return result;
}

struct CommandInfo {
	QString command;
	QString about;
};

[[nodiscard]] const std::vector<CommandInfo> &Commands() {
	static const auto result = std::vector<CommandInfo>{
		{ u".coin"_q, u"🪙 Монетка"_q },
		{ u".dice"_q, u"🎲 Кубик"_q },
		{ u".roll 1-100"_q, u"🔢 Случайное число"_q },
		{ u".8ball"_q, u"🎱 Шар предсказаний"_q },
		{ u".heart"_q, u"💗 Сердце из эмодзи"_q },
		{ u".shrug"_q, u"🤷 ¯\\_(ツ)_/¯"_q },
		{ u".time"_q, u"🕒 Который час"_q },
		{ u".off"_q, u"⏹ Выключить режим"_q },
	};
	return result;
}

rpl::variable<TextMode> &ModeVariable() {
	static auto result = rpl::variable<TextMode>([] {
		const auto value = Data::LiquidStore::Value(
			QString::fromLatin1(kModeKey)).toInt();
		return (value > 0 && value <= int(TextMode::Strike))
			? TextMode(value)
			: TextMode::None;
	}());
	return result;
}

template <typename T>
[[nodiscard]] const T &Pick(const std::vector<T> &list) {
	return list[base::RandomIndex(list.size())];
}

[[nodiscard]] bool Chance(int percent) {
	return int(base::RandomIndex(100)) < percent;
}

[[nodiscard]] QString Kawaii(const QString &text) {
	static const auto faces = std::vector<QString>{
		u"(ᵔᴥᵔ)"_q, u"(◕‿◕✿)"_q, u"(｡♥‿♥｡)"_q, u"(⁄ ⁄•⁄ω⁄•⁄ ⁄)"_q,
		u"(ﾉ◕ヮ◕)ﾉ*:･ﾟ✧"_q, u"(๑˃ᴗ˂)ﻭ"_q, u"(´｡• ω •｡`)"_q, u"(≧◡≦)"_q,
		u"(っ˘ω˘ς)"_q, u"ʕ•ᴥ•ʔ"_q,
	};
	static const auto actions = std::vector<QString>{
		u"*делает вид что всё нормально*"_q,
		u"*краснеет*"_q,
		u"*прячется за подушку*"_q,
		u"*машет лапкой*"_q,
		u"*смущённо хихикает*"_q,
		u"*обнимает*"_q,
		u"*тыкает пальчиками*"_q,
	};
	static const auto sparkles = std::vector<QString>{
		u"💫"_q, u"💕"_q, u"💗"_q, u"✨"_q, u"🌸"_q, u"🎀"_q,
	};
	auto words = text.split(' ', Qt::KeepEmptyParts);
	for (auto &word : words) {
		if (word.size() > 1 && word[0].isLetter() && Chance(25)) {
			const auto first = word.left(1);
			word = first + '-' + first.toLower() + word.mid(1);
		}
	}
	auto result = words.join(' ').replace('!', u"!!"_q);
	result += ' ' + Pick(faces);
	if (Chance(45)) {
		result += '\n' + Pick(actions) + ' ' + Pick(sparkles);
	}
	return result;
}

[[nodiscard]] QString Uwu(QString text) {
	static const auto tails = std::vector<QString>{
		u"uwu"_q, u"owo"_q, u">w<"_q, u"^w^"_q, u"nya~"_q,
	};
	auto result = QString();
	result.reserve(text.size());
	for (const auto ch : text) {
		const auto lower = ch.toLower();
		auto replaced = ch;
		if (lower == 'r' || lower == 'l') {
			replaced = ch.isUpper() ? QChar('W') : QChar('w');
		} else if (lower == QChar(0x0440) || lower == QChar(0x043B)) {
			// р, л -> в
			replaced = ch.isUpper() ? QChar(0x0412) : QChar(0x0432);
		}
		result.append(replaced);
	}
	return result + ' ' + Pick(tails);
}

[[nodiscard]] QString Mock(const QString &text) {
	auto result = QString();
	auto upper = false;
	for (const auto ch : text) {
		if (ch.isLetter()) {
			result.append(upper ? ch.toUpper() : ch.toLower());
			upper = !upper;
		} else {
			result.append(ch);
		}
	}
	return result;
}

[[nodiscard]] QString Zalgo(const QString &text) {
	auto result = QString();
	for (const auto ch : text) {
		result.append(ch);
		if (!ch.isLetterOrNumber()) {
			continue;
		}
		const auto count = 1 + int(base::RandomIndex(3));
		for (auto i = 0; i != count; ++i) {
			// Combining diacritical marks U+0300..U+036F.
			result.append(QChar(ushort(0x0300 + base::RandomIndex(0x70))));
		}
	}
	return result;
}

[[nodiscard]] QString Reverse(const QString &text) {
	// By code points, so emoji and surrogate pairs stay whole.
	auto points = text.toUcs4();
	std::reverse(points.begin(), points.end());
	return QString::fromUcs4(points.constData(), points.size());
}

[[nodiscard]] QString Translit(const QString &text) {
	static const auto map = base::flat_map<QChar, QString>{
		{ QChar(0x0430), u"a"_q }, { QChar(0x0431), u"b"_q },
		{ QChar(0x0432), u"v"_q }, { QChar(0x0433), u"g"_q },
		{ QChar(0x0434), u"d"_q }, { QChar(0x0435), u"e"_q },
		{ QChar(0x0451), u"yo"_q }, { QChar(0x0436), u"zh"_q },
		{ QChar(0x0437), u"z"_q }, { QChar(0x0438), u"i"_q },
		{ QChar(0x0439), u"y"_q }, { QChar(0x043A), u"k"_q },
		{ QChar(0x043B), u"l"_q }, { QChar(0x043C), u"m"_q },
		{ QChar(0x043D), u"n"_q }, { QChar(0x043E), u"o"_q },
		{ QChar(0x043F), u"p"_q }, { QChar(0x0440), u"r"_q },
		{ QChar(0x0441), u"s"_q }, { QChar(0x0442), u"t"_q },
		{ QChar(0x0443), u"u"_q }, { QChar(0x0444), u"f"_q },
		{ QChar(0x0445), u"h"_q }, { QChar(0x0446), u"ts"_q },
		{ QChar(0x0447), u"ch"_q }, { QChar(0x0448), u"sh"_q },
		{ QChar(0x0449), u"sch"_q }, { QChar(0x044A), QString() },
		{ QChar(0x044B), u"y"_q }, { QChar(0x044C), QString() },
		{ QChar(0x044D), u"e"_q }, { QChar(0x044E), u"yu"_q },
		{ QChar(0x044F), u"ya"_q },
	};
	auto result = QString();
	for (const auto ch : text) {
		const auto i = map.find(ch.toLower());
		if (i == end(map)) {
			result.append(ch);
		} else if (ch.isUpper() && !i->second.isEmpty()) {
			result.append(i->second.left(1).toUpper() + i->second.mid(1));
		} else {
			result.append(i->second);
		}
	}
	return result;
}

// Unicode "fonts": they exist for Latin only, so Cyrillic goes through
// the transliteration first, and digits are styled where possible.
[[nodiscard]] QString MapLatin(
		const QString &text,
		char32_t upper,
		char32_t lower,
		char32_t digits,
		const base::flat_map<QChar, char32_t> &exceptions = {}) {
	auto result = QString();
	result.reserve(text.size() * 2);
	const auto source = Translit(text);
	for (const auto ch : source) {
		const auto e = exceptions.find(ch);
		auto code = char32_t(0);
		if (e != end(exceptions)) {
			code = e->second;
		} else if (ch >= 'A' && ch <= 'Z' && upper) {
			code = upper + (ch.unicode() - 'A');
		} else if (ch >= 'a' && ch <= 'z' && lower) {
			code = lower + (ch.unicode() - 'a');
		} else if (ch >= '0' && ch <= '9' && digits) {
			code = digits + (ch.unicode() - '0');
		}
		if (code) {
			result += QString::fromUcs4(&code, 1);
		} else {
			result += ch;
		}
	}
	return result;
}

[[nodiscard]] QString Script(const QString &text) {
	return MapLatin(text, 0x1D4D0, 0x1D4EA, 0);
}

[[nodiscard]] QString Bold(const QString &text) {
	return MapLatin(text, 0x1D400, 0x1D41A, 0x1D7CE);
}

[[nodiscard]] QString Double(const QString &text) {
	static const auto exceptions = base::flat_map<QChar, char32_t>{
		{ QChar('C'), 0x2102 }, { QChar('H'), 0x210D },
		{ QChar('N'), 0x2115 }, { QChar('P'), 0x2119 },
		{ QChar('Q'), 0x211A }, { QChar('R'), 0x211D },
		{ QChar('Z'), 0x2124 },
	};
	return MapLatin(text, 0x1D538, 0x1D552, 0x1D7D8, exceptions);
}

[[nodiscard]] QString Bubble(const QString &text) {
	static const auto exceptions = base::flat_map<QChar, char32_t>{
		{ QChar('0'), 0x24EA },
	};
	return MapLatin(text, 0x24B6, 0x24D0, 0x2460 - 1, exceptions);
}

[[nodiscard]] QString Wide(const QString &text) {
	auto result = QString();
	for (const auto ch : text) {
		const auto code = ch.unicode();
		if (code == ' ') {
			result += QChar(0x3000);
		} else if (code > 0x20 && code < 0x7F) {
			result += QChar(code - 0x20 + 0xFF00);
		} else {
			result += ch;
		}
	}
	return result;
}

[[nodiscard]] QString SmallCaps(const QString &text) {
	static const auto map = base::flat_map<QChar, QString>{
		{ 'a', u"ᴀ"_q }, { 'b', u"ʙ"_q }, { 'c', u"ᴄ"_q }, { 'd', u"ᴅ"_q },
		{ 'e', u"ᴇ"_q }, { 'f', u"ꜰ"_q }, { 'g', u"ɢ"_q }, { 'h', u"ʜ"_q },
		{ 'i', u"ɪ"_q }, { 'j', u"ᴊ"_q }, { 'k', u"ᴋ"_q }, { 'l', u"ʟ"_q },
		{ 'm', u"ᴍ"_q }, { 'n', u"ɴ"_q }, { 'o', u"ᴏ"_q }, { 'p', u"ᴘ"_q },
		{ 'q', u"ǫ"_q }, { 'r', u"ʀ"_q }, { 's', u"ꜱ"_q }, { 't', u"ᴛ"_q },
		{ 'u', u"ᴜ"_q }, { 'v', u"ᴠ"_q }, { 'w', u"ᴡ"_q }, { 'x', u"x"_q },
		{ 'y', u"ʏ"_q }, { 'z', u"ᴢ"_q },
	};
	auto result = QString();
	for (const auto ch : Translit(text).toLower()) {
		const auto i = map.find(ch);
		result += (i != end(map)) ? i->second : QString(ch);
	}
	return result;
}

[[nodiscard]] QString Upside(const QString &text) {
	static const auto map = base::flat_map<QChar, QString>{
		{ 'a', u"ɐ"_q }, { 'b', u"q"_q }, { 'c', u"ɔ"_q }, { 'd', u"p"_q },
		{ 'e', u"ǝ"_q }, { 'f', u"ɟ"_q }, { 'g', u"ƃ"_q }, { 'h', u"ɥ"_q },
		{ 'i', u"ᴉ"_q }, { 'j', u"ɾ"_q }, { 'k', u"ʞ"_q }, { 'l', u"l"_q },
		{ 'm', u"ɯ"_q }, { 'n', u"u"_q }, { 'o', u"o"_q }, { 'p', u"d"_q },
		{ 'q', u"b"_q }, { 'r', u"ɹ"_q }, { 's', u"s"_q }, { 't', u"ʇ"_q },
		{ 'u', u"n"_q }, { 'v', u"ʌ"_q }, { 'w', u"ʍ"_q }, { 'x', u"x"_q },
		{ 'y', u"ʎ"_q }, { 'z', u"z"_q }, { '.', u"˙"_q }, { ',', u"'"_q },
		{ '?', u"¿"_q }, { '!', u"¡"_q }, { '(', u")"_q }, { ')', u"("_q },
		{ '\'', u","_q }, { '_', u"‾"_q }, { '6', u"9"_q }, { '9', u"6"_q },
	};
	auto result = QString();
	const auto source = Translit(text).toLower();
	for (auto i = source.size(); i > 0;) {
		const auto ch = source[--i];
		const auto j = map.find(ch);
		result += (j != end(map)) ? j->second : QString(ch);
	}
	return result;
}

[[nodiscard]] QString Strike(const QString &text) {
	auto result = QString();
	result.reserve(text.size() * 2);
	for (const auto ch : text) {
		result += ch;
		if (!ch.isSpace() && !ch.isSurrogate()) {
			result += QChar(0x0336);
		}
	}
	return result;
}

[[nodiscard]] QString ApplyMode(TextMode mode, const QString &text) {
	switch (mode) {
	case TextMode::Kawaii: return Kawaii(text);
	case TextMode::Uwu: return Uwu(text);
	case TextMode::Caps: return text.toUpper();
	case TextMode::Mock: return Mock(text);
	case TextMode::Zalgo: return Zalgo(text);
	case TextMode::Reverse: return Reverse(text);
	case TextMode::Translit: return Translit(text);
	case TextMode::Script: return Script(text);
	case TextMode::Bold: return Bold(text);
	case TextMode::Double: return Double(text);
	case TextMode::SmallCaps: return SmallCaps(text);
	case TextMode::Bubble: return Bubble(text);
	case TextMode::Wide: return Wide(text);
	case TextMode::Upside: return Upside(text);
	case TextMode::Strike: return Strike(text);
	case TextMode::None: break;
	}
	return text;
}

[[nodiscard]] QString Heart() {
	static const auto fills = std::vector<QString>{
		u"🥰"_q, u"❤️"_q, u"💗"_q, u"💜"_q, u"🩷"_q,
	};
	const auto fill = Pick(fills);
	const auto pattern = std::vector<const char*>{
		"..##.##..",
		".#######.",
		"#########",
		"#########",
		".#######.",
		"..#####..",
		"...###...",
		"....#....",
	};
	auto result = QStringList();
	for (const auto row : pattern) {
		auto line = QString();
		for (auto c = row; *c; ++c) {
			line += (*c == '#') ? fill : u"⬜"_q;
		}
		result.push_back(line);
	}
	return result.join('\n');
}

[[nodiscard]] base::flat_map<QString, QString> CustomCommands() {
	auto result = base::flat_map<QString, QString>();
	const auto object = Data::LiquidStore::Value(
		QString::fromLatin1(kCommandsKey)).toObject();
	for (auto i = object.begin(); i != object.end(); ++i) {
		result.emplace(i.key(), i.value().toString());
	}
	return result;
}

} // namespace

TextMode CurrentTextMode() {
	if (!PluginEnabled(u"textmodes"_q)) {
		return TextMode::None;
	}
	return ModeVariable().current();
}

void SetTextMode(TextMode mode) {
	ModeVariable() = mode;
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kModeKey),
		(mode == TextMode::None) ? QJsonValue() : QJsonValue(int(mode)));
}

rpl::producer<TextMode> TextModeValue() {
	return ModeVariable().value();
}

QString TextModeTitle(TextMode mode) {
	for (const auto &info : Modes()) {
		if (info.mode == mode) {
			return info.title;
		}
	}
	return QString();
}

TextTransform TransformOutgoing(const QString &text) {
	if (!PluginEnabled(u"textmodes"_q)) {
		return {};
	}
	const auto trimmed = text.trimmed();
	if (trimmed.startsWith('.') && !trimmed.contains('\n')) {
		const auto parts = trimmed.split(' ', Qt::SkipEmptyParts);
		const auto command = parts.value(0).toLower();
		for (const auto &info : Modes()) {
			if (command == info.command) {
				const auto off = (CurrentTextMode() == info.mode);
				SetTextMode(off ? TextMode::None : info.mode);
				return {
					.swallow = true,
					.toast = off
						? (info.title + u" выключен"_q)
						: (info.title + u" включён. Выключить: "_q
							+ info.command + u" или .off"_q),
				};
			}
		}
		if (command == u".off"_q) {
			SetTextMode(TextMode::None);
			return { .swallow = true, .toast = u"Режим текста выключен"_q };
		} else if (command == u".coin"_q) {
			return {
				.changed = true,
				.text = Chance(50) ? u"🪙 Орёл!"_q : u"💫 Решка!"_q,
			};
		} else if (command == u".dice"_q) {
			static const auto faces = std::vector<QString>{
				u"⚀"_q, u"⚁"_q, u"⚂"_q, u"⚃"_q, u"⚄"_q, u"⚅"_q,
			};
			const auto value = int(base::RandomIndex(6));
			return {
				.changed = true,
				.text = u"🎲 "_q + faces[value]
					+ u" — "_q + QString::number(value + 1),
			};
		} else if (command == u".roll"_q) {
			// .roll -> 1-100, .roll N -> 1-N, .roll A-B (or A B) -> A-B
			auto from = int64(1);
			auto till = int64(100);
			const auto range = trimmed.mid(command.size()).trimmed();
			const auto match = QRegularExpression(
				u"^(-?\\d+)\\s*(?:-|–|—|\\.\\.|\\s)\\s*(-?\\d+)$"_q
			).match(range);
			if (match.hasMatch()) {
				from = match.captured(1).toLongLong();
				till = match.captured(2).toLongLong();
			} else if (!range.isEmpty()) {
				till = range.toLongLong();
			}
			if (till < from) {
				std::swap(from, till);
			}
			const auto span = std::min<int64>(till - from, 1'000'000'000);
			const auto value = from + int64(base::RandomIndex(span + 1));
			return {
				.changed = true,
				.text = u"🎲 "_q + QString::number(value)
					+ u" ("_q + QString::number(from)
					+ '-' + QString::number(till) + ')',
			};
		} else if (command == u".8ball"_q) {
			static const auto answers = std::vector<QString>{
				u"Бесспорно"_q, u"Предрешено"_q, u"Никаких сомнений"_q,
				u"Определённо да"_q, u"Можешь быть уверен в этом"_q,
				u"Мне кажется — да"_q, u"Вероятнее всего"_q,
				u"Хорошие перспективы"_q, u"Знаки говорят — да"_q,
				u"Пока не ясно, попробуй снова"_q, u"Спроси позже"_q,
				u"Лучше не рассказывать"_q, u"Сейчас нельзя предсказать"_q,
				u"Даже не думай"_q, u"Мой ответ — нет"_q,
				u"По моим данным — нет"_q, u"Перспективы не очень"_q,
				u"Весьма сомнительно"_q,
			};
			const auto question = trimmed.mid(command.size()).trimmed();
			return {
				.changed = true,
				.text = (question.isEmpty()
					? QString()
					: (u"❓ "_q + question + '\n'))
					+ u"🎱 "_q + Pick(answers),
			};
		} else if (command == u".heart"_q) {
			return { .changed = true, .text = Heart() };
		} else if (command == u".shrug"_q) {
			return { .changed = true, .text = u"¯\\_(ツ)_/¯"_q };
		} else if (command == u".time"_q) {
			return {
				.changed = true,
				.text = u"🕒 "_q + QDateTime::currentDateTime().toString(
					u"HH:mm, dd.MM.yyyy"_q),
			};
		}
		const auto custom = CustomCommands();
		const auto i = custom.find(command.mid(1));
		if (i != end(custom)) {
			return { .changed = true, .text = i->second };
		}
	}
	const auto mode = CurrentTextMode();
	if (mode == TextMode::None || trimmed.isEmpty()) {
		return {};
	}
	return { .changed = true, .text = ApplyMode(mode, text) };
}

void FillTextModeMenu(
		not_null<Ui::PopupMenu*> menu,
		Fn<void(QString)> insertCommand) {
	if (!PluginEnabled(u"textmodes"_q)) {
		return;
	}
	menu->addSeparator();
	auto modes = std::make_unique<Ui::PopupMenu>(
		menu->parentWidget(),
		menu->st());
	const auto current = CurrentTextMode();
	for (const auto &info : Modes()) {
		const auto mode = info.mode;
		modes->addAction(
			((current == mode) ? u"✓ "_q : QString()) + info.title,
			[=] {
				SetTextMode((CurrentTextMode() == mode)
					? TextMode::None
					: mode);
			});
	}
	if (current != TextMode::None) {
		modes->addSeparator();
		modes->addAction(u"Выключить"_q, [] {
			SetTextMode(TextMode::None);
		});
	}
	menu->addAction(
		(current == TextMode::None)
			? u"✨ Режим текста"_q
			: (u"✨ Режим: "_q + TextModeTitle(current)),
		std::move(modes),
		&st::menuIconEmoji);

	auto commands = std::make_unique<Ui::PopupMenu>(
		menu->parentWidget(),
		menu->st());
	for (const auto &info : Commands()) {
		const auto command = info.command;
		commands->addAction(info.about + u"  "_q + command, [=] {
			insertCommand(command);
		});
	}
	for (const auto &[name, text] : CustomCommands()) {
		const auto command = '.' + name;
		commands->addAction(u"⭐ "_q + command, [=] {
			insertCommand(command);
		});
	}
	menu->addAction(u"⚡ Команды"_q, std::move(commands), &st::menuIconBotCommands);
}

void ShowTextCommandsBox(not_null<SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"⚡ Свои команды"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Одна команда на строку: имя = текст. Например:\n"
			"адрес = ул. Пушкина, д. 1\n"
			"Потом пишешь .адрес и отправляешь — уйдёт текст."_q,
			st::boxDividerLabel));
		auto lines = QStringList();
		for (const auto &[name, text] : CustomCommands()) {
			lines.push_back(name + u" = "_q + text);
		}
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"имя = текст"_q),
			lines.join('\n')));
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->addButton(rpl::single(u"Сохранить"_q), [=] {
			auto object = QJsonObject();
			for (const auto &line : field->getLastText().split('\n')) {
				const auto index = line.indexOf('=');
				if (index <= 0) {
					continue;
				}
				auto name = line.left(index).trimmed().toLower();
				if (name.startsWith('.')) {
					name = name.mid(1);
				}
				const auto text = line.mid(index + 1).trimmed();
				if (!name.isEmpty() && !name.contains(' ') && !text.isEmpty()) {
					object.insert(name.left(32), text.left(4000));
				}
			}
			Data::LiquidStore::SetValue(
				QString::fromLatin1(kCommandsKey),
				object.isEmpty() ? QJsonValue() : QJsonValue(object));
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Window::Liquid
