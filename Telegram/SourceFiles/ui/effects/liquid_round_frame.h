/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// aglergram: an animated glowing ring drawn into the frames of a round
// video while it is recorded, so everyone sees it in any Telegram.
namespace Ui::LiquidRound {

enum class Frame {
	None,
	Neon,
	Fire,
	Ice,
	Gold,
	Love,
};

// Set on the main thread, read by the recorder for every frame.
[[nodiscard]] Frame Current();
void SetCurrent(Frame frame);

// `square` is the round video frame before it is cut to a circle.
void Paint(QImage &square, Frame frame, crl::time now);

} // namespace Ui::LiquidRound
