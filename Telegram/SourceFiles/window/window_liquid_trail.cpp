/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_liquid_trail.h"

#include "base/random.h"
#include "data/data_liquid_store.h"
#include "lang/lang_keys.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "mainwindow.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QPointer>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>

namespace Window::Liquid {
namespace {

constexpr auto kTrailKey = "cursorTrail";
constexpr auto kLifetime = crl::time(380);
constexpr auto kSparkLifetime = crl::time(650);
constexpr auto kMaxPoints = 64;
constexpr auto kMaxSparks = 90;


rpl::variable<TrailStyle> &StyleVariable() {
	static auto result = rpl::variable<TrailStyle>([] {
		const auto value = Data::LiquidStore::Value(
			QString::fromLatin1(kTrailKey)).toInt();
		return (value > 0 && value <= int(TrailStyle::Sparks))
			? TrailStyle(value)
			: TrailStyle::None;
	}());
	return result;
}

class TrailOverlay final : public Ui::RpWidget {
public:
	explicit TrailOverlay(not_null<QWidget*> window);

protected:
	bool eventFilter(QObject *object, QEvent *event) override;
	void paintEvent(QPaintEvent *e) override;

private:
	struct Point {
		QPointF position;
		crl::time when = 0;
	};
	struct Spark {
		QPointF position;
		QPointF velocity;
		crl::time when = 0;
		float64 hue = 0.;
		float64 size = 1.;
	};

	void addPoint(QPoint global);
	void step(crl::time now);
	void updateDirty();
	[[nodiscard]] QRect currentBounds() const;

	const not_null<QWidget*> _window;
	std::vector<Point> _points;
	std::vector<Spark> _sparks;
	Ui::Animations::Basic _animation;
	QRect _painted;
	float64 _hue = 0.;

};

TrailOverlay::TrailOverlay(not_null<QWidget*> window)
: RpWidget(window)
, _window(window)
, _animation([=](crl::time now) { step(now); }) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_NoSystemBackground);
	setGeometry(QRect(QPoint(), window->size()));
	window->installEventFilter(this);
	qApp->installEventFilter(this);
	StyleVariable().value() | rpl::on_next([=](TrailStyle style) {
		setVisible(style != TrailStyle::None);
		_points.clear();
		_sparks.clear();
		update();
	}, lifetime());
}

bool TrailOverlay::eventFilter(QObject *object, QEvent *event) {
	const auto type = event->type();
	if (object == _window.get()) {
		if (type == QEvent::Resize) {
			setGeometry(QRect(QPoint(), _window->size()));
		} else if (type == QEvent::ChildAdded
			|| type == QEvent::ChildPolished) {
			// Stay above everything the window shows.
			crl::on_main(this, [=] { raise(); });
		}
	} else if (type == QEvent::MouseMove && isVisible()) {
		const auto widget = qobject_cast<QWidget*>(object);
		if (widget && widget->window() == _window->window()) {
			addPoint(QCursor::pos());
		}
	}
	return false;
}

void TrailOverlay::addPoint(QPoint global) {
	const auto position = QPointF(mapFromGlobal(global));
	const auto now = crl::now();
	if (!_points.empty()
		&& QLineF(_points.back().position, position).length() < 1.5) {
		return;
	}
	_points.push_back({ position, now });
	if (_points.size() > kMaxPoints) {
		_points.erase(_points.begin());
	}
	_hue = std::fmod(_hue + 0.012, 1.);
	if (CurrentTrailStyle() == TrailStyle::Sparks) {
		const auto count = 2;
		for (auto i = 0; i != count && _sparks.size() < kMaxSparks; ++i) {
			const auto angle = base::RandomIndex(360) * 3.14159265358979 / 180.;
			const auto speed = 0.02 + base::RandomIndex(60) / 1000.;
			_sparks.push_back({
				position,
				QPointF(std::cos(angle) * speed, std::sin(angle) * speed + 0.01),
				now,
				std::fmod(_hue + base::RandomIndex(20) / 100., 1.),
				1.5 + base::RandomIndex(25) / 10.,
			});
		}
	}
	raise();
	if (!_animation.animating()) {
		_animation.start();
	}
	updateDirty();
}

void TrailOverlay::step(crl::time now) {
	while (!_points.empty() && now - _points.front().when > kLifetime) {
		_points.erase(_points.begin());
	}
	_sparks.erase(ranges::remove_if(_sparks, [&](const Spark &spark) {
		return now - spark.when > kSparkLifetime;
	}), _sparks.end());
	if (_points.empty() && _sparks.empty()) {
		_animation.stop();
	}
	updateDirty();
}

QRect TrailOverlay::currentBounds() const {
	auto result = QRectF();
	const auto now = crl::now();
	for (const auto &point : _points) {
		result |= QRectF(point.position, QSizeF(1, 1));
	}
	for (const auto &spark : _sparks) {
		const auto age = float64(now - spark.when);
		result |= QRectF(spark.position + spark.velocity * age, QSizeF(1, 1));
	}
	const auto margin = 24;
	return result.isNull()
		? QRect()
		: result.toAlignedRect().marginsAdded({ margin, margin, margin, margin });
}

void TrailOverlay::updateDirty() {
	const auto bounds = currentBounds();
	update(_painted.united(bounds));
	_painted = bounds;
}

void TrailOverlay::paintEvent(QPaintEvent *e) {
	if (_points.empty() && _sparks.empty()) {
		return;
	}
	auto p = QPainter(this);
	PainterHighQualityEnabler hq(p);
	const auto now = crl::now();
	const auto style = CurrentTrailStyle();
	const auto count = int(_points.size());
	const auto accent = st::windowActiveTextFg->c;
	for (auto i = 1; i < count; ++i) {
		const auto &from = _points[i - 1];
		const auto &to = _points[i];
		const auto life = 1. - std::clamp(
			float64(now - to.when) / kLifetime,
			0.,
			1.);
		const auto order = float64(i) / count;
		const auto fade = life * order;
		if (fade <= 0.) {
			continue;
		}
		auto color = QColor();
		if (style == TrailStyle::Rainbow) {
			color = QColor::fromHsvF(
				std::fmod(_hue - (1. - order) * 0.35 + 1., 1.),
				0.75,
				1.);
		} else {
			color = accent;
		}
		const auto width = ((style == TrailStyle::Comet) ? 9. : 6.) * fade + 1.;
		// Soft glow first, then the bright core.
		auto glow = color;
		glow.setAlphaF(0.22 * fade);
		auto pen = QPen(glow, width * 2.6, Qt::SolidLine, Qt::RoundCap);
		p.setPen(pen);
		p.drawLine(from.position, to.position);
		auto core = (style == TrailStyle::Comet)
			? QColor::fromRgbF(1., 1., 1., 0.85 * fade)
			: color;
		core.setAlphaF(std::min(float64(core.alphaF()), 0.9 * fade));
		pen = QPen(core, width, Qt::SolidLine, Qt::RoundCap);
		p.setPen(pen);
		p.drawLine(from.position, to.position);
	}
	p.setPen(Qt::NoPen);
	for (const auto &spark : _sparks) {
		const auto age = float64(now - spark.when);
		const auto life = 1. - std::clamp(age / kSparkLifetime, 0., 1.);
		const auto position = spark.position + spark.velocity * age;
		auto color = QColor::fromHsvF(spark.hue, 0.6, 1.);
		color.setAlphaF(life);
		p.setBrush(color);
		const auto r = spark.size * (0.4 + 0.6 * life);
		p.drawEllipse(position, r, r);
	}
}

} // namespace

TrailStyle CurrentTrailStyle() {
	return StyleVariable().current();
}

void SetTrailStyle(TrailStyle style) {
	StyleVariable() = style;
	Data::LiquidStore::SetValue(
		QString::fromLatin1(kTrailKey),
		(style == TrailStyle::None) ? QJsonValue() : QJsonValue(int(style)));
}

QString TrailStyleTitle(TrailStyle style) {
	switch (style) {
	case TrailStyle::Rainbow: return u"🌈 Радужный неон"_q;
	case TrailStyle::Comet: return u"☄️ Комета"_q;
	case TrailStyle::Sparks: return u"✨ Искры"_q;
	case TrailStyle::None: break;
	}
	return u"Выключен"_q;
}

void StartCursorTrail(not_null<SessionController*> controller) {
	const auto window = controller->widget();
	if (window->property("aglergramTrail").toBool()) {
		return;
	}
	window->setProperty("aglergramTrail", true);
	const auto overlay = Ui::CreateChild<TrailOverlay>(window.get());
	overlay->raise();
}

void ShowCursorTrailBox(not_null<SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"След за курсором"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Светящийся хвост за мышкой внутри окна aglergram. Клики не "
			"перехватывает."_q,
			st::boxDividerLabel));
		const auto current = CurrentTrailStyle();
		for (const auto variant : {
				TrailStyle::Rainbow,
				TrailStyle::Comet,
				TrailStyle::Sparks,
				TrailStyle::None }) {
			const auto button = box->addRow(
				object_ptr<Ui::SettingsButton>(
					box,
					rpl::single(((variant == current) ? u"✓ "_q : QString())
						+ TrailStyleTitle(variant)),
					st::settingsButtonNoIcon),
				style::margins());
			button->setClickedCallback([=] {
				SetTrailStyle(variant);
				box->closeBox();
			});
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Window::Liquid
