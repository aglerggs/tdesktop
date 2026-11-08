/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// aglergram: live wallpapers drawn by the app itself — aura gradients,
// northern lights, snow, rain, fireflies, lava lamp. No video files.
namespace Window::Liquid {

struct AuraPreset {
	QString id; // Stored as "aura:<id>".
	QString title;
};

[[nodiscard]] const std::vector<AuraPreset> &AuraPresets();
[[nodiscard]] bool IsAuraPath(const QString &path);
[[nodiscard]] QString AuraPath(const QString &id);

// One frame at the moment `ms`, small: it is scaled up smoothly.
[[nodiscard]] QImage RenderAuraFrame(const QString &path, crl::time ms);

} // namespace Window::Liquid
