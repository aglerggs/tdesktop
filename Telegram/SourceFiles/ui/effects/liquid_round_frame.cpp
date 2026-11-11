/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/liquid_round_frame.h"

#include "ui/painter.h"

#include <QtGui/QConicalGradient>

#include <atomic>

namespace Ui::LiquidRound {
namespace {

std::atomic<int> GlobalFrame = 0;

[[nodiscard]] std::vector<QColor> Colors(Frame frame) {
	switch (frame) {
	case Frame::Neon: return {
		QColor(0xFF, 0x2D, 0x95),
		QColor(0xFF, 0xD6, 0x00),
		QColor(0x00, 0xE6, 0x76),
		QColor(0x00, 0xB0, 0xFF),
		QColor(0xD5, 0x00, 0xF9),
	};
	case Frame::Fire: return {
		QColor(0xFF, 0x3D, 0x00),
		QColor(0xFF, 0xB3, 0x00),
		QColor(0xFF, 0x17, 0x44),
		QColor(0xFF, 0x6D, 0x00),
	};
	case Frame::Ice: return {
		QColor(0x00, 0xE5, 0xFF),
		QColor(0xE0, 0xF7, 0xFA),
		QColor(0x29, 0x79, 0xFF),
		QColor(0x80, 0xD8, 0xFF),
	};
	case Frame::Gold: return {
		QColor(0xFF, 0xD5, 0x4F),
		QColor(0xFF, 0xF8, 0xE1),
		QColor(0xFF, 0x8F, 0x00),
		QColor(0xFF, 0xE0, 0x82),
	};
	case Frame::Love: return {
		QColor(0xFF, 0x40, 0x81),
		QColor(0xF8, 0xBB, 0xD0),
		QColor(0xE0, 0x40, 0xFB),
		QColor(0xFF, 0x80, 0xAB),
	};
	case Frame::None: break;
	}
	return {};
}

} // namespace

Frame Current() {
	return Frame(GlobalFrame.load());
}

void SetCurrent(Frame frame) {
	GlobalFrame = int(frame);
}

void Paint(QImage &square, Frame frame, crl::time now) {
	const auto colors = Colors(frame);
	if (colors.empty() || square.isNull()) {
		return;
	}
	const auto side = float64(std::min(square.width(), square.height()));
	const auto center = QPointF(square.width() / 2., square.height() / 2.);
	const auto spin = (now % 4000) / 4000.;
	const auto pulse = 0.5 + 0.5 * std::sin((now % 1400) / 1400. * 2 * M_PI);
	const auto base = side * ((frame == Frame::Fire)
		? (0.04 + 0.012 * pulse)
		: 0.042);

	auto gradient = QConicalGradient(center, -spin * 360.);
	const auto count = int(colors.size());
	for (auto i = 0; i <= count; ++i) {
		gradient.setColorAt(i / float64(count), colors[i % count]);
	}

	auto p = QPainter(&square);
	PainterHighQualityEnabler hq(p);
	p.setBrush(Qt::NoBrush);

	// The circle the video is cut to; the ring hugs its edge from inside.
	const auto edge = side / 2. - side * 0.004;
	// Soft glow first, inward.
	constexpr auto kGlowSteps = 8;
	for (auto k = kGlowSteps; k > 0; --k) {
		const auto width = base * (1. + k * 0.16);
		p.setOpacity((0.05 + 0.03 * pulse) * (kGlowSteps - k + 1) / kGlowSteps);
		p.setPen(QPen(QBrush(gradient), width));
		const auto radius = edge - width / 2.;
		p.drawEllipse(center, radius, radius);
	}
	// The bright ring itself with a thin white core.
	p.setOpacity(1.);
	p.setPen(QPen(QBrush(gradient), base));
	const auto radius = edge - base / 2.;
	p.drawEllipse(center, radius, radius);
	p.setOpacity(0.35 + 0.3 * pulse);
	p.setPen(QPen(QColor(255, 255, 255), base * 0.22));
	p.drawEllipse(center, radius, radius);
}

} // namespace Ui::LiquidRound
