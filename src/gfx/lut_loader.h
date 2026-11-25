// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

struct Lut3DData {
	UINT                  size = 0;
	std::vector<uint16_t> rgba16; // size^3 * 4 texels (R, G, B, 65535) for DXGI_FORMAT_R16G16B16A16_UNORM
};

bool LoadCubeLutFile(const char *lutNameOrPath, Lut3DData &out, std::string *resolvedPathOut = nullptr);
std::vector<std::string> ListAvailableCubeLuts();
