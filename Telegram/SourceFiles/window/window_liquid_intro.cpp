/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_intro.h"

#include "data/data_session.h"
#include "data/data_peer.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "main/main_session.h"
#include "ui/abstract_button.h"
#include "ui/effects/animations.h"
#include "ui/effects/liquid_glass.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/userpic_view.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"

#include <QtGui/QPainterPath>

namespace Window {
namespace {

constexpr auto kFadeIn = crl::time(450);
constexpr auto kZoomStart = crl::time(500);
constexpr auto kZoomEnd = crl::time(3000);
constexpr auto kFlashPeak = crl::time(3050);
constexpr auto kFlashEnd = crl::time(3700);
constexpr auto kWelcomeStart = kFlashPeak;
constexpr auto kHideDuration = crl::time(420);
constexpr auto kChatsCount = 6;
constexpr auto kUserpicSize = 64;

// Where the two fingertips almost touch, in the first picture.
constexpr auto kFingersX = 0.462;
constexpr auto kFingersY = 0.452;

[[nodiscard]] float64 EaseInOutCubic(float64 t) {
	return (t < 0.5)
		? (4. * t * t * t)
		: (1. - std::pow(-2. * t + 2., 3) / 2.);
}

[[nodiscard]] float64 Spring(float64 t) {
	return (t >= 1.) ? 1. : (1. - std::exp(-6. * t) * std::cos(9. * t));
}

[[nodiscard]] QRectF CoverRect(QSizeF image, QSizeF area) {
	const auto scale = std::max(
		area.width() / image.width(),
		area.height() / image.height());
	const auto size = image * scale;
	return QRectF(
		QPointF(
			(area.width() - size.width()) / 2.,
			(area.height() - size.height()) / 2.),
		size);
}

[[nodiscard]] QFont MakeFont(int size, bool bold) {
	auto result = (bold ? st::semiboldFont : st::normalFont)->f;
	result.setPixelSize(size);
	return result;
}

class ChatButton final : public Ui::AbstractButton {
public:
	ChatButton(QWidget *parent, not_null<History*> history)
	: AbstractButton(parent)
	, _history(history)
	, _name(history->peer->shortName()) {
		resize(96, kUserpicSize + 34);
		setCursor(style::cur_pointer);
	}

	[[nodiscard]] not_null<History*> history() const {
		return _history;
	}

	void setAppear(float64 value) {
		_appear = value;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		if (_appear <= 0.) {
			return;
		}
		auto p = Painter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto t = std::clamp(_appear, 0., 1.);
		const auto spring = Spring(t);
		p.setOpacity(std::min(1., t * 3.));
		const auto center = QPointF(width() / 2., kUserpicSize / 2. + 4.);
		const auto hover = isOver() ? 1.06 : 1.;
		p.translate(center);
		p.scale(spring * hover, spring * hover);
		p.translate(-center);

		const auto left = (width() - kUserpicSize) / 2;
		const auto top = 4;
		_history->peer->paintUserpic(
			p,
			_userpic,
			left,
			top,
			kUserpicSize,
			true);
		const auto ring = QRectF(left, top, kUserpicSize, kUserpicSize)
			.marginsAdded({ 2., 2., 2., 2. });
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(255, 255, 255, isOver() ? 200 : 110), 1.5));
		p.drawEllipse(ring);

		p.setFont(MakeFont(13, false));
		p.setPen(QColor(255, 255, 255, 230));
		const auto text = QFontMetrics(p.font()).elidedText(
			_name,
			Qt::ElideRight,
			width() - 4);
		p.drawText(
			QRect(0, top + kUserpicSize + 8, width(), 20),
			Qt::AlignHCenter | Qt::AlignTop,
			text);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	const not_null<History*> _history;
	const QString _name;
	Ui::PeerUserpicView _userpic;
	float64 _appear = 0.;

};

class EnterButton final : public Ui::AbstractButton {
public:
	EnterButton(QWidget *parent) : AbstractButton(parent) {
		resize(260, 52);
		setCursor(style::cur_pointer);
	}

	void setAppear(float64 value) {
		_appear = value;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		if (_appear <= 0.) {
			return;
		}
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setOpacity(std::min(1., _appear * 2.));
		const auto r = QRectF(rect()).marginsRemoved({ 1., 1., 1., 1. });
		const auto radius = r.height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, isOver() ? 60 : 38));
		p.drawRoundedRect(r, radius, radius);
		auto gloss = QLinearGradient(r.topLeft(), r.bottomLeft());
		gloss.setColorAt(0., QColor(255, 255, 255, 80));
		gloss.setColorAt(0.5, QColor(255, 255, 255, 10));
		gloss.setColorAt(1., QColor(255, 255, 255, 0));
		p.setBrush(gloss);
		p.drawRoundedRect(r, radius, radius);
		auto rim = QLinearGradient(r.topLeft(), r.bottomLeft());
		rim.setColorAt(0., QColor(255, 255, 255, 200));
		rim.setColorAt(1., QColor(255, 255, 255, 60));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QBrush(rim), 1.2));
		p.drawRoundedRect(r, radius, radius);
		p.setFont(MakeFont(16, true));
		p.setPen(QColor(255, 255, 255));
		p.drawText(rect(), Qt::AlignCenter, u"Перейти к чатам"_q);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	float64 _appear = 0.;

};

class Intro final : public Ui::RpWidget {
public:
	Intro(QWidget *parent, not_null<SessionController*> controller);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void tick();
	void layoutWelcome();
	void skipSplash();
	void hideAnimated(Fn<void()> done = nullptr);
	[[nodiscard]] crl::time elapsed() const;
	void paintSplash(QPainter &p, crl::time now);
	void paintWelcome(QPainter &p, crl::time now);

	const not_null<SessionController*> _controller;
	const QImage _creation;
	const QImage _welcome;
	crl::time _started = 0;
	crl::time _skipShift = 0;
	crl::time _hideStarted = 0;
	Fn<void()> _hideDone;
	Ui::Animations::Basic _ticker;
	std::vector<not_null<ChatButton*>> _chats;
	EnterButton *_enter = nullptr;

};

Intro::Intro(QWidget *parent, not_null<SessionController*> controller)
: RpWidget(parent)
, _controller(controller)
, _creation(u":/gui/art/liquid/intro_creation.jpg"_q)
, _welcome(u":/gui/art/liquid/intro_welcome.jpg"_q)
, _ticker([=] { tick(); }) {
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	auto added = 0;
	const auto list = controller->session().data().chatsList()->indexed();
	for (const auto &row : *list) {
		if (added >= kChatsCount) {
			break;
		} else if (const auto history = row->history()) {
			const auto button = Ui::CreateChild<ChatButton>(this, history);
			button->setClickedCallback([=] {
				const auto weak = base::make_weak(_controller);
				hideAnimated([=] {
					if (const auto strong = weak.get()) {
						strong->showPeerHistory(
							history,
							SectionShow::Way::ClearStack);
					}
				});
			});
			_chats.push_back(button);
			++added;
		}
	}
	_enter = Ui::CreateChild<EnterButton>(this);
	_enter->setClickedCallback([=] { hideAnimated(); });

	sizeValue() | rpl::on_next([=] {
		layoutWelcome();
	}, lifetime());

	_started = crl::now();
	_ticker.start();
	setFocus();
}

crl::time Intro::elapsed() const {
	return crl::now() - _started + _skipShift;
}

void Intro::skipSplash() {
	if (elapsed() < kWelcomeStart - 250) {
		_skipShift += (kWelcomeStart - 250) - elapsed();
	}
}

void Intro::hideAnimated(Fn<void()> done) {
	if (_hideStarted) {
		return;
	}
	_hideDone = std::move(done);
	_hideStarted = crl::now();
	for (const auto &button : _chats) {
		button->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	_enter->setAttribute(Qt::WA_TransparentForMouseEvents);
	if (!_ticker.animating()) {
		_ticker.start();
	}
}

void Intro::tick() {
	if (_hideStarted && crl::now() - _hideStarted >= kHideDuration) {
		_ticker.stop();
		const auto done = base::take(_hideDone);
		hide();
		deleteLater();
		if (done) {
			done();
		}
		return;
	}
	const auto now = elapsed();
	const auto welcome = (now - kWelcomeStart) / 1000.;
	const auto index = std::max(0, int(_chats.size()));
	for (auto i = 0; i != index; ++i) {
		const auto t = (welcome - 0.55 - i * 0.07) / 0.6;
		_chats[i]->setAppear(t);
	}
	_enter->setAppear((welcome - 0.45) / 0.5);
	update();
	if (!_hideStarted && now > kFlashEnd + 2600) {
		// The welcome screen settled, no need to animate any more.
		_ticker.stop();
	}
}

void Intro::layoutWelcome() {
	const auto chatsWidth = int(_chats.size()) * 104;
	auto left = (width() - chatsWidth) / 2 + 4;
	const auto chatsTop = height() - 120 - (kUserpicSize + 34);
	for (const auto &button : _chats) {
		button->move(left, chatsTop);
		left += 104;
	}
	_enter->move((width() - _enter->width()) / 2, height() - 92);
}

void Intro::mousePressEvent(QMouseEvent *e) {
	if (elapsed() < kWelcomeStart) {
		skipSplash();
	}
}

void Intro::keyPressEvent(QKeyEvent *e) {
	if (elapsed() < kWelcomeStart) {
		skipSplash();
	} else if (e->key() == Qt::Key_Escape
		|| e->key() == Qt::Key_Return
		|| e->key() == Qt::Key_Enter) {
		hideAnimated();
	}
}

void Intro::paintSplash(QPainter &p, crl::time now) {
	if (_creation.isNull()) {
		return;
	}
	const auto area = QSizeF(size());
	const auto cover = CoverRect(QSizeF(_creation.size()), area);
	const auto zoom = std::clamp(
		(now - kZoomStart) / float64(kZoomEnd - kZoomStart),
		0.,
		1.);
	const auto eased = EaseInOutCubic(zoom);
	const auto scale = 1. + 3.2 * eased;
	const auto fingers = QPointF(
		cover.x() + cover.width() * kFingersX,
		cover.y() + cover.height() * kFingersY);
	const auto screenCenter = QPointF(area.width() / 2., area.height() / 2.);
	// Move the fingers to the screen center while zooming into them.
	const auto focus = fingers + (screenCenter - fingers) * eased;
	p.save();
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.translate(focus);
	p.scale(scale, scale);
	p.translate(-fingers);
	p.setOpacity(std::clamp(now / float64(kFadeIn), 0., 1.));
	p.drawImage(cover, _creation);
	p.restore();

	// Soft vignette that tightens around the fingers.
	auto vignette = QRadialGradient(
		screenCenter,
		std::max(area.width(), area.height()) * (0.75 - 0.25 * eased));
	vignette.setColorAt(0., QColor(0, 0, 0, 0));
	vignette.setColorAt(0.7, QColor(0, 0, 0, int(40 + 60 * eased)));
	vignette.setColorAt(1., QColor(0, 0, 0, int(140 + 80 * eased)));
	p.fillRect(rect(), vignette);
}

void Intro::paintWelcome(QPainter &p, crl::time now) {
	const auto t = (now - kWelcomeStart) / 1000.;
	if (!_welcome.isNull()) {
		const auto area = QSizeF(size());
		const auto cover = CoverRect(QSizeF(_welcome.size()), area);
		// Slow breathing zoom on the portrait.
		const auto scale = 1.08 - 0.06 * std::min(1., t / 6.);
		const auto center = QPointF(area.width() / 2., area.height() * 0.4);
		p.save();
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.translate(center);
		p.scale(scale, scale);
		p.translate(-center);
		p.drawImage(cover, _welcome);
		p.restore();
	}
	auto shade = QLinearGradient(0, 0, 0, height());
	shade.setColorAt(0., QColor(0, 0, 0, 70));
	shade.setColorAt(0.45, QColor(0, 0, 0, 40));
	shade.setColorAt(1., QColor(0, 0, 0, 215));
	p.fillRect(rect(), shade);

	const auto titleAppear = std::clamp((t - 0.2) / 0.6, 0., 1.);
	if (titleAppear > 0.) {
		const auto spring = Spring(titleAppear);
		const auto titleTop = height() - 120 - (kUserpicSize + 34) - 110;
		p.save();
		p.setOpacity(std::min(1., titleAppear * 2.));
		p.translate(0, 24. * (1. - spring));
		p.setFont(MakeFont(40, true));
		p.setPen(QColor(255, 255, 255));
		p.drawText(
			QRect(0, titleTop, width(), 52),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"Добро пожаловать"_q);
		p.setFont(MakeFont(16, false));
		p.setPen(QColor(255, 255, 255, 190));
		p.drawText(
			QRect(0, titleTop + 54, width(), 26),
			Qt::AlignHCenter | Qt::AlignVCenter,
			u"aglergram — выбери, с кого начнём"_q);
		p.restore();
	}
}

void Intro::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto now = elapsed();
	auto opacity = 1.;
	if (_hideStarted) {
		opacity = 1. - std::clamp(
			(crl::now() - _hideStarted) / float64(kHideDuration),
			0.,
			1.);
	}
	p.setOpacity(opacity);
	p.fillRect(rect(), QColor(0, 0, 0));
	if (now < kWelcomeStart) {
		paintSplash(p, now);
	} else {
		paintWelcome(p, now);
	}
	// Golden flash between the two scenes.
	const auto flash = (now < kZoomEnd - 200 || now > kFlashEnd)
		? 0.
		: (now < kFlashPeak)
		? (now - (kZoomEnd - 200)) / float64(kFlashPeak - kZoomEnd + 200)
		: 1. - (now - kFlashPeak) / float64(kFlashEnd - kFlashPeak);
	if (flash > 0.) {
		p.setOpacity(opacity * std::clamp(flash, 0., 1.));
		auto glow = QRadialGradient(
			QPointF(width() / 2., height() / 2.),
			std::max(width(), height()) * 0.7);
		glow.setColorAt(0., QColor(255, 248, 225));
		glow.setColorAt(0.5, QColor(255, 222, 150));
		glow.setColorAt(1., QColor(214, 170, 90));
		p.fillRect(rect(), glow);
	}
}

} // namespace

void ShowLiquidIntro(
		not_null<QWidget*> parent,
		not_null<SessionController*> controller) {
	const auto intro = Ui::CreateChild<Intro>(parent.get(), controller);
	intro->setGeometry(parent->rect());
	if (const auto rp = dynamic_cast<Ui::RpWidget*>(parent.get())) {
		rp->sizeValue() | rpl::on_next([=](QSize size) {
			intro->setGeometry(QRect(QPoint(), size));
		}, intro->lifetime());
	}
	intro->show();
	intro->raise();
}

} // namespace Window
