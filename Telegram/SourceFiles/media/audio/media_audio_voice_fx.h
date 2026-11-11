/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// aglergram: voice effects applied to the recorded samples before they
// are encoded, so the voice message itself sounds changed for everyone.
namespace Media::Capture {

enum class VoiceEffect {
	None,
	Robot,
	Chipmunk,
	Deep,
	Echo,
	Radio,
	Cave,
};

// Chosen on the main thread, read by the capture thread at its start.
[[nodiscard]] VoiceEffect CurrentVoiceEffect();
void SetCurrentVoiceEffect(VoiceEffect effect);

class VoiceFx {
public:
	virtual ~VoiceFx() = default;

	// Mono signed 16-bit samples, processed in place.
	virtual void process(int16 *samples, int count) = 0;

};

// nullptr for VoiceEffect::None.
[[nodiscard]] std::unique_ptr<VoiceFx> MakeVoiceFx(
	VoiceEffect effect,
	int frequency);

} // namespace Media::Capture
