#pragma once

#include <windows.h>

constexpr int kRoadGridSize = 48;
constexpr float kRoadGridSpacing = 3.0f;
constexpr float kNoRoadHeight = -10000.0f;
constexpr float kUnknownMaterialOffset = 4096.0f;

struct RoadSurfaceGrid {
	int originX = 0;
	int originY = 0;
	unsigned revision = 0;
	float heights[kRoadGridSize * kRoadGridSize] = {};
};

bool FindPavedGround(float x, float y, float z, float &height);

class RoadSurfaceCache {
public:
	bool Update(const float view[16], RoadSurfaceGrid &output);

private:
	struct Sample {
		int x = 0;
		int y = 0;
		float cameraZ = 0.0f;
		float height = kNoRoadHeight;
		DWORD time = 0;
		bool initialized = false;
		bool groundKnown = false;
	};
	Sample m_samples[kRoadGridSize * kRoadGridSize] = {};
	unsigned m_cursor = 0;
	unsigned m_revision = 0;
	int m_lastOriginX = 0x7fffffff;
	int m_lastOriginY = 0x7fffffff;
	unsigned m_lastRoadSamples = 0;
};
