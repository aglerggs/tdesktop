/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// aglergram: a video as a round message or a GIF, quick reply buttons
// above the message field and the summary of the day.
namespace Window::Liquid {

enum class ConvertKind {
	Round,
	Gif,
};

// Picks a video file and sends the converted one to the open chat.
void ConvertFromFile(
	not_null<SessionController*> controller,
	ConvertKind kind);

// The same for a video from a message, sent to the open chat.
[[nodiscard]] bool CanConvertMessage(not_null<HistoryItem*> item);
void ConvertMessage(
	not_null<SessionController*> controller,
	not_null<HistoryItem*> item,
	ConvertKind kind);

// Quick replies: up to 8 own phrases, one click sends one.
[[nodiscard]] QStringList QuickReplies();
void ShowQuickRepliesBox(not_null<SessionController*> controller);
[[nodiscard]] not_null<Ui::RpWidget*> CreateQuickRepliesStrip(
	not_null<Ui::RpWidget*> parent,
	Fn<void(QString)> send);

// The summary of the day: who wrote, mentions, what is still unread.
void ShowDaySummary(not_null<SessionController*> controller);
[[nodiscard]] bool DaySummaryEnabled();
void SetDaySummaryEnabled(bool enabled);
void StartDaySummary(not_null<SessionController*> controller);

} // namespace Window::Liquid
