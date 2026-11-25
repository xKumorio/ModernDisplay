#include "rain_lighting.h"
#include "../core/log.h"

#include <windows.h>
#include <cstdint>

namespace {
using Byte = unsigned char;
struct Colour { Byte rgb[3]; };
struct Hour {
	Colour world, objects, zenith, horizon, lowCloud;
};
constexpr int kWeatherCount = 23;
constexpr int kRainWeather[] = { 8, 16 };
// SA 1.0 US CTimeCycle uses separate [hour][weather] arrays for every colour channel.
// https://github.com/gta-reversed/gta-reversed/blob/master/source/game_sa/TimeCycle.h
constexpr uintptr_t kChannels[][3] = {
	{ 0xB7C3C8, 0xB7C310, 0xB7C258 }, { 0xB7C1A0, 0xB7C0E8, 0xB7C030 },
	{ 0xB7BF78, 0xB7BEC0, 0xB7BE08 }, { 0xB7BD50, 0xB7BC98, 0xB7BBE0 },
	{ 0xB7AEF0, 0xB7AE38, 0xB7AD80 }
};
constexpr Colour Hour::* kFields[] = { &Hour::world, &Hour::objects, &Hour::zenith, &Hour::horizon, &Hour::lowCloud };
static_assert(ARRAYSIZE(kFields) == ARRAYSIZE(kChannels));

// Midnight, 05:00, 06:00, 07:00, noon, 19:00, 20:00, 22:00.
constexpr Hour kPalette[2][8] = {
	{
		{{20,26,34},{112,132,151},{13,20,33},{56,65,77},{38,44,56}},
		{{21,27,34},{115,136,154},{19,29,44},{75,83,93},{44,52,65}},
		{{30,35,41},{138,149,158},{45,58,76},{130,126,124},{83,86,96}},
		{{39,46,53},{155,166,175},{68,85,104},{151,164,173},{102,113,124}},
		{{45,52,59},{169,177,183},{78,98,118},{164,177,185},{115,128,139}},
		{{36,38,44},{153,153,158},{62,73,92},{149,128,118},{103,95,99}},
		{{25,29,37},{130,142,156},{32,43,62},{104,102,108},{66,66,78}},
		{{20,26,34},{117,136,152},{16,25,40},{65,74,88},{44,50,62}}
	}, {
		{{23,24,35},{119,133,154},{18,21,37},{67,67,85},{44,43,59}},
		{{24,26,37},{122,135,155},{24,29,49},{83,78,95},{53,51,67}},
		{{33,33,43},{143,148,162},{56,62,86},{149,127,124},{91,82,95}},
		{{41,44,51},{158,166,176},{77,87,109},{155,161,173},{106,111,130}},
		{{45,49,56},{171,176,182},{82,94,119},{166,173,187},{120,125,143}},
		{{40,37,45},{159,149,156},{69,67,94},{173,130,119},{115,91,104}},
		{{31,29,40},{143,142,158},{39,42,68},{125,101,112},{82,68,86}},
		{{23,25,36},{124,137,157},{21,26,44},{77,77,98},{52,50,69}}
	}
};

using Samples = Hour[2][8];
Samples original{};
bool applied = false;

enum class Operation { Capture, Apply, Restore };

void Transfer(Operation op)
{
	for (int rain = 0; rain < 2; ++rain) {
		for (int hour = 0; hour < 8; ++hour) {
			const int index = hour * kWeatherCount + kRainWeather[rain];
			for (size_t field = 0; field < ARRAYSIZE(kFields); ++field) {
				const auto member = kFields[field];
				for (int ch = 0; ch < 3; ++ch) {
					Byte &live = reinterpret_cast<Byte *>(kChannels[field][ch])[index];
					Byte &saved = (original[rain][hour].*member).rgb[ch];
					const Byte target = (kPalette[rain][hour].*member).rgb[ch];
					if (op == Operation::Capture) saved = live;
					else if (op == Operation::Apply) live = target;
					else if (live == target) live = saved;
				}
			}
		}
	}
}

bool TablesReady()
{
	// The loader fills the weather tables after the graphics device already exists.
	const auto *farClip = reinterpret_cast<const short *>(0xB7B1D0);
	return farClip[4 * kWeatherCount + 8] > 0 && farClip[7 * kWeatherCount + 16] > 0;
}

bool OriginalTableReloaded()
{
	// Reapply after GTA reloads the captured timecycle, without overwriting a live editor's individual changes.
	for (int rain = 0; rain < 2; ++rain) {
		for (int hour = 0; hour < 8; ++hour) {
			const int index = hour * kWeatherCount + kRainWeather[rain];
			for (size_t field = 0; field < ARRAYSIZE(kFields); ++field) {
				const auto member = kFields[field];
				for (int ch = 0; ch < 3; ++ch) {
					if (reinterpret_cast<const Byte *>(kChannels[field][ch])[index] !=
					    (original[rain][hour].*member).rgb[ch]) return false;
				}
			}
		}
	}
	return true;
}
}

void UpdateRainLighting(bool enabled)
{
	if (!enabled && !applied) return;
	static DWORD lastCheck = 0;
	const DWORD now = GetTickCount();
	if (enabled && applied && now - lastCheck < 1000) return;
	if (enabled && !applied && now - lastCheck < 100) return;
	lastCheck = now;
	if (!enabled) {
		Transfer(Operation::Restore);
		applied = false;
		Log("Rain lighting: original weather colours restored");
		return;
	}
	if (!TablesReady()) return;
	if (applied) {
		if (!OriginalTableReloaded()) return;
	} else Transfer(Operation::Capture);
	Transfer(Operation::Apply);
	applied = true;
	Log("Rain lighting: weather 8 and 16 active");
}
