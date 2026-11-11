/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

// aglergram: built-in plugins — every feature of the fork as a card that
// can be turned off, plus ChatSaver and Forward Pro.
namespace Window::Liquid {

// Ids: textmodes, quickreplies, whisper, convert, bookmarks, chatsaver,
// forwardpro, twitch, hidden. Everything is on until turned off.
[[nodiscard]] bool PluginEnabled(const QString &id);
void SetPluginEnabled(const QString &id, bool enabled);
[[nodiscard]] rpl::producer<bool> PluginEnabledValue(const QString &id);

void ShowLocalRenameBox(
	not_null<SessionController*> controller,
	not_null<PeerData*> peer);
// Applies stored plugin settings at the start (the voice effect).
void StartPlugins();
void ShowRoundFrameBox(not_null<SessionController*> controller);
void ShowVoiceFxBox(not_null<SessionController*> controller);
void ShowPluginsBox(not_null<SessionController*> controller);

// ChatSaver: the whole history of a chat into a .txt or .html file.
void SaveChatToFile(
	not_null<SessionController*> controller,
	not_null<History*> history);

// Forward Pro: send a copy of a message from another logged-in account.
[[nodiscard]] bool CanForwardFromOtherAccount(
	not_null<SessionController*> controller,
	not_null<HistoryItem*> item);
void ForwardFromOtherAccount(
	not_null<SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Window::Liquid
