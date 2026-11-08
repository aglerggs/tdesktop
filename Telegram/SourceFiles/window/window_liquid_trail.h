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

// aglergram: a glowing trail behind the mouse cursor over the window.
// It does not catch clicks and only repaints where the trail is.
namespace Window::Liquid {

enum class TrailStyle {
	None,
	Rainbow,
	Comet,
	Sparks,
};

[[nodiscard]] TrailStyle CurrentTrailStyle();
void SetTrailStyle(TrailStyle style);
[[nodiscard]] QString TrailStyleTitle(TrailStyle style);

void StartCursorTrail(not_null<SessionController*> controller);
void ShowCursorTrailBox(not_null<SessionController*> controller);

} // namespace Window::Liquid
