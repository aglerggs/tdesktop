/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rect_part.h"

namespace Ui::LiquidGlass {

// Global switch, fixed for the whole run: the glass is on if it was
// enabled in the settings at the start and TDESKTOP_NO_LIQUID_GLASS
// environment variable is not set.
[[nodiscard]] bool Enabled();

struct Settings {
	bool enabled = true; // Applied after a restart.
	float64 blur = 1.; // Blur radius multiplier, applied at once.
	float64 tint = 1.; // Tint strength multiplier, applied at once.
	bool intro = true; // Startup splash and welcome screen.
};
inline constexpr auto kMinBlur = 0.25;
inline constexpr auto kMaxBlur = 2.;
inline constexpr auto kMinTint = 0.2;
inline constexpr auto kMaxTint = 1.8;

// Reads the settings file, call once at the start before any widget.
void LoadSettings(const QString &path);
[[nodiscard]] const Settings &CurrentSettings();
void SetSettings(Settings settings);
[[nodiscard]] rpl::producer<> SettingsChanged();

struct Style {
	// Corner radius of the glass shape, logical pixels.
	int radius = 0;

	// Backdrop blur radius, logical pixels.
	int blur = 12;

	// Width of the refracting rim along the free edges, logical pixels.
	int bevel = 14;

	// Rim lensing strength, 0 disables refraction.
	float64 refraction = 1.;

	// Chromatic split on the rim, 0 disables it.
	float64 dispersion = 0.3;

	// Backdrop vibrancy, 1 keeps the colors as is.
	float64 saturation = 1.3;

	QColor tint;
	QColor highlight;
	QColor shade;
	QColor shadow;
	int shadowBlur = 0;
	int shadowShift = 0;

	// Edges that are "free": they refract, shine and get rounded.
	// A full width bar has only one free edge, a floating capsule - all.
	RectParts rim = RectPart::AllSides;
};

// What lies under a glass shape.
struct Backdrop {
	QImage image; // Any device pixel ratio.
	QRect rect; // Logical pixels, in the coordinates of the requester.

	[[nodiscard]] bool empty() const {
		return image.isNull() || rect.isEmpty();
	}
};
using BackdropProvider = Fn<Backdrop(QRect rect)>;

// Glass colors derived from a theme color the glass lies on.
[[nodiscard]] Style MakeStyle(
	const QColor &base,
	int radius,
	RectParts rim = RectPart::AllSides);

class Surface final {
public:
	Surface() = default;
	explicit Surface(Style st);

	void setStyle(Style st);
	[[nodiscard]] const Style &style() const;

	// How much backdrop is needed around the shape on each side.
	[[nodiscard]] int backdropMargin() const;

	// Only the outer shadow, for callers that paint something
	// between the shadow and the glass body.
	void paintShadow(QPainter &p, QRect shape);

	// Glass body: refracted and blurred backdrop + tint + highlights.
	// `backdrop` is what lies under `backdropRect` (painter coordinates,
	// logical pixels), it may have any device pixel ratio.
	void paintBody(
		QPainter &p,
		QRect shape,
		const QImage &backdrop,
		QRect backdropRect);

	// paintShadow() + paintBody().
	void paint(
		QPainter &p,
		QRect shape,
		const QImage &backdrop,
		QRect backdropRect);
	void paint(QPainter &p, QRect shape, const Backdrop &backdrop);

private:
	void validateShadow(QSize size);
	void validateBody(
		QSize shapeSize,
		const QImage &backdrop,
		QPointF shapeInBackdrop);
	void paintOverlay(QPainter &p, QRectF shape) const;

	Style _st;
	QImage _shadow;
	QSize _shadowSize;
	QImage _work;
	QImage _body;

};

// A glossy "liquid" pill without a backdrop, for selections and buttons
// lying on a solid background: the fill color with a shine and a rim.
void PaintPill(
	QPainter &p,
	QRectF rect,
	float64 radius,
	const QColor &fill,
	bool strong);

// Only the shine and the rim of PaintPill, over something already painted.
void PaintShine(
	QPainter &p,
	QRectF rect,
	float64 radius,
	bool strong,
	bool dark);

// Rounded only where both adjacent sides are free.
[[nodiscard]] QPainterPath ShapePath(
	QRectF rect,
	float64 radius,
	RectParts rim);

} // namespace Ui::LiquidGlass
