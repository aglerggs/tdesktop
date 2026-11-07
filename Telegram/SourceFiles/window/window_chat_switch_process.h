/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/round_rect.h"
#include "ui/effects/animations.h"
#include "ui/effects/liquid_glass.h"
#include "ui/userpic_view.h"

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Shortcuts {
struct ChatSwitchRequest;
} // namespace Shortcuts

namespace Ui {
class AbstractButton;
class RpWidget;
} // namespace Ui

namespace Window {

class ChatSwitchProcess final {
public:
	// Create widget in geometry->parentWidget() and geometry->geometry().
	ChatSwitchProcess(
		not_null<Ui::RpWidget*> geometry,
		not_null<Main::Session*> session,
		Data::Thread *opened);
	~ChatSwitchProcess();

	[[nodiscard]] rpl::producer<not_null<Data::Thread*>> chosen() const;
	[[nodiscard]] rpl::producer<> closeRequests() const;

	using Request = Shortcuts::ChatSwitchRequest;
	void process(const Request &request);

	[[nodiscard]] rpl::lifetime &lifetime();

private:
	struct Entry;

	void setupWidget(not_null<Ui::RpWidget*> geometry);
	void setupContent(Data::Thread *opened);
	void setupView();

	void layout(QSize size);
	void remove(not_null<Data::Thread*> thread);

	void setSelected(int index);

	const not_null<Main::Session*> _session;
	const std::unique_ptr<Ui::RpWidget> _widget;
	const not_null<Ui::RpWidget*> _view;

	QRect _shadowed;
	QRect _outer;
	QRect _inner;
	Ui::RoundRect _bg;

	// Liquid glass: the panel is glass over the window snapshot, the
	// selection is a glossy pill sliding with a spring between chats.
	void updateSelectionPill(bool animated);
	[[nodiscard]] QRect selectionTarget() const;
	std::unique_ptr<Ui::LiquidGlass::Surface> _glass;
	QImage _glassSnapshot;
	QRect _glassSnapshotRect;
	QRectF _pillFrom;
	QRectF _pillTo;
	Ui::Animations::Simple _pillAnimation;
	Ui::Animations::Simple _appearAnimation;

	base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> _userpics;
	std::vector<not_null<Data::Thread*>> _list;
	std::vector<Entry> _entries;

	int _selected = -1;
	int _shownRows = 0;
	int _shownCount = 0;
	int _shownPerRow = 0;

	rpl::event_stream<not_null<Data::Thread*>> _chosen;
	rpl::event_stream<> _closeRequests;

	rpl::lifetime _lifetime;

};

} // namespace Window
