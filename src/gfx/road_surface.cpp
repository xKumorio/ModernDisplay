#include "road_surface.h"

#include "../core/log.h"

#include <cmath>
#include <cstdint>

namespace {
struct Point3 { float x, y, z; };
struct CollisionPoint {
	Point3 point;
	float unused0;
	Point3 normal;
	float unused1;
	BYTE surfaceA[4];
	BYTE surfaceB[4];
	float depth;
};
static_assert(sizeof(CollisionPoint) == 0x2c);

bool IsPavedSurface(BYTE material)
{
	return material <= 5 || material == 7 || material == 8;
}

float SampleHeight(float x, float y, float z, bool &groundKnown)
{
	using ProcessVerticalLine = bool(__cdecl *)(const Point3 &, float, CollisionPoint &, void *&,
	                                            bool, bool, bool, bool, bool, bool, void *);
	constexpr uintptr_t kProcessVerticalLine = 0x5674E0;
	Point3 start = { x, y, z + 2.0f };
	CollisionPoint hit = {};
	void *entity = nullptr;
	groundKnown = false;
	__try {
		const auto trace = reinterpret_cast<ProcessVerticalLine>(kProcessVerticalLine);
		const bool found = trace(start, z - 20.0f, hit, entity, true, false, false, false, false, false, nullptr);
		groundKnown = found && std::isfinite(hit.point.z) && hit.point.z < start.z - 0.03f;
		if (groundKnown && IsPavedSurface(hit.surfaceB[0]) && hit.normal.z > 0.72f) {
			return hit.point.z + (hit.surfaceB[0] == 0 ? kUnknownMaterialOffset : 0.0f);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return kNoRoadHeight;
}
}

bool FindPavedGround(float x, float y, float z, float &height)
{
	bool known = false;
	height = SampleHeight(x, y, z, known);
	if (!known || height == kNoRoadHeight) return false;
	if (height > kUnknownMaterialOffset * 0.5f) height -= kUnknownMaterialOffset;
	return z >= height - 0.3f && z < height + 3.5f;
}

bool RoadSurfaceCache::Update(const float view[16], RoadSurfaceGrid &output)
{
	const float cx = -(view[12] * view[0] + view[13] * view[1] + view[14] * view[2]);
	const float cy = -(view[12] * view[4] + view[13] * view[5] + view[14] * view[6]);
	const float cz = -(view[12] * view[8] + view[13] * view[9] + view[14] * view[10]);
	if (!(std::fabs(cx) < 10000.0f && std::fabs(cy) < 10000.0f && std::fabs(cz) < 2000.0f)) return false;

	// Center the 144m x 144m grid 30m ahead of the camera (~102m ahead, ~42m behind).
	const int x0 = static_cast<int>(std::floor((cx + view[2] * 30.0f) / kRoadGridSpacing)) - kRoadGridSize / 2;
	const int y0 = static_cast<int>(std::floor((cy + view[6] * 30.0f) / kRoadGridSpacing)) - kRoadGridSize / 2;
	const DWORD now = GetTickCount();
	constexpr int count = kRoadGridSize * kRoadGridSize;
	auto cell = [this](int x, int y) -> Sample & {
		return m_samples[(static_cast<unsigned>(y) % kRoadGridSize) * kRoadGridSize +
		                 static_cast<unsigned>(x) % kRoadGridSize];
	};

	// Pass 1: fill newly entered / invalid cells first (up to 16/frame) so puddles ahead of a fast car
	// are 100% ready ~100m in front before you ever reach them.
	int queries = 0;
	for (int visited = 0; visited < count && queries < 16; ++visited) {
		const int i = static_cast<int>(m_cursor++ % count);
		const int x = x0 + i % kRoadGridSize;
		const int y = y0 + i / kRoadGridSize;
		Sample &sample = cell(x, y);
		const bool valid = sample.initialized && sample.x == x && sample.y == y && std::fabs(sample.cameraZ - cz) < 4.0f;
		if (valid) continue;
		bool groundKnown = false;
		const float height = SampleHeight(x * kRoadGridSpacing, y * kRoadGridSpacing, cz, groundKnown);
		sample = { x, y, cz, height, now, true, groundKnown };
		++queries;
	}
	// Pass 2: if the grid is already warm, lazily refresh up to 2 expired cells/frame (near-zero CPU cost).
	if (queries == 0) {
		for (int visited = 0; visited < 128 && queries < 2; ++visited) {
			const int i = static_cast<int>(m_cursor++ % count);
			const int x = x0 + i % kRoadGridSize;
			const int y = y0 + i / kRoadGridSize;
			Sample &sample = cell(x, y);
			const DWORD interval = sample.groundKnown ? 30000 : 1000;
			if (now - sample.time < interval) continue;
			bool groundKnown = false;
			const float height = SampleHeight(x * kRoadGridSpacing, y * kRoadGridSpacing, cz, groundKnown);
			sample = { x, y, cz, height, now, true, groundKnown };
			++queries;
		}
	}

	if (queries == 0 && x0 == m_lastOriginX && y0 == m_lastOriginY && output.revision == m_revision && m_revision != 0)
		return m_lastRoadSamples != 0;

	m_lastOriginX = x0;
	m_lastOriginY = y0;
	output.originX = x0;
	output.originY = y0;
	unsigned roadSamples = 0;
	for (int i = 0; i < count; ++i) {
		const int x = x0 + i % kRoadGridSize;
		const int y = y0 + i / kRoadGridSize;
		const Sample &sample = cell(x, y);
		const bool valid = sample.initialized && sample.x == x && sample.y == y && std::fabs(sample.cameraZ - cz) < 4.0f;
		output.heights[i] = valid ? sample.height : kNoRoadHeight;
		roadSamples += output.heights[i] != kNoRoadHeight;
	}
	m_lastRoadSamples = roadSamples;
	output.revision = ++m_revision;
	static bool logged = false;
	if (roadSamples && !logged) {
		logged = true;
		Log("Rain: using paved collision surfaces for puddles");
	}
	return roadSamples != 0;
}
