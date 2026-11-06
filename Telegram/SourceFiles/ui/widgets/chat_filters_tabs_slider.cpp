/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/widgets/chat_filters_tabs_slider.h"

#include "ui/effects/ripple_animation.h"
#include "ui/effects/liquid_glass.h"
#include "ui/painter.h"
#include "ui/widgets/side_bar_button.h"
#include "styles/style_dialogs.h"
#include "styles/style_widgets.h"

#include <QScrollBar>

namespace Ui {

ChatsFiltersTabs::ChatsFiltersTabs(
	not_null<Ui::RpWidget*> parent,
	const style::SettingsSlider &st)
: Ui::SettingsSlider(parent, st)
, _st(st)
, _unreadSt([&] {
	auto st = Ui::UnreadBadgeStyle();
	st.align = style::al_left;
	return st;
}())
, _unreadMaxString(u"99+"_q)
, _unreadSkip(st::lineWidth * 5)
, _iconSkip(st::dialogsFiltersTabsIconSkip) {
	Expects(_st.barSnapToLabel && _st.strictSkip);
	if (_st.barRadius > 0) {
		_bar.emplace(_st.barRadius, _st.barFg);
		_barActive.emplace(_st.barRadius, _st.barFgActive);
	}
	{
		const auto one = Ui::CountUnreadBadgeSize(u"9"_q, _unreadSt, 1);
		_cachedBadgeWidths = {
			one.width(),
			Ui::CountUnreadBadgeSize(u"99"_q, _unreadSt, 2).width(),
			Ui::CountUnreadBadgeSize(u"999"_q, _unreadSt, 2).width(),
		};
		_cachedBadgeHeight = one.height();
	}
	style::PaletteChanged(
	) | rpl::on_next([=] {
		for (auto &[index, unread] : _unreadCounts) {
			unread.cache = cacheUnreadCount(unread.count, unread.muted);
		}
		update();
	}, lifetime());
	Ui::DiscreteSlider::setSelectOnPress(false);
}

bool ChatsFiltersTabs::setSectionsAndCheckChanged(
		std::vector<TextWithEntities> &&sections,
		const Text::MarkedContext &context,
		Fn<bool()> paused) {
	const auto &was = sectionsRef();
	const auto changed = [&] {
		if (was.size() != sections.size()) {
			return true;
		}
		for (auto i = 0; i < sections.size(); i++) {
			if (was[i].label.toTextWithEntities() != sections[i]) {
				return true;
			}
		}
		return false;
	}();
	if (changed) {
		Ui::DiscreteSlider::setSections(std::move(sections), context);
		updateSectionsContentWidths();
	}
	_emojiPaused = std::move(paused);
	return changed;
}

void ChatsFiltersTabs::setTabsMode(ChatsFiltersTabsMode mode) {
	if (_tabsMode == mode) {
		return;
	}
	_tabsMode = mode;
	updateSectionsContentWidths();
	fitWidthToSections();
	update();
}

void ChatsFiltersTabs::setSectionIcons(
		std::vector<const style::internal::Icon*> icons) {
	if (_sectionIcons == icons) {
		return;
	}
	_sectionIcons = std::move(icons);
	if (_tabsMode != ChatsFiltersTabsMode::TextOnly) {
		updateSectionsContentWidths();
		fitWidthToSections();
		update();
	}
}

auto ChatsFiltersTabs::sectionIcon(int index) const
-> const style::internal::Icon* {
	return (_tabsMode == ChatsFiltersTabsMode::TextOnly
		|| index < 0
		|| index >= int(_sectionIcons.size()))
		? nullptr
		: _sectionIcons[index];
}

int ChatsFiltersTabs::iconExtraWidth(
		const style::internal::Icon *icon) const {
	return !icon
		? 0
		: (_tabsMode == ChatsFiltersTabsMode::TextAndIcons)
		? (icon->width() + _iconSkip)
		: icon->width();
}

int ChatsFiltersTabs::badgeExtraWidth(int index) const {
	const auto it = _unreadCounts.find(index);
	if (it == _unreadCounts.end()) {
		return 0;
	}
	const auto count = int(it->second.count);
	const auto widthIndex = (count < 10) ? 0 : (count < 100) ? 1 : 2;
	return _cachedBadgeWidths[widthIndex] + _unreadSkip;
}

void ChatsFiltersTabs::updateSectionsContentWidths() {
	auto index = 0;
	enumerateSections([&](Section &section) {
		const auto labelWidth = (_tabsMode == ChatsFiltersTabsMode::IconsOnly)
			? 0
			: section.label.maxWidth();
		section.contentWidth = iconExtraWidth(sectionIcon(index))
			+ labelWidth
			+ badgeExtraWidth(index);
		++index;
		return true;
	});
}

void ChatsFiltersTabs::fitWidthToSections() {
	SettingsSlider::fitWidthToSections();

	_lockedFromX = calculateLockedFromX();

	{
		_sections.clear();
		enumerateSections([&](Section &section) {
			_sections.push_back({ not_null{ &section }, 0, false });
			return true;
		});
	}
}

void ChatsFiltersTabs::setUnreadCount(int index, int unreadCount, bool mute) {
	const auto it = _unreadCounts.find(index);
	if (it == _unreadCounts.end()) {
		if (unreadCount) {
			_unreadCounts.emplace(index, Unread{
				.cache = cacheUnreadCount(unreadCount, mute),
				.count = ushort(std::clamp(
					unreadCount,
					0,
					int(std::numeric_limits<ushort>::max()))),
				.muted = mute,
			});
			update();
		}
	} else if (!unreadCount) {
		_unreadCounts.erase(it);
		update();
	} else if (it->second.count != unreadCount || it->second.muted != mute) {
		it->second.count = unreadCount;
		it->second.muted = mute;
		it->second.cache = cacheUnreadCount(unreadCount, mute);
		update();
	}
	updateSectionsContentWidths();
}

int ChatsFiltersTabs::calculateLockedFromX() const {
	if (!_lockedFrom) {
		return std::numeric_limits<int>::max();
	}
	auto left = 0;
	auto index = 0;
	enumerateSections([&](const Section &section) {
		const auto currentRight = section.left + section.width;
		if (index == _lockedFrom) {
			return false;
		}
		left = currentRight;
		index++;
		return true;
	});
	return left ? left : std::numeric_limits<int>::max();
}

void ChatsFiltersTabs::setLockedFrom(int index) {
	_lockedFrom = index;
	_lockedFromX = calculateLockedFromX();
	if (!index) {
		_paletteLifetime.destroy();
		return;
	}
	_paletteLifetime = style::PaletteChanged(
	) | rpl::on_next([this] {
		_lockCache.emplace(Ui::SideBarLockIcon(_st.labelFg));
	});
}

QImage ChatsFiltersTabs::cacheUnreadCount(int count, bool muted) const {
	const auto widthIndex = (count < 10) ? 0 : (count < 100) ? 1 : 2;
	auto image = QImage(
		QSize(_cachedBadgeWidths[widthIndex], _cachedBadgeHeight)
			* style::DevicePixelRatio(),
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(style::DevicePixelRatio());
	image.fill(Qt::transparent);
	const auto string = (count > 999)
		? _unreadMaxString
		: QString::number(count);
	{
		auto p = QPainter(&image);
		if (muted) {
			auto copy = _unreadSt;
			copy.muted = muted;
			Ui::PaintUnreadBadge(p, string, 0, 0, copy, 0);
		} else {
			Ui::PaintUnreadBadge(p, string, 0, 0, _unreadSt, 0);
		}
	}
	return image;
}

QRectF ChatsFiltersTabs::liquidTargetRect(int index) const {
	if (index < 0 || index >= int(_sections.size())) {
		return QRectF();
	}
	const auto &shifted = _sections[index];
	const auto &section = *shifted.section;
	const auto left = section.left
		+ shifted.horizontalShift
		+ (section.width - section.contentWidth) / 2;
	const auto font = _st.labelStyle.font->height;
	const auto padding = QMarginsF(
		font * 0.75,
		font * 0.4,
		font * 0.75,
		font * 0.4);
	return QRectF(
		left,
		_st.labelTop,
		section.contentWidth,
		font).marginsAdded(padding);
}

void ChatsFiltersTabs::paintLiquidSelection(QPainter &p, int activeIndex) {
	const auto target = liquidTargetRect(activeIndex);
	if (target.isEmpty()) {
		return;
	}
	constexpr auto kLeadingDuration = crl::time(220);
	constexpr auto kTrailingDuration = crl::time(420);
	if (activeIndex != _liquidIndex) {
		const auto first = (_liquidIndex < 0) || _liquidShown.isEmpty();
		_liquidIndex = activeIndex;
		if (first || anim::Disabled()) {
			_liquidFrom = target;
			_liquidLeading.stop();
			_liquidTrailing.stop();
		} else {
			_liquidFrom = _liquidShown;
			_liquidRightLeads = (target.center().x() > _liquidFrom.center().x());
			const auto callback = [=] { update(); };
			_liquidLeading.start(callback, 0., 1., kLeadingDuration);
			_liquidTrailing.start(callback, 0., 1., kTrailingDuration);
		}
	}
	const auto easeOut = [](float64 t) {
		const auto u = 1. - t;
		return 1. - u * u * u;
	};
	const auto easeOutBack = [](float64 t) {
		// A little overshoot, the drop wobbles before it settles.
		constexpr auto c1 = 1.25;
		constexpr auto c3 = c1 + 1.;
		const auto u = t - 1.;
		return 1. + c3 * u * u * u + c1 * u * u;
	};
	const auto leading = easeOut(_liquidLeading.value(1.));
	const auto trailing = easeOutBack(_liquidTrailing.value(1.));
	const auto mix = [](float64 from, float64 to, float64 progress) {
		return from + (to - from) * progress;
	};
	auto left = mix(
		_liquidFrom.left(),
		target.left(),
		_liquidRightLeads ? trailing : leading);
	auto right = mix(
		_liquidFrom.right(),
		target.right(),
		_liquidRightLeads ? leading : trailing);
	if (right - left < target.height()) {
		const auto center = (left + right) / 2.;
		left = center - target.height() / 2.;
		right = center + target.height() / 2.;
	}
	// Stretched while flowing, so a bit thinner, like a drop.
	const auto stretch = (right - left) / std::max(target.width(), 1.);
	const auto squeeze = 1. - 0.18 * std::clamp(stretch - 1., 0., 1.);
	const auto height = target.height() * squeeze;
	const auto rect = QRectF(
		left,
		target.center().y() - height / 2.,
		right - left,
		height);
	_liquidShown = rect;

	auto fill = _st.barFgActive->c;
	fill.setAlphaF(0.16);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	const auto radius = rect.height() / 2.;
	p.drawRoundedRect(rect, radius, radius);
	Ui::LiquidGlass::PaintShine(
		p,
		rect,
		radius,
		false,
		(st::windowBg->c.lightnessF() < 0.5));
}

void ChatsFiltersTabs::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	const auto clip = e->rect();
	const auto range = getCurrentActiveRange();
	const auto activeIndex = activeSection();
	const auto now = crl::now();
	const auto liquid = Ui::LiquidGlass::Enabled() && !_reordering;
	if (liquid) {
		paintLiquidSelection(p, activeIndex);
	}

	auto index = 0;
	auto raisedIndex = -1;
	auto activeHorizontalShift = 0;
	const auto drawSection = [&](Section &section) {
		// const auto activeWidth = _st.barSnapToLabel
		// 	? section.contentWidth
		// 	: section.width;

		const auto horizontalShift = _sections[index].horizontalShift;
		const auto shiftedLeft = section.left + horizontalShift;
		if (_sections[index].raise) {
			raisedIndex = index;
		}
		if (index == activeIndex) {
			activeHorizontalShift = horizontalShift;
		}

		// const auto activeLeft = shiftedLeft
		// 	+ (section.width - activeWidth) / 2;
		// const auto active = 1.
		// 	- std::clamp(
		// 		std::abs(range.left - activeLeft) / float64(range.width),
		// 		0.,
		// 		1.);
		const auto active = (index == activeIndex) ? 1. : 0.;
		if (section.ripple) {
			const auto color = anim::color(
				_st.rippleBg,
				_st.rippleBgActive,
				active);
			section.ripple->paint(p, shiftedLeft, 0, width(), &color);
			if (section.ripple->empty()) {
				section.ripple.reset();
			}
		}
		const auto labelLeft = shiftedLeft
			+ (section.width - section.contentWidth) / 2;
		const auto rect = myrtlrect(
			labelLeft,
			_st.labelTop,
			section.contentWidth,
			_st.labelStyle.font->height);
		if (rect.intersects(clip)) {
			const auto locked = (_lockedFrom && (index >= _lockedFrom));
			if (locked) {
				constexpr auto kPremiumLockedOpacity = 0.6;
				p.setOpacity(kPremiumLockedOpacity);
			}
			const auto icon = sectionIcon(index);
			const auto iconExtra = iconExtraWidth(icon);
			const auto labelWidth = (_tabsMode
				== ChatsFiltersTabsMode::IconsOnly)
				? 0
				: section.label.maxWidth();
			p.setPen(anim::pen(_st.labelFg, _st.labelFgActive, active));
			if (icon) {
				icon->paint(
					p,
					labelLeft,
					_st.labelTop
						+ (_st.labelStyle.font->height - icon->height()) / 2,
					width(),
					anim::color(_st.labelFg, _st.labelFgActive, active));
			}
			if (labelWidth > 0) {
				section.label.draw(p, {
					.position = QPoint(labelLeft + iconExtra, _st.labelTop),
					.outerWidth = width(),
					.availableWidth = section.label.maxWidth(),
					.now = now,
					.pausedEmoji = _emojiPaused && _emojiPaused(),
				});
			}
			{
				const auto it = _unreadCounts.find(index);
				if (it != _unreadCounts.end()) {
					p.drawImage(
						labelLeft
							+ iconExtra
							+ labelWidth
							+ _unreadSkip,
						_st.labelTop,
						it->second.cache);
				}
			}
			if (locked) {
				if (!_lockCache) {
					_lockCache.emplace(Ui::SideBarLockIcon(_st.labelFg));
				}
				const auto size = _lockCache->size()
					/ style::DevicePixelRatio();
				p.drawImage(
					labelLeft + (iconExtra + labelWidth - size.width()) / 2,
					height() - size.height() - st::lineWidth,
					*_lockCache);
				p.setOpacity(1.0);
			}
		}
		index++;
		return true;
	};
	enumerateSections(drawSection);
	if (raisedIndex >= 0) {
		index = raisedIndex;
		drawSection(*_sections[raisedIndex].section);
	}
	if (_st.barSnapToLabel && !liquid) {
		const auto drawRect = [&](QRect rect, bool active) {
			const auto &bar = active ? _barActive : _bar;
			if (bar) {
				bar->paint(p, rect);
			} else {
				p.fillRect(rect, active ? _st.barFgActive : _st.barFg);
			}
		};
		const auto add = _st.barStroke / 2;
		const auto from = std::max(range.left - add, 0);
		const auto till = std::min(range.left + range.width + add, width());
		if (from < till) {
			drawRect(
				myrtlrect(
					from,
					_st.barTop,
					till - from,
					_st.barStroke).translated(activeHorizontalShift, 0),
				true);
		}
	}
}

void ChatsFiltersTabs::mousePressEvent(QMouseEvent *e) {
	const auto mouseButton = e->button();
	if (mouseButton == Qt::MouseButton::LeftButton) {
		_lockedPressed = (e->pos().x() >= _lockedFromX);
		if (_lockedPressed) {
			Ui::RpWidget::mousePressEvent(e);
		} else {
			Ui::SettingsSlider::mousePressEvent(e);
		}
	} else {
		Ui::RpWidget::mousePressEvent(e);
	}
}

void ChatsFiltersTabs::mouseMoveEvent(QMouseEvent *e) {
	if (_reordering) {
		Ui::RpWidget::mouseMoveEvent(e);
	} else {
		Ui::SettingsSlider::mouseMoveEvent(e);
	}
}

void ChatsFiltersTabs::mouseReleaseEvent(QMouseEvent *e) {
	const auto mouseButton = e->button();
	if (mouseButton == Qt::MouseButton::LeftButton) {
		if (base::take(_lockedPressed)) {
			_lockedPressed = false;
			_lockedClicked.fire({});
		} else {
			if (_reordering) {
				for (const auto &section : _sections) {
					if (section.section->ripple) {
						section.section->ripple->lastStop();
					}
				}
			} else {
				Ui::SettingsSlider::mouseReleaseEvent(e);
			}
		}
	} else {
		Ui::RpWidget::mouseReleaseEvent(e);
	}
}

void ChatsFiltersTabs::contextMenuEvent(QContextMenuEvent *e) {
	const auto pos = e->pos();
	if (pos.x() >= _lockedFromX) {
		return;
	}
	auto left = 0;
	auto index = 0;
	enumerateSections([&](const Section &section) {
		const auto currentRight = section.left + section.width;
		if (pos.x() > left && pos.x() < currentRight) {
			return false;
		}
		left = currentRight;
		index++;
		return true;
	});
	_contextMenuRequested.fire_copy(index);
}

rpl::producer<int> ChatsFiltersTabs::contextMenuRequested() const {
	return _contextMenuRequested.events();
}

rpl::producer<> ChatsFiltersTabs::lockedClicked() const {
	return _lockedClicked.events();
}

int ChatsFiltersTabs::count() const {
	return _sections.size();
}

void ChatsFiltersTabs::setHorizontalShift(int index, int shift) {
	Expects(index >= 0 && index < _sections.size());

	auto &section = _sections[index];
	if (shift - section.horizontalShift) {
		section.horizontalShift = shift;
		update();
	}
}

void ChatsFiltersTabs::setRaised(int index) {
	_sections[index].raise = true;
	update();
}

void ChatsFiltersTabs::reorderSections(int oldIndex, int newIndex) {
	Expects(oldIndex >= 0 && oldIndex < _sections.size());
	Expects(newIndex >= 0 && newIndex < _sections.size());
	// Expects(!_inResize);
	auto lefts = std::vector<int>();
	enumerateSections([&](Section &section) {
		lefts.emplace_back(section.left);
		return true;
	});
	const auto wasActive = activeSection();

	{
		auto unreadCounts = base::flat_map<Index, Unread>();
		for (auto &[index, unread] : _unreadCounts) {
			unreadCounts.emplace(
				base::reorder_index(index, oldIndex, newIndex),
				std::move(unread));
		}
		_unreadCounts = std::move(unreadCounts);
	}
	if (oldIndex < _sectionIcons.size() && newIndex < _sectionIcons.size()) {
		base::reorder(_sectionIcons, oldIndex, newIndex);
	}

	base::reorder(sectionsRef(), oldIndex, newIndex);
	Ui::DiscreteSlider::setActiveSectionFast(
		base::reorder_index(wasActive, oldIndex, newIndex));
	Ui::DiscreteSlider::stopAnimation();

	{
		_sections.clear();
		auto left = 0;
		enumerateSections([&](Section &section) {
			_sections.push_back({ not_null{ &section }, 0, false });
			section.left = left;
			left += section.width;
			return true;
		});
	}
	update();
}

not_null<Ui::DiscreteSlider::Section*> ChatsFiltersTabs::widgetAt(
		int index) const {
	Expects(index >= 0 && index < count());

	return _sections[index].section;
}

void ChatsFiltersTabs::setReordering(int value) {
	_reordering = value;
}

int ChatsFiltersTabs::reordering() const {
	return _reordering;
}

void ChatsFiltersTabs::stopAnimation() {
	Ui::DiscreteSlider::stopAnimation();
}

} // namespace Ui
