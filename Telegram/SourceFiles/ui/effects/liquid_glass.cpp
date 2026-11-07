/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/effects/liquid_glass.h"

#include "ui/effects/animation_value.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <cmath>

namespace Ui::LiquidGlass {
namespace {

// The glass body is computed in 1 / kScale of the logical resolution:
// the backdrop is blurred anyway, so it is cheap and looks the same.
constexpr auto kScale = 2;

// Where the rim lensing samples from at the very edge, in bevels.
constexpr auto kRimReach = 0.85;

struct Distance {
	float64 inside = 0.; // Positive inside the shape.
	float64 nx = 0.; // Outward normal.
	float64 ny = 0.;
};

// Signed distance to a rounded rectangle centered at (0, 0).
[[nodiscard]] inline Distance RoundedRectDistance(
		float64 x,
		float64 y,
		float64 halfWidth,
		float64 halfHeight,
		float64 radius) {
	const auto ax = std::abs(x);
	const auto ay = std::abs(y);
	const auto qx = ax - (halfWidth - radius);
	const auto qy = ay - (halfHeight - radius);
	auto result = Distance();
	if (qx > 0. && qy > 0.) {
		const auto length = std::sqrt(qx * qx + qy * qy);
		result.inside = radius - length;
		if (length > 0.) {
			result.nx = qx / length;
			result.ny = qy / length;
		}
	} else if (qx > qy) {
		result.inside = radius - qx;
		result.nx = 1.;
	} else {
		result.inside = radius - qy;
		result.ny = 1.;
	}
	if (x < 0.) {
		result.nx = -result.nx;
	}
	if (y < 0.) {
		result.ny = -result.ny;
	}
	return result;
}

struct Pixel {
	float64 b = 0.;
	float64 g = 0.;
	float64 r = 0.;
	float64 a = 0.;
};

// Bilinear sample with clamp-to-edge from ARGB32_Premultiplied.
[[nodiscard]] inline Pixel Sample(
		const uchar *bits,
		int width,
		int height,
		int stride,
		float64 x,
		float64 y) {
	x = std::clamp(x - 0.5, 0., float64(width - 1));
	y = std::clamp(y - 0.5, 0., float64(height - 1));
	const auto x0 = int(x);
	const auto y0 = int(y);
	const auto x1 = std::min(x0 + 1, width - 1);
	const auto y1 = std::min(y0 + 1, height - 1);
	const auto fx = x - x0;
	const auto fy = y - y0;
	const auto row0 = bits + y0 * stride;
	const auto row1 = bits + y1 * stride;
	const auto p00 = row0 + x0 * 4;
	const auto p01 = row0 + x1 * 4;
	const auto p10 = row1 + x0 * 4;
	const auto p11 = row1 + x1 * 4;
	const auto w00 = (1. - fx) * (1. - fy);
	const auto w01 = fx * (1. - fy);
	const auto w10 = (1. - fx) * fy;
	const auto w11 = fx * fy;
	const auto mix = [&](int i) {
		return p00[i] * w00 + p01[i] * w01 + p10[i] * w10 + p11[i] * w11;
	};
	// Little endian ARGB32 is stored as B, G, R, A.
	return { mix(0), mix(1), mix(2), mix(3) };
}

[[nodiscard]] inline uchar ToByte(float64 value, float64 max) {
	return uchar(std::clamp(value, 0., max) + 0.5);
}

[[nodiscard]] QColor WithAlphaF(QColor color, float64 alpha) {
	color.setAlphaF(std::clamp(alpha, 0., 1.));
	return color;
}

Settings GlobalSettings;
bool EnabledAtStart = true;
QString SettingsPath;
rpl::event_stream<> SettingsUpdates;

[[nodiscard]] Settings Normalized(Settings settings) {
	settings.blur = std::clamp(settings.blur, kMinBlur, kMaxBlur);
	settings.tint = std::clamp(settings.tint, kMinTint, kMaxTint);
	return settings;
}

void SaveSettings() {
	if (SettingsPath.isEmpty()) {
		return;
	}
	auto object = QJsonObject();
	object.insert(u"enabled"_q, GlobalSettings.enabled);
	object.insert(u"blur"_q, GlobalSettings.blur);
	object.insert(u"tint"_q, GlobalSettings.tint);
	object.insert(u"intro"_q, GlobalSettings.intro);
	auto file = QFile(SettingsPath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	}
}

} // namespace

bool Enabled() {
	static const auto disabledByEnvironment = qEnvironmentVariableIsSet(
		"TDESKTOP_NO_LIQUID_GLASS");
	return !disabledByEnvironment && EnabledAtStart;
}

void LoadSettings(const QString &path) {
	SettingsPath = path;
	auto file = QFile(path);
	if (file.open(QIODevice::ReadOnly)) {
		const auto document = QJsonDocument::fromJson(file.readAll());
		const auto object = document.object();
		auto loaded = Settings();
		loaded.enabled = object.value(u"enabled"_q).toBool(true);
		loaded.blur = object.value(u"blur"_q).toDouble(1.);
		loaded.tint = object.value(u"tint"_q).toDouble(1.);
		loaded.intro = object.value(u"intro"_q).toBool(true);
		GlobalSettings = Normalized(loaded);
	}
	EnabledAtStart = GlobalSettings.enabled;
}

const Settings &CurrentSettings() {
	return GlobalSettings;
}

void SetSettings(Settings settings) {
	GlobalSettings = Normalized(settings);
	SaveSettings();
	SettingsUpdates.fire({});
}

rpl::producer<> SettingsChanged() {
	return SettingsUpdates.events();
}

Style MakeStyle(const QColor &base, int radius, RectParts rim) {
	const auto dark = (base.lightnessF() < 0.5);
	auto result = Style();
	result.radius = radius;
	result.rim = rim;
	result.blur = 14;
	result.bevel = 16;
	result.refraction = 1.;
	result.dispersion = 0.35;
	result.saturation = dark ? 1.35 : 1.4;
	// On dark themes a slightly lighter pane with a brighter rim reads as
	// glass even over a plain dark background.
	result.tint = WithAlphaF(
		dark ? base.lighter(130) : base,
		dark ? 0.45 : 0.4);
	result.highlight = dark
		? QColor(255, 255, 255, 110)
		: QColor(255, 255, 255, 210);
	result.shade = dark
		? QColor(0, 0, 0, 56)
		: QColor(0, 0, 0, 16);
	const auto floating = (rim == RectPart::AllSides);
	result.shadow = dark
		? QColor(0, 0, 0, 120)
		: QColor(0, 0, 0, 46);
	result.shadowBlur = floating ? 12 : 0;
	result.shadowShift = floating ? 3 : 0;
	return result;
}

QPainterPath ShapePath(QRectF rect, float64 radius, RectParts rim) {
	auto result = QPainterPath();
	const auto allFree = (rim == RectPart::AllSides);
	if (radius <= 0.) {
		result.addRect(rect);
		return result;
	} else if (allFree) {
		result.addRoundedRect(rect, radius, radius);
		return result;
	}
	// Round only the corners between two free sides.
	const auto has = [&](RectPart part) {
		return (rim & part) != 0;
	};
	const auto round = [&](RectPart a, RectPart b) {
		return (has(a) && has(b)) ? radius : 0.;
	};
	const auto tl = round(RectPart::Top, RectPart::Left);
	const auto tr = round(RectPart::Top, RectPart::Right);
	const auto bl = round(RectPart::Bottom, RectPart::Left);
	const auto br = round(RectPart::Bottom, RectPart::Right);
	const auto l = rect.left();
	const auto t = rect.top();
	const auto r = rect.right();
	const auto b = rect.bottom();
	result.moveTo(l + tl, t);
	result.lineTo(r - tr, t);
	if (tr > 0.) {
		result.arcTo(QRectF(r - 2 * tr, t, 2 * tr, 2 * tr), 90., -90.);
	}
	result.lineTo(r, b - br);
	if (br > 0.) {
		result.arcTo(QRectF(r - 2 * br, b - 2 * br, 2 * br, 2 * br), 0., -90.);
	}
	result.lineTo(l + bl, b);
	if (bl > 0.) {
		result.arcTo(QRectF(l, b - 2 * bl, 2 * bl, 2 * bl), 270., -90.);
	}
	result.lineTo(l, t + tl);
	if (tl > 0.) {
		result.arcTo(QRectF(l, t, 2 * tl, 2 * tl), 180., -90.);
	}
	result.closeSubpath();
	return result;
}

void PaintPill(
		QPainter &p,
		QRectF rect,
		float64 radius,
		const QColor &fill,
		bool strong) {
	if (rect.isEmpty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	radius = std::min(radius, std::min(rect.width(), rect.height()) / 2.);
	const auto dark = (fill.lightnessF() < 0.5);

	p.setPen(Qt::NoPen);
	if (strong) {
		// Soft contact shadow below.
		auto shadow = fill.darker(dark ? 160 : 135);
		shadow.setAlphaF(0.35);
		p.setBrush(shadow);
		p.drawRoundedRect(rect.translated(0., 1.5), radius, radius);
	}
	p.setBrush(fill);
	p.drawRoundedRect(rect, radius, radius);
	PaintShine(p, rect, radius, strong, dark);
}

void PaintShine(
		QPainter &p,
		QRectF rect,
		float64 radius,
		bool strong,
		bool dark) {
	if (rect.isEmpty()) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	radius = std::min(radius, std::min(rect.width(), rect.height()) / 2.);

	auto path = QPainterPath();
	path.addRoundedRect(rect, radius, radius);

	// Shine in the upper half.
	const auto white = QColor(255, 255, 255);
	const auto soft = dark ? 0.3 : 1.; // Subtle on dark backgrounds.
	auto shine = QLinearGradient(
		rect.topLeft(),
		QPointF(rect.x(), rect.y() + rect.height() * 0.6));
	shine.setColorAt(0., WithAlphaF(white, strong ? 0.26 : 0.34 * soft));
	shine.setColorAt(1., WithAlphaF(white, 0.));
	p.fillPath(path, shine);

	// Inner glow at the bottom, like light passing through.
	auto glow = QLinearGradient(
		QPointF(rect.x(), rect.y() + rect.height() * 0.65),
		rect.bottomLeft());
	glow.setColorAt(0., WithAlphaF(white, 0.));
	glow.setColorAt(1., WithAlphaF(white, strong ? 0.12 : 0.08 * soft));
	p.fillPath(path, glow);

	// Specular rim.
	const auto line = 1.;
	const auto inner = rect.marginsRemoved(
		QMarginsF(line / 2., line / 2., line / 2., line / 2.));
	auto rim = QLinearGradient(inner.topLeft(), inner.bottomLeft());
	rim.setColorAt(0., WithAlphaF(white, strong ? 0.55 : 0.75 * soft));
	rim.setColorAt(0.5, WithAlphaF(white, strong ? 0.08 : 0.15 * soft));
	rim.setColorAt(1., WithAlphaF(white, strong ? 0.3 : 0.4 * soft));
	p.setPen(QPen(QBrush(rim), line));
	p.setBrush(Qt::NoBrush);
	const auto innerRadius = std::max(radius - line / 2., 0.);
	p.drawRoundedRect(inner, innerRadius, innerRadius);
	p.setPen(Qt::NoPen);
}

Surface::Surface(Style st) : _st(std::move(st)) {
}

void Surface::setStyle(Style st) {
	_st = std::move(st);
	_shadowSize = QSize();
}

const Style &Surface::style() const {
	return _st;
}

int Surface::backdropMargin() const {
	// Blur needs its radius around, the rim lensing - its reach.
	const auto blur = int(std::ceil(_st.blur * GlobalSettings.blur));
	return blur + int(std::ceil(_st.bevel * kRimReach)) + kScale;
}

void Surface::paint(
		QPainter &p,
		QRect shape,
		const QImage &backdrop,
		QRect backdropRect) {
	paintShadow(p, shape);
	paintBody(p, shape, backdrop, backdropRect);
}

void Surface::paint(QPainter &p, QRect shape, const Backdrop &backdrop) {
	paint(p, shape, backdrop.image, backdrop.rect);
}

void Surface::paintShadow(QPainter &p, QRect shape) {
	if (shape.isEmpty()
		|| _st.shadowBlur <= 0
		|| _st.shadow.alpha() <= 0) {
		return;
	}
	validateShadow(shape.size());
	const auto margin = _st.shadowBlur * 2;
	p.drawImage(
		shape.x() - margin,
		shape.y() - margin + _st.shadowShift,
		_shadow);
}

void Surface::validateShadow(QSize size) {
	if (_shadowSize == size && !_shadow.isNull()) {
		return;
	}
	_shadowSize = size;
	const auto ratio = style::DevicePixelRatio();
	const auto margin = _st.shadowBlur * 2;
	const auto full = size + QSize(margin * 2, margin * 2);
	auto image = QImage(full * ratio, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(ratio);
	{
		auto q = QPainter(&image);
		auto hq = PainterHighQualityEnabler(q);
		q.setPen(Qt::NoPen);
		q.setBrush(_st.shadow);
		q.drawPath(ShapePath(
			QRectF(QPointF(margin, margin), QSizeF(size)),
			_st.radius,
			_st.rim));
	}
	_shadow = Images::BlurLargeImage(
		std::move(image),
		std::max(_st.shadowBlur * ratio / 2, 1));
	_shadow.setDevicePixelRatio(ratio);
}

void Surface::paintBody(
		QPainter &p,
		QRect shape,
		const QImage &backdrop,
		QRect backdropRect) {
	if (shape.isEmpty()) {
		return;
	} else if (backdrop.isNull() || backdropRect.isEmpty()) {
		const auto path = ShapePath(QRectF(shape), _st.radius, _st.rim);
		auto hq = PainterHighQualityEnabler(p);
		p.fillPath(path, WithAlphaF(_st.tint, 1.));
		paintOverlay(p, QRectF(shape));
		return;
	}

	// Bring the backdrop to the work resolution.
	const auto workSize = QSize(
		std::max((backdropRect.width() + kScale - 1) / kScale, 1),
		std::max((backdropRect.height() + kScale - 1) / kScale, 1));
	auto work = backdrop.scaled(
		workSize,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	if (work.format() != QImage::Format_ARGB32_Premultiplied) {
		work = std::move(work).convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}
	work.setDevicePixelRatio(1.);
	const auto radius = std::max(
		int(base::SafeRound(_st.blur * GlobalSettings.blur)) / kScale,
		1);
	if (work.width() > radius && work.height() > radius) {
		work = Images::BlurLargeImage(std::move(work), radius);
	}
	_work = std::move(work);

	const auto inBackdrop = QPointF(shape.topLeft() - backdropRect.topLeft())
		/ float64(kScale);
	validateBody(shape.size(), _work, inBackdrop);

	{
		auto hq = PainterHighQualityEnabler(p);
		p.drawImage(QRectF(shape), _body);
	}
	paintOverlay(p, QRectF(shape));
}

void Surface::validateBody(
		QSize shapeSize,
		const QImage &blurred,
		QPointF shapeInBackdrop) {
	const auto bodySize = QSize(
		std::max((shapeSize.width() + kScale - 1) / kScale, 1),
		std::max((shapeSize.height() + kScale - 1) / kScale, 1));
	if (_body.size() != bodySize
		|| _body.format() != QImage::Format_ARGB32_Premultiplied) {
		_body = QImage(bodySize, QImage::Format_ARGB32_Premultiplied);
	}

	// Geometry in logical pixels, center of the shape at (0, 0).
	// Sides that are not free are pushed far away, so they neither
	// refract nor get rounded.
	const auto far = float64(_st.bevel * 4 + _st.radius + 1);
	const auto has = [&](RectPart part) {
		return (_st.rim & part) != 0;
	};
	const auto extendLeft = has(RectPart::Left) ? 0. : far;
	const auto extendRight = has(RectPart::Right) ? 0. : far;
	const auto extendTop = has(RectPart::Top) ? 0. : far;
	const auto extendBottom = has(RectPart::Bottom) ? 0. : far;
	const auto logicalWidth = float64(shapeSize.width());
	const auto logicalHeight = float64(shapeSize.height());
	const auto halfWidth = (logicalWidth + extendLeft + extendRight) / 2.;
	const auto halfHeight = (logicalHeight + extendTop + extendBottom) / 2.;
	const auto centerX = (logicalWidth + extendRight - extendLeft) / 2.;
	const auto centerY = (logicalHeight + extendBottom - extendTop) / 2.;
	const auto cornerRadius = std::clamp(
		float64(_st.radius),
		0.,
		std::min(halfWidth, halfHeight));
	const auto bevel = std::max(float64(_st.bevel), 1.);
	const auto reach = bevel * kRimReach * _st.refraction;
	const auto dispersion = _st.dispersion;
	const auto saturation = _st.saturation;
	const auto lensing = (reach > 0.);

	const auto src = blurred.constBits();
	const auto srcWidth = blurred.width();
	const auto srcHeight = blurred.height();
	const auto srcStride = blurred.bytesPerLine();
	const auto dstStride = _body.bytesPerLine();
	const auto dst = _body.bits();
	const auto scale = float64(kScale);

	for (auto y = 0; y != bodySize.height(); ++y) {
		auto out = dst + y * dstStride;
		const auto ly = (y + 0.5) * scale;
		for (auto x = 0; x != bodySize.width(); ++x, out += 4) {
			const auto lx = (x + 0.5) * scale;
			const auto distance = RoundedRectDistance(
				lx - centerX,
				ly - centerY,
				halfWidth,
				halfHeight,
				cornerRadius);

			// Antialiased coverage, in work pixels.
			const auto coverage = std::clamp(
				distance.inside / scale + 0.5,
				0.,
				1.);
			if (coverage <= 0.) {
				out[0] = out[1] = out[2] = out[3] = 0;
				continue;
			}

			// Work space position of this pixel in the backdrop.
			const auto sx = shapeInBackdrop.x() + x + 0.5;
			const auto sy = shapeInBackdrop.y() + y + 0.5;

			auto color = Pixel();
			const auto rim = lensing && (distance.inside < bevel);
			if (!rim) {
				color = Sample(src, srcWidth, srcHeight, srcStride, sx, sy);
			} else {
				// Convex rim: the content from outside of the shape is
				// pulled in and squeezed, like through the edge of a lens.
				const auto t = 1. - std::max(distance.inside, 0.) / bevel;
				const auto shift = reach * t * t / scale;
				const auto dx = distance.nx * shift;
				const auto dy = distance.ny * shift;
				const auto green = Sample(
					src,
					srcWidth,
					srcHeight,
					srcStride,
					sx + dx,
					sy + dy);
				if (dispersion > 0.) {
					const auto red = Sample(
						src,
						srcWidth,
						srcHeight,
						srcStride,
						sx + dx * (1. + dispersion),
						sy + dy * (1. + dispersion));
					const auto blue = Sample(
						src,
						srcWidth,
						srcHeight,
						srcStride,
						sx + dx * (1. - dispersion),
						sy + dy * (1. - dispersion));
					color = { blue.b, green.g, red.r, green.a };
				} else {
					color = green;
				}
			}

			if (saturation != 1.) {
				const auto gray = color.r * 0.299
					+ color.g * 0.587
					+ color.b * 0.114;
				color.r = gray + (color.r - gray) * saturation;
				color.g = gray + (color.g - gray) * saturation;
				color.b = gray + (color.b - gray) * saturation;
			}
			const auto alpha = color.a * coverage;
			out[3] = ToByte(alpha, 255.);
			out[2] = ToByte(color.r * coverage, out[3]);
			out[1] = ToByte(color.g * coverage, out[3]);
			out[0] = ToByte(color.b * coverage, out[3]);
		}
	}
}

void Surface::paintOverlay(QPainter &p, QRectF shape) const {
	auto hq = PainterHighQualityEnabler(p);
	const auto path = ShapePath(shape, _st.radius, _st.rim);
	const auto floating = (_st.rim == RectPart::AllSides);
	const auto transparent = WithAlphaF(_st.highlight, 0.);

	p.fillPath(
		path,
		WithAlphaF(_st.tint, _st.tint.alphaF() * GlobalSettings.tint));

	// Soft shine in the upper half.
	if (_st.highlight.alpha() > 0 && (floating || (_st.rim & RectPart::Top))) {
		auto gradient = QLinearGradient(
			shape.topLeft(),
			QPointF(shape.x(), shape.y() + shape.height() * 0.55));
		gradient.setColorAt(
			0.,
			WithAlphaF(_st.highlight, _st.highlight.alphaF() * 0.42));
		gradient.setColorAt(1., transparent);
		p.fillPath(path, gradient);
	}

	// Inner shade at the bottom.
	if (_st.shade.alpha() > 0) {
		const auto transparentShade = WithAlphaF(_st.shade, 0.);
		auto gradient = QLinearGradient(
			QPointF(shape.x(), shape.y() + shape.height() * 0.45),
			shape.bottomLeft());
		gradient.setColorAt(0., transparentShade);
		gradient.setColorAt(1., _st.shade);
		p.fillPath(path, gradient);
	}

	if (_st.highlight.alpha() <= 0) {
		return;
	}
	const auto line = 1.;
	if (floating) {
		// Specular rim: bright on top, a caustic glow at the bottom.
		const auto inner = shape.marginsRemoved(
			QMarginsF(line / 2., line / 2., line / 2., line / 2.));
		const auto innerRadius = std::max(_st.radius - line / 2., 0.);
		auto gradient = QLinearGradient(inner.topLeft(), inner.bottomLeft());
		gradient.setColorAt(0., _st.highlight);
		gradient.setColorAt(
			0.45,
			WithAlphaF(_st.highlight, _st.highlight.alphaF() * 0.18));
		gradient.setColorAt(
			1.,
			WithAlphaF(_st.highlight, _st.highlight.alphaF() * 0.6));
		auto pen = QPen(QBrush(gradient), line);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(ShapePath(inner, innerRadius, _st.rim));
		p.setPen(Qt::NoPen);
		return;
	}

	// Bars: a hairline along every free side.
	const auto hairline = WithAlphaF(
		_st.highlight,
		_st.highlight.alphaF() * 0.75);
	const auto shadeLine = WithAlphaF(
		_st.shade,
		std::min(_st.shade.alphaF() * 2., 1.));
	if (_st.rim & RectPart::Bottom) {
		p.fillRect(
			QRectF(shape.x(), shape.bottom() - line, shape.width(), line),
			shadeLine);
		p.fillRect(
			QRectF(
				shape.x(),
				shape.bottom() - 2 * line,
				shape.width(),
				line),
			hairline);
	}
	if (_st.rim & RectPart::Top) {
		p.fillRect(
			QRectF(shape.x(), shape.y(), shape.width(), line),
			hairline);
	}
	if (_st.rim & RectPart::Left) {
		p.fillRect(
			QRectF(shape.x(), shape.y(), line, shape.height()),
			hairline);
	}
	if (_st.rim & RectPart::Right) {
		p.fillRect(
			QRectF(shape.right() - line, shape.y(), line, shape.height()),
			hairline);
	}
}

} // namespace Ui::LiquidGlass
