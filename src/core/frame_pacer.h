// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <stdint.h>

// High-precision hybrid frame pacer:
// Uses Win10 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION for coarse sleep (~0.5ms accuracy, zero CPU burn)
// followed by a tight QPC + _mm_pause() spin-wait for the final sub-millisecond (<0.02ms jitter).
class FramePacer {
public:
	FramePacer();
	~FramePacer();

	void Reset();
	void Pace(double targetFps);

private:
	HANDLE        m_timer = nullptr;
	bool          m_highResTimer = false;
	LARGE_INTEGER m_freq = {};
	LONGLONG      m_nextDeadline = 0;
	double        m_lastTarget = 0.0;
};
