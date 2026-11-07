/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {

class SessionController;

// Liquid glass fork: a startup splash (a zoom into the touching fingers)
// followed by a welcome screen with quick access to the recent chats.
void ShowLiquidIntro(
	not_null<QWidget*> parent,
	not_null<SessionController*> controller);

} // namespace Window
