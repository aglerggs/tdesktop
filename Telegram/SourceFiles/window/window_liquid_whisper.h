/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

// aglergram: voice and round messages to text on this computer with
// whisper.cpp, no Premium. The program and the model are downloaded
// into tdata/whisper on the first use.
namespace Window::Liquid {

[[nodiscard]] bool CanTranscribeLocally(not_null<HistoryItem*> item);
void TranscribeLocally(
	not_null<SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Window::Liquid
