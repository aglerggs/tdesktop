/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

// aglergram: "listening now", like in Discord. The track playing in
// Spotify goes to the aglergram service on blinistiy.fun, and other
// aglergram users see it in the profile. Telegram itself knows nothing.
namespace Window::Liquid {

struct NowPlaying {
	QString title;
	QString artist;
};

[[nodiscard]] const NowPlaying *NowPlayingFor(PeerId peer);
[[nodiscard]] rpl::producer<> NowPlayingUpdates();

void StartMusic(not_null<SessionController*> controller);
void ShowMusicBox(not_null<SessionController*> controller);

} // namespace Window::Liquid
