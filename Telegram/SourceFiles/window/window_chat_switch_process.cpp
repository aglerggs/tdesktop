/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_chat_switch_process.h"

#include "core/application.h"
#include "core/shortcuts.h"
#include "data/components/recent_peers.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_saved_sublist.h"
#include "data/data_thread.h"
#include "info/profile/info_profile_cover.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "ui/controls/userpic_button.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/rp_widget.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_userpic_button.h"
#include "styles/style_window.h"

namespace Window {
namespace {

class Button final : public Ui::AbstractButton {
public:
	Button(
		QWidget *parent,
		not_null<Data::Thread*> thread,
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics);

	[[nodiscard]] rpl::producer<> selectRequests() const;

	void setSelected(
		bool selected,
		anim::type animated = anim::type::normal);

private:
	void setup(
		not_null<Data::Thread*> thread,
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics);

	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;

	rpl::event_stream<> _selectRequests;
	Ui::Animations::Simple _overAnimation;
	bool _selected = false;

};

void CloseInWindows(not_null<Data::Thread*> thread) {
	using WindowPointer = base::weak_ptr<Window::SessionController>;
	auto closing = std::vector<WindowPointer>();
	auto clearing = std::vector<WindowPointer>();
	for (const auto &window : thread->session().windows()) {
		if (window->windowId().chat() == thread) {
			closing.push_back(base::make_weak(window));
		} else if (window->activeChatCurrent().thread() == thread) {
			clearing.push_back(base::make_weak(window));
		}
	}
	for (const auto &window : closing) {
		if (const auto strong = window.get()) {
			Core::App().closeWindow(&strong->window());
		}
	}
	for (const auto &window : clearing) {
		if (const auto strong = window.get()) {
			strong->clearSectionStack();
		}
	}
}

Button::Button(
	QWidget *parent,
	not_null<Data::Thread*> thread,
	base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics)
: AbstractButton(parent) {
	setup(thread, userpics);
}

void Button::setup(
		not_null<Data::Thread*> thread,
		base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> &userpics) {
	resize(st::chatSwitchSize);

	auto userpicSt = &st::chatSwitchUserpic;
	const auto userpicSize = userpicSt->size;
	if (const auto topic = thread->asTopic()) {
		using namespace Info::Profile;
		const auto userpic = Ui::CreateChild<TopicIconButton>(
			this,
			topic,
			[] { return true; }); // paused
		userpic->show();
		userpic->move(
			((width() - userpic->width()) / 2),
			st::chatSwitchUserpicTop);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);

		userpicSt = &st::chatSwitchUserpicSmall;
	} else if (const auto sublist = thread->asSublist()) {
		const auto sublistPeer = sublist->sublistPeer();
		const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
			this,
			sublistPeer,
			st::chatSwitchUserpicSublist);
		userpic->showMyNotesOnSelf(true);
		userpic->show();
		userpic->move(
			((width() - userpicSize.width()) / 2),
			st::chatSwitchUserpicTop);
		userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		userpics.emplace(sublistPeer, sublistPeer->createUserpicView());

		userpicSt = &st::chatSwitchUserpicSmall;
	}
	const auto peer = thread->peer();
	const auto userpic = Ui::CreateChild<Ui::UserpicButton>(
		this,
		peer,
		*userpicSt);
	userpic->showSavedMessagesOnSelf(true);
	userpic->show();
	userpic->move(
		(((width() - userpicSize.width()) / 2)
			+ (userpicSize.width() - userpicSt->size.width())),
		(st::chatSwitchUserpicTop
			+ (userpicSize.height() - userpicSt->size.height())));
	userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
	userpics.emplace(peer, peer->createUserpicView());

	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		this,
		((thread->asHistory() && peer->isSelf())
			? tr::lng_saved_messages(tr::now)
			: thread->chatListName()),
		st::chatSwitchNameLabel);
	label->setBreakEverywhere(true);
	label->show();
	label->resizeToNaturalWidth(
		width() - 2 * st::chatSwitchNameSkip);
	label->move(
		(width() - label->width()) / 2,
		(height() + userpic->y() + userpic->height() - label->height()) / 2);

	show();
}

rpl::producer<> Button::selectRequests() const {
	return _selectRequests.events();
}

void Button::setSelected(bool selected, anim::type animated) {
	if (_selected == selected) {
		if (animated == anim::type::instant) {
			_overAnimation.stop();
		}
		return;
	}
	_selected = selected;
	if (animated == anim::type::instant) {
		_overAnimation.stop();
		update();
	} else {
		_overAnimation.start(
			[=] { update(); },
			selected ? 0. : 1.,
			selected ? 1. : 0.,
			st::slideWrapDuration);
	}
}

void Button::paintEvent(QPaintEvent *e) {
	if (Ui::LiquidGlass::Enabled()) {
		// The sliding glass pill is painted by the panel below.
		return;
	}
	const auto selected = _selected ? 1. : 0.;
	const auto selection = _overAnimation.value(selected);
	if (selection <= 0.) {
		return;
	}

	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = st::boxRadius;
	const auto line = st::chatSwitchSelectLine;
	auto pen = st::defaultRoundCheckbox.bgActive->p;
	pen.setWidthF(line * selection);
	p.setPen(pen);
	const auto r = QRectF(rect()).marginsRemoved(
		{ line / 2., line / 2., line / 2., line / 2. });
	p.drawRoundedRect(r, radius, radius);
}

void Button::mouseMoveEvent(QMouseEvent *e) {
	if (!_selected) {
		_selectRequests.fire({});
	}
}

} // namespace

[[nodiscard]] static float64 Lerp(float64 from, float64 to, float64 progress) {
	return from + (to - from) * progress;
}

struct ChatSwitchProcess::Entry {
	not_null<Data::Thread*> thread;
	std::unique_ptr<Button> button;
};

ChatSwitchProcess::ChatSwitchProcess(
	not_null<Ui::RpWidget*> geometry,
	not_null<Main::Session*> session,
	Data::Thread *opened)
: _session(session)
, _widget(std::make_unique<Ui::RpWidget>(
	geometry->parentWidget() ? geometry->parentWidget() : geometry))
, _view(Ui::CreateChild<Ui::RpWidget>(_widget.get()))
, _bg(st::boxRadius, st::boxBg) {
	setupWidget(geometry);
	setupContent(opened);
	setupView();
}

ChatSwitchProcess::~ChatSwitchProcess() {
	_session->recentPeers().chatOpenKeepUserpics(std::move(_userpics));
}

rpl::producer<not_null<Data::Thread*>> ChatSwitchProcess::chosen() const {
	return _chosen.events();
}

rpl::producer<> ChatSwitchProcess::closeRequests() const {
	return _closeRequests.events();
}

void ChatSwitchProcess::process(const Request &request) {
	Expects(_selected < _shownCount);

	if (request.action == Qt::Key_Escape) {
		_closeRequests.fire({});
	} else if (request.action == Qt::Key_Enter) {
		if (_selected >= 0) {
			_chosen.fire_copy(_list[_selected]);
		} else {
			_closeRequests.fire({});
		}
	} else if (request.action == Qt::Key_Tab
		|| request.action == Qt::Key_Right) {
		if (_selected < 0 || _selected + 1 >= _shownCount) {
			setSelected(0);
		} else {
			setSelected(_selected + 1);
		}
	} else if (request.action == Qt::Key_Backtab
		|| request.action == Qt::Key_Left) {
		if (_selected <= 0) {
			setSelected(_shownCount - 1);
		} else {
			setSelected(_selected - 1);
		}
	} else if (request.action == Qt::Key_Up) {
		const auto now = std::max(_selected, 0) - _shownPerRow;
		const auto bound = (now < 0) ? (_shownCount + now) : now;
		setSelected(bound);
	} else if (request.action == Qt::Key_Down) {
		const auto now = std::max(_selected, 0) + _shownPerRow;
		const auto bound = (now >= _shownCount) ? (now - _shownCount) : now;
		setSelected(bound);
	} else if (request.action == Qt::Key_Q) {
		if (_selected >= 0) {
			const auto thread = _list[_selected];
			thread->session().recentPeers().chatOpenRemove(thread);
			remove(thread);
			CloseInWindows(thread);
		}
	}
}

void ChatSwitchProcess::setSelected(int index) {
	if (_selected == index || _list.size() < 2) {
		return;
	}
	if (_selected >= 0) {
		_entries[_selected].button->setSelected(false);
	}
	_selected = index;
	if (_selected >= 0) {
		_entries[_selected].button->setSelected(true);
	}
	updateSelectionPill(true);
}

QRect ChatSwitchProcess::selectionTarget() const {
	if (_selected < 0 || _selected >= int(_entries.size())) {
		return QRect();
	}
	const auto button = _entries[_selected].button.get();
	return button->isHidden()
		? QRect()
		: button->geometry().translated(_shadowed.topLeft());
}

void ChatSwitchProcess::updateSelectionPill(bool animated) {
	if (!_glass) {
		return;
	}
	const auto target = QRectF(selectionTarget());
	if (target.isEmpty()) {
		_pillAnimation.stop();
		_pillFrom = _pillTo = QRectF();
		_view->update();
		return;
	}
	const auto progress = _pillAnimation.value(1.);
	const auto current = (_pillTo.isEmpty() || _pillFrom.isEmpty())
		? _pillTo
		: QRectF(
			Lerp(_pillFrom.x(), _pillTo.x(), progress),
			Lerp(_pillFrom.y(), _pillTo.y(), progress),
			Lerp(_pillFrom.width(), _pillTo.width(), progress),
			Lerp(
				_pillFrom.height(),
				_pillTo.height(),
				progress));
	_pillFrom = current.isEmpty() ? target : current;
	_pillTo = target;
	if (animated && !current.isEmpty() && current != target) {
		_pillAnimation.start(
			[=] { _view->update(); },
			0.,
			1.,
			crl::time(380),
			anim::easeOutBack);
	} else {
		_pillAnimation.stop();
		_pillFrom = target;
	}
	_view->update();
}

void ChatSwitchProcess::setupWidget(not_null<Ui::RpWidget*> geometry) {
	geometry->geometryValue(
	) | rpl::on_next([=](QRect value) {
		const auto parent = geometry->parentWidget();
		_widget->setGeometry((parent == _widget->parentWidget())
			? value
			: QRect(QPoint(), value.size()));
	}, _widget->lifetime());

	_widget->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::MouseButtonPress) {
			crl::on_main(_widget.get(), [=] {
				_closeRequests.fire({});
			});
		}
	}, _widget->lifetime());

	_widget->show();
}

void ChatSwitchProcess::setupContent(Data::Thread *opened) {
	_list = _session->recentPeers().collectChatOpenHistory();
	if (_list.size() < 2) {
		return;
	}

	if (opened) {
		const auto i = ranges::find(_list, not_null(opened));
		if (i == end(_list)) {
			_list.insert(begin(_list), opened);
		} else if (i != begin(_list)) {
			ranges::rotate(begin(_list), i, i + 1);
		}
		_selected = 0;
	}

	const auto find = [=](Button *button) {
		return ranges::find(_entries, button, [](const Entry &entry) {
			return entry.button.get();
		});
	};
	for (const auto &thread : _list) {
		auto button = std::make_unique<Button>(
			_view.get(),
			thread,
			_userpics);
		const auto raw = button.get();

		raw->selectRequests() | rpl::on_next([=] {
			const auto i = find(raw);
			setSelected(int(i - begin(_entries)));
		}, raw->lifetime());

		raw->setClickedCallback([=] {
			_chosen.fire_copy(thread);
		});

		_entries.push_back({ thread, std::move(button) });

		auto destroyed = thread->asTopic()
			? thread->asTopic()->destroyed()
			: thread->asSublist()
			? thread->asSublist()->destroyed()
			: nullptr;
		if (!destroyed) {
			continue;
		}
		std::move(destroyed) | rpl::on_next([=] {
			remove(thread);
		}, raw->lifetime());
	}
}

void ChatSwitchProcess::remove(not_null<Data::Thread*> thread) {
	_list.erase(ranges::remove(_list, thread), end(_list));

	const auto i = ranges::find(_entries, thread, &Entry::thread);
	if (i != end(_entries)) {
		const auto selected = _selected;
		const auto index = int(i - begin(_entries));
		if (_selected > index) {
			--_selected;
		} else if (_selected == index) {
			_selected = -1;
		}

		_entries.erase(i);
		const auto weak = base::make_weak(_widget.get());
		layout(_widget->size());
		if (weak && _selected < 0 && selected > 0) {
			if (_entries.empty()) {
				_closeRequests.fire({});
			} else {
				setSelected(std::min(selected - 1, _shownCount - 1));
			}
		}
	}
}

void ChatSwitchProcess::setupView() {
	_widget->sizeValue() | rpl::on_next([=](QSize size) {
		layout(size);
	}, _view->lifetime());
	_view->show();

	if (Ui::LiquidGlass::Enabled()) {
		// Snapshot the window under the panel once, the panel is short-lived.
		if (const auto parent = _widget->parentWidget()) {
			const auto grab = Ui::GrabWidgetToImage(parent);
			if (!grab.isNull()) {
				_glassSnapshot = grab.scaled(
					parent->size(),
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation);
				_glassSnapshot.setDevicePixelRatio(1.);
				_glassSnapshotRect = QRect(
					-_widget->pos(),
					parent->size());
			}
		}
		_glass = std::make_unique<Ui::LiquidGlass::Surface>();
		_glass->setStyle(Ui::LiquidGlass::MakeStyle(
			st::boxBg->c,
			st::boxRadius * 2));
		_appearAnimation.start(
			[=] { _view->update(); },
			0.,
			1.,
			crl::time(420),
			anim::linear);
	}

	_view->paintRequest() | rpl::on_next([=](QRect clip) {
		if (_outer.isEmpty()) {
			return;
		}
		auto p = QPainter(_view);
		p.translate(-_shadowed.topLeft());
		if (!_glass) {
			Ui::Shadow::paint(p, _outer, _view->width(), st::boxRoundShadow);
			_bg.paint(p, _outer);
			return;
		}
		// Springy appearance: 90% -> ~102% -> 100% with a quick fade.
		const auto t = _appearAnimation.value(1.);
		const auto scale = (t >= 1.)
			? 1.
			: (1. - 0.1 * std::exp(-6. * t) * std::cos(10. * t));
		p.setOpacity(std::min(1., t * 4.));
		const auto center = QRectF(_outer).center();
		p.translate(center);
		p.scale(scale, scale);
		p.translate(-center);
		auto backdrop = Ui::LiquidGlass::Backdrop{
			.image = _glassSnapshot,
			.rect = _glassSnapshotRect,
		};
		_glass->paint(p, _outer, backdrop);

		const auto progress = _pillAnimation.value(1.);
		const auto pill = _pillFrom.isEmpty()
			? _pillTo
			: QRectF(
				Lerp(_pillFrom.x(), _pillTo.x(), progress),
				Lerp(_pillFrom.y(), _pillTo.y(), progress),
				Lerp(
					_pillFrom.width(),
					_pillTo.width(),
					progress),
				Lerp(
					_pillFrom.height(),
					_pillTo.height(),
					progress));
		if (!pill.isEmpty()) {
			auto hq = PainterHighQualityEnabler(p);
			const auto radius = st::boxRadius * 1.5;
			auto accent = st::defaultRoundCheckbox.bgActive->c;
			accent.setAlphaF(0.22);
			p.setPen(Qt::NoPen);
			p.setBrush(accent);
			p.drawRoundedRect(pill, radius, radius);
			auto gloss = QLinearGradient(pill.topLeft(), pill.bottomLeft());
			gloss.setColorAt(0., QColor(255, 255, 255, 60));
			gloss.setColorAt(0.5, QColor(255, 255, 255, 8));
			gloss.setColorAt(1., QColor(255, 255, 255, 0));
			p.setBrush(gloss);
			p.drawRoundedRect(pill, radius, radius);
			auto rim = st::defaultRoundCheckbox.bgActive->c;
			rim.setAlphaF(0.75);
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(rim, 1.5));
			p.drawRoundedRect(
				pill.marginsRemoved({ 0.75, 0.75, 0.75, 0.75 }),
				radius,
				radius);
		}
	}, _view->lifetime());

	_view->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::MouseButtonPress) {
			e->accept();
		}
	}, _view->lifetime());
}

void ChatSwitchProcess::layout(QSize size) {
	const auto full = QRect(QPoint(), size);
	const auto outer = full.marginsRemoved(st::chatSwitchMargins);
	auto inner = outer.marginsRemoved(st::chatSwitchPadding);
	const auto available = inner.width();
	const auto canPerRow = (available / st::chatSwitchSize.width());
	const auto canRows = (canPerRow > 2 * 7)
		? 1
		: (canPerRow > 3 * 4)
		? 2
		: 3;
	if (canPerRow < 1) {
		return;
	} else if (_list.size() < 2) {
		_closeRequests.fire({});
		return;
	}
	const auto count = int(_list.size());
	_shownRows = std::min(canRows, (count + canPerRow - 1) / canPerRow);
	_shownPerRow = std::min(count / _shownRows, canPerRow);
	if (_shownRows > 2) {
		if (_shownPerRow * 2 > _shownRows * 4) {
			_shownRows = 2;
		} else if (_shownPerRow > 4) {
			_shownPerRow = 4;
		}
	}
	if (_shownRows > 1) {
		if (_shownPerRow > _shownRows * 7) {
			_shownRows = 1;
		} else if (_shownPerRow > 7) {
			_shownPerRow = 7;
		}
	}
	_shownCount = _shownPerRow * _shownRows;
	if (_selected >= _shownCount) {
		_selected = -1;
	}

	const auto width = _shownPerRow * st::chatSwitchSize.width();
	const auto height = _shownRows * st::chatSwitchSize.height();

	size = QSize(width, height);
	_inner = QRect(
		(full.width() - width) / 2,
		(full.height() - height) / 2,
		width,
		height);
	_outer = _inner.marginsAdded(st::chatSwitchPadding);

	const auto padding = st::boxRoundShadow.extend + st::chatSwitchPadding;

	auto index = 0;
	auto top = padding.top();
	for (auto row = 0; row != _shownRows; ++row) {
		auto left = padding.left();
		for (auto column = 0; column != _shownPerRow; ++column) {
			const auto &entry = _entries[index++];
			entry.button->moveToLeft(left, top, _inner.width());
			entry.button->show();
			left += st::chatSwitchSize.width();
		}
		top += st::chatSwitchSize.height();
	}
	for (auto i = _shownRows * _shownPerRow; i < count; ++i) {
		_entries[i].button->hide();
	}

	_shadowed = _outer.marginsAdded(st::boxRoundShadow.extend);
	_view->setGeometry(_shadowed);
	updateSelectionPill(false);
}

rpl::lifetime &ChatSwitchProcess::lifetime() {
	return _lifetime;
}

} // namespace Window
