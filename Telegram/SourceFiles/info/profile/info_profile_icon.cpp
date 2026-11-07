/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_icon.h"

#include "ui/effects/liquid_glass.h"
#include "ui/painter.h"

namespace Info {
namespace Profile {
namespace {

constexpr auto kGlassIconPadding = 5;

[[nodiscard]] QColor GlassIconColor(const style::icon *icon) {
	// Liquid glass: each kind of icon gets its own bright tile color.
	static const auto colors = std::array<QColor, 8>{ {
		QColor(0x3D, 0x8B, 0xFD),
		QColor(0x34, 0xC7, 0x59),
		QColor(0xFF, 0x95, 0x00),
		QColor(0xFF, 0x3B, 0x30),
		QColor(0xAF, 0x52, 0xDE),
		QColor(0x30, 0xB0, 0xC7),
		QColor(0xFF, 0x2D, 0x55),
		QColor(0x58, 0x56, 0xD6),
	} };
	const auto hash = std::hash<const void*>()(icon);
	return colors[(hash >> 4) % colors.size()];
}

} // namespace
} // namespace Profile
} // namespace Info

namespace Info {
namespace Profile {

FloatingIcon::FloatingIcon(
	RpWidget *parent,
	const style::icon &icon,
	QPoint position)
: FloatingIcon(parent, icon, position, Tag{}) {
}

FloatingIcon::FloatingIcon(
	RpWidget *parent,
	const style::icon &icon,
	QPoint position,
	const Tag &)
: RpWidget(parent)
, _icon(&icon)
, _point(position) {
	const auto extra = Ui::LiquidGlass::Enabled() ? kGlassIconPadding : 0;
	setGeometry(QRect(
		QPoint(0, 0),
		QSize(
			_point.x() + _icon->width() + extra,
			_point.y() + _icon->height() + extra)));
	setAttribute(Qt::WA_TransparentForMouseEvents);
}

void FloatingIcon::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (!Ui::LiquidGlass::Enabled()) {
		_icon->paint(p, _point, width());
		return;
	}
	const auto tile = QRectF(
		_point.x() - kGlassIconPadding + 1,
		_point.y() - kGlassIconPadding + 1,
		_icon->width() + 2 * kGlassIconPadding - 2,
		_icon->height() + 2 * kGlassIconPadding - 2);
	const auto color = GlassIconColor(_icon);
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = std::min(tile.width(), tile.height()) * 0.28;
		auto fill = QLinearGradient(tile.topLeft(), tile.bottomLeft());
		fill.setColorAt(0., color.lighter(118));
		fill.setColorAt(1., color);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(tile, radius, radius);
		auto gloss = QLinearGradient(tile.topLeft(), tile.bottomLeft());
		gloss.setColorAt(0., QColor(255, 255, 255, 70));
		gloss.setColorAt(0.5, QColor(255, 255, 255, 0));
		p.setBrush(gloss);
		p.drawRoundedRect(tile, radius, radius);
	}
	_icon->paint(p, _point, width(), QColor(255, 255, 255));
}

} // namespace Profile
} // namespace Info
