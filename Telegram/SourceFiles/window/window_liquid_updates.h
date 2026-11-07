/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

// Liquid glass fork: patch notes, self-update from GitHub releases,
// backups of the fork's local data and live video wallpapers.
namespace Window::Liquid {

[[nodiscard]] QString CurrentVersion();

// Shows "what's new" once after an update, checks for a new release.
void StartUpdates(not_null<SessionController*> controller);
void ShowPatchHistory(not_null<SessionController*> controller);
void CheckForUpdates(not_null<SessionController*> controller, bool manual);

void ExportBackup(not_null<SessionController*> controller);
void ImportBackup(not_null<SessionController*> controller);

// Live wallpaper drawn instead of the chat background.
void ShowLiveWallpaperBox(not_null<SessionController*> controller);
[[nodiscard]] bool PaintLiveWallpaper(QPainter &p, QSize fill, QRect clip);
[[nodiscard]] bool LiveWallpaperActive();
[[nodiscard]] rpl::producer<> LiveWallpaperFrames();

} // namespace Window::Liquid
