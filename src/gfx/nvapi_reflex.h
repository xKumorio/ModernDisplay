// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <d3d11.h>

enum ReflexMarker : int32_t {
	RM_SIMULATION_START   = 0,
	RM_SIMULATION_END     = 1,
	RM_RENDERSUBMIT_START = 2,
	RM_RENDERSUBMIT_END   = 3,
	RM_PRESENT_START      = 4,
	RM_PRESENT_END        = 5,
	RM_INPUT_SAMPLE       = 6,
};

class NvReflex {
public:
	bool Init(ID3D11Device *dev11);
	void Shutdown();

	void Sleep();
	void BeginFrame();
	void Mark(ReflexMarker type);
	void VerifyOnce();

	bool Available() const { return m_available; }

private:
	HMODULE       m_nvapi = nullptr;
	ID3D11Device *m_dev = nullptr;
	bool          m_available = false;
	bool          m_initialized = false;
	bool          m_verifiedLogged = false;
	uint64_t      m_frameId = 0;
};
