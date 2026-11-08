/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_aura.h"

#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QRadialGradient>

#include <cmath>

namespace Window::Liquid {
namespace {

constexpr auto kPrefix = "aura:";
constexpr auto kPi = 3.14159265358979323846;

// Stable "random" numbers in [0, 1) for the particle with this index.
[[nodiscard]] float64 Noise(int index, int salt) {
	auto x = uint32(index) * 374761393u + uint32(salt) * 668265263u;
	x = (x ^ (x >> 13)) * 1274126177u;
	x ^= (x >> 16);
	return (x & 0xFFFFFF) / float64(0x1000000);
}

[[nodiscard]] QColor Mix(QColor a, QColor b, float64 t) {
	t = std::clamp(t, 0., 1.);
	return QColor::fromRgbF(
		a.redF() + (b.redF() - a.redF()) * t,
		a.greenF() + (b.greenF() - a.greenF()) * t,
		a.blueF() + (b.blueF() - a.blueF()) * t,
		a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

[[nodiscard]] QColor Alpha(QColor color, float64 alpha) {
	color.setAlphaF(std::clamp(alpha, 0., 1.));
	return color;
}

void SoftBlob(
		QPainter &p,
		QPointF center,
		float64 radius,
		QColor color,
		float64 alpha) {
	auto gradient = QRadialGradient(center, radius);
	gradient.setColorAt(0., Alpha(color, alpha));
	gradient.setColorAt(0.55, Alpha(color, alpha * 0.45));
	gradient.setColorAt(1., Alpha(color, 0.));
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawEllipse(center, radius, radius);
}

void VerticalGradient(QPainter &p, QSize size, QColor top, QColor bottom) {
	auto gradient = QLinearGradient(0, 0, 0, size.height());
	gradient.setColorAt(0., top);
	gradient.setColorAt(1., bottom);
	p.fillRect(QRect(QPoint(), size), gradient);
}

// Aura: big soft blobs drifting on slow Lissajous paths.
struct Palette {
	QColor base;
	std::array<QColor, 5> blobs;
};

void Aura(QPainter &p, QSize size, float64 t, const Palette &palette) {
	p.fillRect(QRect(QPoint(), size), palette.base);
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	for (auto i = 0; i != int(palette.blobs.size()); ++i) {
		const auto sx = 0.05 + Noise(i, 1) * 0.08;
		const auto sy = 0.04 + Noise(i, 2) * 0.07;
		const auto x = w * (0.5 + 0.42 * std::sin(t * sx + Noise(i, 3) * 6.28));
		const auto y = h * (0.5 + 0.42 * std::cos(t * sy + Noise(i, 4) * 6.28));
		const auto pulse = 0.85 + 0.15 * std::sin(t * 0.3 + i);
		const auto radius = std::max(w, h) * (0.3 + Noise(i, 5) * 0.22) * pulse;
		SoftBlob(p, { x, y }, radius, palette.blobs[i], 0.75);
	}
}

void Stars(QPainter &p, QSize size, float64 t, int count, float64 height) {
	p.setPen(Qt::NoPen);
	for (auto i = 0; i != count; ++i) {
		const auto x = Noise(i, 11) * size.width();
		const auto y = Noise(i, 12) * size.height() * height;
		const auto twinkle = 0.5 + 0.5 * std::sin(t * (1. + Noise(i, 13) * 2.)
			+ Noise(i, 14) * 6.28);
		const auto r = 0.4 + Noise(i, 15) * 1.1;
		p.setBrush(QColor::fromRgbF(1., 1., 1., 0.25 + 0.75 * twinkle));
		p.drawEllipse(QPointF(x, y), r, r);
	}
}

void Aurora(QPainter &p, QSize size, float64 t) {
	VerticalGradient(p, size, QColor(4, 8, 24), QColor(10, 24, 40));
	Stars(p, size, t, 140, 0.8);
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	p.setCompositionMode(QPainter::CompositionMode_Screen);
	const auto colors = std::array<QColor, 3>{
		QColor(60, 255, 160),
		QColor(80, 200, 255),
		QColor(190, 90, 255),
	};
	for (auto band = 0; band != 3; ++band) {
		const auto base = h * (0.5 + band * 0.08);
		const auto step = 3.;
		for (auto x = 0.; x < w; x += step) {
			const auto u = x / w;
			const auto curve = base
				+ std::sin(u * 4. + t * (0.18 + band * 0.05) + band * 1.7) * h * 0.08
				+ std::sin(u * 11. - t * 0.3 + band) * h * 0.025;
			// Rays brighten and fade along the curtain.
			const auto ray = 0.55
				+ 0.45 * std::sin(u * 37. + t * (0.6 + band * 0.2))
					* std::sin(u * 9. - t * 0.25 + band);
			const auto height = h * (0.18 + 0.16 * (0.5 + 0.5
				* std::sin(u * 6. + t * 0.2 + band * 2.)));
			const auto intensity = std::clamp(ray, 0., 1.)
				* (0.35 + 0.2 * std::sin(t * 0.4 + band));
			auto gradient = QLinearGradient(0, curve, 0, curve - height);
			const auto top = (band == 2) ? colors[2] : colors[1];
			gradient.setColorAt(0., Alpha(colors[band == 2 ? 2 : 0], 0.));
			gradient.setColorAt(0.06, Alpha(colors[band == 2 ? 2 : 0], intensity));
			gradient.setColorAt(0.45, Alpha(top, intensity * 0.5));
			gradient.setColorAt(1., Alpha(top, 0.));
			p.fillRect(QRectF(x, curve - height, step, height + h * 0.01), gradient);
		}
	}
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	// Dark hills at the bottom.
	auto hills = QPainterPath();
	hills.moveTo(0, h);
	for (auto s = 0; s <= 32; ++s) {
		const auto x = w * s / 32.;
		hills.lineTo(x, h * 0.86 - std::sin(x / w * 7. + 1.) * h * 0.04
			- std::sin(x / w * 17.) * h * 0.015);
	}
	hills.lineTo(w, h);
	hills.closeSubpath();
	p.setBrush(QColor(3, 6, 14));
	p.drawPath(hills);
}

void Snow(QPainter &p, QSize size, float64 t) {
	VerticalGradient(p, size, QColor(18, 22, 52), QColor(70, 50, 100));
	SoftBlob(
		p,
		{ size.width() * 0.8, size.height() * 0.18 },
		size.height() * 0.5,
		QColor(160, 170, 255),
		0.25);
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	p.setPen(Qt::NoPen);
	for (auto i = 0; i != 220; ++i) {
		const auto depth = Noise(i, 21);
		const auto speed = 12. + depth * 40.;
		const auto y = std::fmod(Noise(i, 22) * h + t * speed, h + 10.) - 5.;
		const auto x = std::fmod(
			Noise(i, 23) * w
				+ std::sin(t * (0.5 + depth) + Noise(i, 24) * 6.28) * 14.
				+ t * 6.,
			w + 10.) - 5.;
		const auto r = 0.6 + depth * 2.2;
		p.setBrush(QColor::fromRgbF(1., 1., 1., 0.35 + depth * 0.6));
		p.drawEllipse(QPointF(x, y), r, r);
	}
}

void Rain(QPainter &p, QSize size, float64 t) {
	VerticalGradient(p, size, QColor(14, 18, 30), QColor(28, 30, 44));
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	// City lights out of focus.
	p.setCompositionMode(QPainter::CompositionMode_Screen);
	const auto lights = std::array<QColor, 4>{
		QColor(255, 140, 60),
		QColor(255, 80, 140),
		QColor(80, 160, 255),
		QColor(255, 220, 120),
	};
	for (auto i = 0; i != 34; ++i) {
		const auto x = Noise(i, 31) * w;
		const auto y = h * (0.35 + Noise(i, 32) * 0.6);
		const auto r = h * (0.05 + Noise(i, 33) * 0.1);
		const auto pulse = 0.6 + 0.4 * std::sin(t * (0.3 + Noise(i, 34)) + i);
		SoftBlob(p, { x, y }, r, lights[i % lights.size()], 0.6 * pulse);
	}
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	// Falling streaks.
	for (auto i = 0; i != 160; ++i) {
		const auto depth = Noise(i, 35);
		const auto speed = 260. + depth * 320.;
		const auto length = 8. + depth * 18.;
		const auto x = Noise(i, 36) * w;
		const auto y = std::fmod(Noise(i, 37) * h + t * speed, h + length)
			- length;
		auto pen = QPen(QColor::fromRgbF(0.75, 0.82, 1., 0.12 + depth * 0.3));
		pen.setWidthF(0.6 + depth * 0.8);
		p.setPen(pen);
		p.drawLine(QPointF(x, y), QPointF(x - length * 0.15, y + length));
	}
	// Drops on the glass.
	p.setPen(Qt::NoPen);
	for (auto i = 0; i != 40; ++i) {
		const auto life = std::fmod(t * 0.15 + Noise(i, 38), 1.);
		const auto x = Noise(i, 39) * w;
		const auto y = Noise(i, 40) * h + life * h * 0.15;
		const auto r = 1.2 + Noise(i, 41) * 2.6;
		p.setBrush(QColor::fromRgbF(0.85, 0.9, 1., 0.18 * (1. - life)));
		p.drawEllipse(QPointF(x, y), r, r * 1.2);
	}
}

void Fireflies(QPainter &p, QSize size, float64 t) {
	VerticalGradient(p, size, QColor(6, 22, 24), QColor(4, 34, 28));
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	p.setCompositionMode(QPainter::CompositionMode_Screen);
	// A dark forest line at the bottom.
	SoftBlob(p, { w * 0.3, h * 1.1 }, h * 0.6, QColor(30, 90, 60), 0.5);
	SoftBlob(p, { w * 0.8, h * 1.05 }, h * 0.5, QColor(20, 80, 70), 0.45);
	for (auto i = 0; i != 70; ++i) {
		const auto x = w * (0.5 + 0.48 * std::sin(t * (0.04 + Noise(i, 51) * 0.08)
			+ Noise(i, 52) * 6.28));
		const auto y = h * (0.5 + 0.46 * std::sin(t * (0.05 + Noise(i, 53) * 0.07)
			+ Noise(i, 54) * 6.28));
		const auto blink = std::pow(
			0.5 + 0.5 * std::sin(t * (0.8 + Noise(i, 55)) + Noise(i, 56) * 6.28),
			3.);
		const auto r = 14. + Noise(i, 57) * 22.;
		const auto visible = 0.25 + 0.75 * blink;
		SoftBlob(p, { x, y }, r, QColor(255, 220, 110), 0.55 * visible);
		SoftBlob(p, { x, y }, r * 0.22, QColor(255, 255, 220), visible);
	}
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void Lava(QPainter &p, QSize size, float64 t) {
	VerticalGradient(p, size, QColor(40, 6, 30), QColor(20, 4, 40));
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	p.setCompositionMode(QPainter::CompositionMode_Screen);
	const auto colors = std::array<QColor, 3>{
		QColor(255, 90, 60),
		QColor(255, 60, 160),
		QColor(255, 170, 60),
	};
	for (auto i = 0; i != 12; ++i) {
		const auto period = 24. + Noise(i, 61) * 30.;
		const auto phase = std::fmod(t / period + Noise(i, 62), 1.);
		// Up and back down, slowing at the ends like real wax.
		const auto travel = 0.5 - 0.5 * std::cos(phase * 2. * kPi);
		const auto x = w * (0.15 + Noise(i, 63) * 0.7
			+ 0.05 * std::sin(t * 0.2 + i));
		const auto y = h * (1.05 - travel * 1.1);
		const auto r = h * (0.16 + Noise(i, 64) * 0.18);
		SoftBlob(p, { x, y }, r, colors[i % colors.size()], 0.95);
	}
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

} // namespace

const std::vector<AuraPreset> &AuraPresets() {
	static const auto result = std::vector<AuraPreset>{
		{ u"pink"_q, u"🌸 Аура: розовая"_q },
		{ u"mint"_q, u"🍃 Аура: мятная"_q },
		{ u"sunset"_q, u"🌅 Аура: закат"_q },
		{ u"night"_q, u"🌙 Аура: ночная"_q },
		{ u"aurora"_q, u"🌠 Северное сияние"_q },
		{ u"snow"_q, u"❄️ Снегопад"_q },
		{ u"rain"_q, u"🌧 Дождь в городе"_q },
		{ u"fireflies"_q, u"✨ Светлячки"_q },
		{ u"lava"_q, u"🫧 Лава-лампа"_q },
	};
	return result;
}

bool IsAuraPath(const QString &path) {
	return path.startsWith(QString::fromLatin1(kPrefix));
}

QString AuraPath(const QString &id) {
	return QString::fromLatin1(kPrefix) + id;
}

QImage RenderAuraFrame(const QString &path, crl::time ms) {
	const auto id = path.mid(int(qstrlen(kPrefix)));
	const auto blurry = id == u"pink"_q
		|| id == u"mint"_q
		|| id == u"sunset"_q
		|| id == u"night"_q
		|| id == u"lava"_q;
	// Gradients look the same at a small size, particles need more.
	const auto sharp = (id == u"snow"_q || id == u"rain"_q);
	const auto size = blurry
		? QSize(320, 180)
		: sharp
		? QSize(960, 540)
		: QSize(640, 360);
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::black);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	const auto t = ms / 1000.;
	if (id == u"pink"_q) {
		Aura(p, size, t, { QColor(40, 10, 40), {
			QColor(255, 120, 200), QColor(190, 110, 255), QColor(255, 170, 160),
			QColor(140, 90, 255), QColor(255, 90, 150) } });
	} else if (id == u"mint"_q) {
		Aura(p, size, t, { QColor(8, 30, 34), {
			QColor(90, 255, 200), QColor(80, 190, 255), QColor(200, 255, 170),
			QColor(60, 140, 200), QColor(120, 255, 230) } });
	} else if (id == u"sunset"_q) {
		Aura(p, size, t, { QColor(40, 14, 20), {
			QColor(255, 140, 60), QColor(255, 80, 120), QColor(255, 200, 100),
			QColor(180, 70, 200), QColor(255, 110, 80) } });
	} else if (id == u"night"_q) {
		Aura(p, size, t, { QColor(6, 6, 22), {
			QColor(60, 70, 220), QColor(130, 60, 220), QColor(40, 140, 220),
			QColor(90, 40, 160), QColor(60, 100, 255) } });
	} else if (id == u"aurora"_q) {
		Aurora(p, size, t);
	} else if (id == u"snow"_q) {
		Snow(p, size, t);
	} else if (id == u"rain"_q) {
		Rain(p, size, t);
	} else if (id == u"fireflies"_q) {
		Fireflies(p, size, t);
	} else if (id == u"lava"_q) {
		Lava(p, size, t);
	} else {
		return QImage();
	}
	return result;
}

} // namespace Window::Liquid
