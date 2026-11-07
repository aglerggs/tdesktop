/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/profile/info_profile_icon.h"

#include "ui/effects/liquid_glass.h"
#include "ui/painter.h"
#include "styles/style_info.h"

namespace Info {
namespace Profile {
namespace {

constexpr auto kGlassIconPadding = 5;

[[nodiscard]] QColor GlassIconColor(const style::icon *icon) {
	// Liquid glass: calm tinted tiles, the color follows the meaning.
	const auto is = [&](const style::icon &other) {
		return (icon == &other);
	};
	if (is(st::infoIconMediaPhoto) || is(st::infoIconMediaStories)) {
		return QColor(0x4C, 0x9A, 0xFF);
	} else if (is(st::infoIconMediaVideo) || is(st::infoIconMediaGif)) {
		return QColor(0x8E, 0x7C, 0xFF);
	} else if (is(st::infoIconMediaFile) || is(st::infoIconMediaSaved)) {
		return QColor(0xFF, 0xA9, 0x3D);
	} else if (is(st::infoIconMediaAudio) || is(st::infoIconMediaVoice)) {
		return QColor(0x3D, 0xD6, 0x8C);
	} else if (is(st::infoIconMediaLink)) {
		return QColor(0x3D, 0xC8, 0xE0);
	} else if (is(st::infoIconMediaGifts)) {
		return QColor(0xFF, 0x6B, 0x9A);
	} else if (is(st::infoIconDeleteRed) || is(st::infoIconBlock)) {
		return st::attentionButtonFg->c;
	}
	return st::windowActiveTextFg->c;
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
		const auto radius = std::min(tile.width(), tile.height()) * 0.3;
		auto fill = color;
		fill.setAlphaF(st::windowBg->c.lightnessF() < 0.5 ? 0.17 : 0.13);
		p.setPen(Qt::NoPen);
		p.setBrush(fill);
		p.drawRoundedRect(tile, radius, radius);
	}
	_icon->paint(p, _point, width(), color);
}

} // namespace Profile
} // namespace Info
