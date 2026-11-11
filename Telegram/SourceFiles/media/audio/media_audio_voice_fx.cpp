/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/audio/media_audio_voice_fx.h"

#include <atomic>
#include <cmath>

namespace Media::Capture {
namespace {

std::atomic<int> GlobalEffect = 0;

[[nodiscard]] int16 ToSample(float value) {
	return int16(std::clamp(value, -32768.f, 32767.f));
}

// Soft limiter so loud effects do not crackle.
[[nodiscard]] float Soft(float value) {
	const auto x = value / 32768.f;
	return 32768.f * (x / (1.f + 0.28f * std::abs(x)));
}

// Second-order filter, RBJ cookbook.
class Biquad final {
public:
	void lowPass(float frequency, float rate, float q = 0.707f) {
		const auto w = 2.f * float(M_PI) * frequency / rate;
		const auto alpha = std::sin(w) / (2.f * q);
		const auto cosw = std::cos(w);
		set(
			(1.f - cosw) / 2.f,
			1.f - cosw,
			(1.f - cosw) / 2.f,
			1.f + alpha,
			-2.f * cosw,
			1.f - alpha);
	}
	void highPass(float frequency, float rate, float q = 0.707f) {
		const auto w = 2.f * float(M_PI) * frequency / rate;
		const auto alpha = std::sin(w) / (2.f * q);
		const auto cosw = std::cos(w);
		set(
			(1.f + cosw) / 2.f,
			-(1.f + cosw),
			(1.f + cosw) / 2.f,
			1.f + alpha,
			-2.f * cosw,
			1.f - alpha);
	}
	[[nodiscard]] float run(float x) {
		const auto y = _b0 * x + _b1 * _x1 + _b2 * _x2 - _a1 * _y1 - _a2 * _y2;
		_x2 = _x1;
		_x1 = x;
		_y2 = _y1;
		_y1 = y;
		return y;
	}

private:
	void set(float b0, float b1, float b2, float a0, float a1, float a2) {
		_b0 = b0 / a0;
		_b1 = b1 / a0;
		_b2 = b2 / a0;
		_a1 = a1 / a0;
		_a2 = a2 / a0;
	}

	float _b0 = 1.f, _b1 = 0.f, _b2 = 0.f, _a1 = 0.f, _a2 = 0.f;
	float _x1 = 0.f, _x2 = 0.f, _y1 = 0.f, _y2 = 0.f;

};

// Pitch without changing the length: two read heads slide over a delay
// line at the pitch rate and crossfade with triangular windows.
class PitchShift final {
public:
	PitchShift(float ratio, int frequency)
	: _ratio(ratio)
	, _window(float(std::max(frequency / 24, 256))) // ~40 ms.
	, _size(1) {
		while (_size < int(_window) * 2 + 4) {
			_size <<= 1;
		}
		_buffer.assign(_size, 0.f);
	}

	[[nodiscard]] float run(float x) {
		_buffer[_write & (_size - 1)] = x;
		const auto first = tap(_phase);
		auto second = _phase + _window / 2.f;
		if (second >= _window) {
			second -= _window;
		}
		const auto result = first + tap(second);
		_phase += 1.f - _ratio;
		while (_phase < 0.f) {
			_phase += _window;
		}
		while (_phase >= _window) {
			_phase -= _window;
		}
		++_write;
		return result;
	}

private:
	[[nodiscard]] float tap(float delay) const {
		// Triangle: 0 at the window edges, 1 in the middle.
		const auto gain = 1.f - std::abs(2.f * delay / _window - 1.f);
		const auto position = float(_write) - delay - 1.f;
		const auto base = std::floor(position);
		const auto fraction = position - base;
		const auto index = int64(base);
		const auto a = _buffer[index & (_size - 1)];
		const auto b = _buffer[(index + 1) & (_size - 1)];
		return gain * (a + (b - a) * fraction);
	}

	float _ratio = 1.f;
	float _window = 1024.f;
	int _size = 1;
	std::vector<float> _buffer;
	int64 _write = 0;
	float _phase = 0.f;

};

class Robot final : public VoiceFx {
public:
	explicit Robot(int frequency) : _frequency(frequency) {
		_low.lowPass(4000.f, float(frequency));
	}
	void process(int16 *samples, int count) override {
		const auto step = 2. * M_PI * 55. / _frequency;
		for (auto i = 0; i != count; ++i) {
			const auto carrier = float(std::sin(_phase));
			_phase += step;
			if (_phase > 2. * M_PI) {
				_phase -= 2. * M_PI;
			}
			const auto x = float(samples[i]);
			// Ring modulation keeps a bit of the dry voice for clarity.
			const auto wet = _low.run(x * carrier * 1.6f + x * 0.25f);
			samples[i] = ToSample(Soft(wet));
		}
	}

private:
	int _frequency = 48000;
	double _phase = 0.;
	Biquad _low;

};

class Pitch final : public VoiceFx {
public:
	Pitch(float ratio, int frequency) : _shift(ratio, frequency) {
	}
	void process(int16 *samples, int count) override {
		for (auto i = 0; i != count; ++i) {
			samples[i] = ToSample(Soft(_shift.run(float(samples[i]))));
		}
	}

private:
	PitchShift _shift;

};

class Echo final : public VoiceFx {
public:
	Echo(int frequency, float delay, float feedback, float mix, bool dark)
	: _feedback(feedback)
	, _mix(mix)
	, _dark(dark) {
		_buffer.assign(std::max(int(frequency * delay), 1), 0.f);
		_low.lowPass(dark ? 1800.f : 5000.f, float(frequency));
	}
	void process(int16 *samples, int count) override {
		const auto size = int(_buffer.size());
		for (auto i = 0; i != count; ++i) {
			const auto x = float(samples[i]);
			const auto delayed = _buffer[_index];
			_buffer[_index] = x + _low.run(delayed) * _feedback;
			if (++_index == size) {
				_index = 0;
			}
			samples[i] = ToSample(Soft(x + delayed * _mix));
		}
	}

private:
	std::vector<float> _buffer;
	int _index = 0;
	float _feedback = 0.4f;
	float _mix = 0.5f;
	bool _dark = false;
	Biquad _low;

};

class Radio final : public VoiceFx {
public:
	explicit Radio(int frequency) {
		_high.highPass(400.f, float(frequency));
		_low.lowPass(3000.f, float(frequency));
	}
	void process(int16 *samples, int count) override {
		for (auto i = 0; i != count; ++i) {
			const auto band = _low.run(_high.run(float(samples[i])));
			// A bit of drive, like a cheap walkie-talkie speaker.
			const auto driven = std::tanh(band / 32768.f * 3.f) * 20000.f;
			_noise = _noise * 1664525u + 1013904223u;
			const auto hiss = (float(_noise >> 16) / 65535.f - 0.5f) * 220.f;
			samples[i] = ToSample(driven + hiss);
		}
	}

private:
	Biquad _high;
	Biquad _low;
	uint32 _noise = 12345;

};

} // namespace

VoiceEffect CurrentVoiceEffect() {
	return VoiceEffect(GlobalEffect.load());
}

void SetCurrentVoiceEffect(VoiceEffect effect) {
	GlobalEffect = int(effect);
}

std::unique_ptr<VoiceFx> MakeVoiceFx(VoiceEffect effect, int frequency) {
	switch (effect) {
	case VoiceEffect::Robot: return std::make_unique<Robot>(frequency);
	case VoiceEffect::Chipmunk:
		return std::make_unique<Pitch>(1.6f, frequency);
	case VoiceEffect::Deep: return std::make_unique<Pitch>(0.72f, frequency);
	case VoiceEffect::Echo:
		return std::make_unique<Echo>(frequency, 0.28f, 0.42f, 0.55f, false);
	case VoiceEffect::Cave:
		return std::make_unique<Echo>(frequency, 0.11f, 0.68f, 0.7f, true);
	case VoiceEffect::Radio: return std::make_unique<Radio>(frequency);
	case VoiceEffect::None: break;
	}
	return nullptr;
}

} // namespace Media::Capture
