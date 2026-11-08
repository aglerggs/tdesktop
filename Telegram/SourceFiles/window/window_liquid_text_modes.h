/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// aglergram: text modes (kawaii, uwu, CAPS...) and dot commands
// (.coin, .dice, .heart...) applied to the text before it is sent, so
// there is no "edited" mark and no bot reading the chats.
namespace Window::Liquid {

enum class TextMode {
	None,
	Kawaii,
	Uwu,
	Caps,
	Mock,
	Zalgo,
	Reverse,
	Translit,
};

[[nodiscard]] TextMode CurrentTextMode();
void SetTextMode(TextMode mode);
[[nodiscard]] rpl::producer<TextMode> TextModeValue();
[[nodiscard]] QString TextModeTitle(TextMode mode);

struct TextTransform {
	bool changed = false; // The text is replaced by `text`.
	bool swallow = false; // A command that sends nothing (.kawaii, .off).
	QString text;
	QString toast;
};
[[nodiscard]] TextTransform TransformOutgoing(const QString &text);

// Right click on the message field: modes and the list of commands.
void FillTextModeMenu(
	not_null<Ui::PopupMenu*> menu,
	Fn<void(QString)> insertCommand);

void ShowTextCommandsBox(not_null<SessionController*> controller);

} // namespace Window::Liquid
