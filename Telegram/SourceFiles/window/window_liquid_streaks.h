/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Window::Liquid {

// A TikTok-like streak: days in a row both of you wrote in a private chat.
struct Streak {
	int count = 0;
	bool atRisk = false; // Yesterday counted, today not yet.

	explicit operator bool() const {
		return count > 0;
	}
};

[[nodiscard]] Streak StreakFor(not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<> StreakUpdates();

void StartStreaks(not_null<SessionController*> controller);

} // namespace Window::Liquid
