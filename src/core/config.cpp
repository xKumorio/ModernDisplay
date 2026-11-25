// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DisplayConfig g_cfg;
volatile LONG g_cfgSeq = 1;

static float ClampF(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static int ClampI(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void TrimStr(char *s)
{
	char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = 0;
}

const char *ReflexModeName(ReflexMode m)
{
	switch (m) {
	case ReflexMode::Off:   return "off";
	case ReflexMode::On:    return "on";
	case ReflexMode::Boost: return "boost";
	}
	return "boost";
}

const char *ScalingModeName(ScalingMode m)
{
	switch (m) {
	case ScalingMode::Stretch: return "stretch";
	case ScalingMode::Aspect:  return "aspect";
	case ScalingMode::Integer: return "integer";
	}
	return "stretch";
}

void FormatFpsLimit(int v, char *buf, size_t cap)
{
	if (v < 0)      snprintf(buf, cap, "auto");
	else if (v == 0) snprintf(buf, cap, "0");
	else            snprintf(buf, cap, "%d", v);
}

void ConfigNotifyChanged()
{
	InterlockedIncrement(&g_cfgSeq);
}

static void BuildIniPath(char *out, size_t cap)
{
	snprintf(out, cap, "%s\\%s.ini", g_dir, MD_NAME);
}

void ConfigSave()
{
	char path[MAX_PATH];
	BuildIniPath(path, sizeof(path));

	FILE *f = nullptr;
	if (fopen_s(&f, path, "w") != 0 || !f) return;

	char fpsStr[16];
	FormatFpsLimit(g_cfg.fpsLimit, fpsStr, sizeof(fpsStr));

	// Write UTF-8 BOM so Windows Notepad opens Russian/UTF-8 comments cleanly
	static const unsigned char kBom[3] = { 0xEF, 0xBB, 0xBF };
	fwrite(kBom, 1, 3, f);

	if (g_cfg.english) {
		fprintf(f,
			"; ============================================================================\n"
			"; " MD_NAME " v" MD_VERSION " - (c) 2026 " MD_AUTHOR " (" MD_AUTHOR_URL ")\n"
			"; D3D9Ex -> D3D11 DXGI Flip Model + Low-Latency Suite + CAS + Frame Pacer\n"
			";\n"
			"; All settings below can be changed live in-game without restarting:\n"
			";   SA-MP:    /md chat command (type /md for help and current values)\n"
			";   Hotkeys:  Alt+Enter - toggle windowed / borderless fullscreen\n"
			";             Alt+F10   - toggle minimal FPS & latency HUD\n"
			"; ============================================================================\n\n"

			"[Language]\n"
			"; 0 = Russian (default), 1 = English\n"
			"; Changing this rewrites all comments in this .ini on next launch (all settings are preserved).\n"
			"; In-game command: /md lang <ru|en>\n"
			"english=%d\n\n"

			"[Performance]\n"
			"; Hardware input-latency reduction (Universal Low-Latency Suite - works on all GPUs).\n"
			"; On NVIDIA automatically uses native NVIDIA Reflex (nvapi.dll).\n"
			"; On AMD Radeon and Intel Arc/UHD/Xe automatically enables Universal DX11 Low-Latency (ULL-DX11):\n"
			"; MaxLatency=1 queue + pre-input sync before mouse/keyboard polling + HW latency telemetry.\n"
			"; In-game: /md reflex <off|on|boost>\n"
			";   off   - disabled (maximum raw FPS without latency pacing)\n"
			";   on    - minimal frame queue and low input lag\n"
			";   boost - same + high GPU clocks (NVIDIA) / tight pre-input sync (AMD/Intel, recommended)\n"
			"reflex=%s\n\n"

			"; Asynchronous D3D11 presentation thread (double ping-pong buffer).\n"
			"; Moves post-processing and DXGI Flip Present off the game thread (+20-35%% FPS when CPU-bound).\n"
			"; In-game: /md async <0|1>\n"
			";   1 - enabled (recommended)\n"
			";   0 - disabled (synchronous present on game thread, for debugging)\n"
			"async_present=%d\n"
			"; Experimental: shared render targets with async present when MSAA=0 and discard swap effect.\n"
			"direct_render_targets=%d\n\n"

			"; Deep SSE2 cache for D3D9 states, matrices, lights, and shader constants.\n"
			"; Eliminates tens of thousands of redundant RenderWare/SA-MP driver calls per second (2-5x faster),\n"
			"; trims unchanged shader constants via SSE2, and skips empty 0-primitive draws.\n"
			"; Main source of FPS gain in crowded SA-MP areas.\n"
			"; In-game: /md cache <0|1>\n"
			";   1 - enabled (recommended, maximum FPS)\n"
			";   0 - disabled (pass all D3D9 calls straight to driver)\n"
			"state_cache=%d\n\n"

			"; Smart D3DPOOL_MANAGED texture updates (Dirty-Rect + skip D3DLOCK_READONLY).\n"
			"; Merges chat font, textdraw, and 3D text edits into a minimal dirty rectangle\n"
			"; (6x faster during chat spam, zero redundant GPU uploads on read-only locks).\n"
			"; In-game: /md managed <0|1>\n"
			";   1 - enabled (recommended, removes chat/3D-text microstutters)\n"
			";   0 - disabled (classic full texture upload on every UnlockRect)\n"
			"smart_managed=%d\n"
			"; Compressed DXT mip-level cache. Takes effect on next launch.\n"
			"compressed_mip_cache=%d\n\n"

			"; Instant F8 screenshots in SA-MP with zero freeze (background PNG compression via Windows WIC).\n"
			"; Captures in <1 ms instead of 250-600 ms freeze and encodes PNG on a background thread.\n"
			"; In-game: /md shot <0|1> (or /md f8)\n"
			";   1 - enabled (recommended, 0 ms freeze on F8)\n"
			";   0 - disabled (stock synchronous SA-MP screenshot)\n"
			"async_screenshot=%d\n\n"

			"; High-precision FPS limiter (Win10 High-Resolution Waitable Timer + QPC spin-wait).\n"
			"; Delivers a flat frametime graph (~0.005 ms jitter vs ~0.7 ms with regular Sleep).\n"
			"; In-game: /md fps <off|auto|20..1000>\n"
			";   0       - off (uncapped)\n"
			";   auto    - auto cap (monitor Hz - 3, e.g. 141 on 144 Hz) - ideal for G-Sync/FreeSync\n"
			";   20..1000 - exact FPS cap (e.g. 144, 200, 240, 300)\n"
			"fps_limit=%s\n\n"

			"; Background FPS limit (when minimized or unfocused in Discord/browser).\n"
			"; Keeps GPU cool and quiet in the background without pausing SA-MP.\n"
			"; In-game: /md bgfps <0|10..240>\n"
			";   0       - off\n"
			";   10..240 - target background FPS (30 recommended)\n"
			"bg_fps_limit=%d\n\n"

			"[Graphics]\n"
			"; Built-in D3D11 XeGTAO (Ambient Occlusion on NVIDIA, AMD, and Intel without NVIDIA Inspector).\n"
			"; Computes contact shadows from the real Z-buffer and never bleeds through chat or HUD.\n"
			"; Note: reading the D3D9 Z-buffer requires MSAA=0 (MSAA is disabled automatically when ao>0).\n"
			"; In-game: /md ao <0|1|2|3|off|low|med|high>\n"
			";   0 - off (default, preserves native MSAA)\n"
			";   1 - Low (fast, ~0.15 ms)\n"
			";   2 - Medium (balanced, ~0.25 ms)\n"
			";   3 - High (maximum detail, ~0.40 ms)\n"
			"ao=%d\n"
			"; XeGTAO darkening strength (0.10..2.00, default 0.85).\n"
			"; In-game: /md aostr <0.1-2.0> (or /md ao 1.2)\n"
			"ao_strength=%.2f\n\n"

			"; 3D anti-aliasing SMAA 1x / Temporal AA (SMAA T2x) - removes jagged edges and shimmering without blur.\n"
			"; Masked strictly to the 3D world; never touches SA-MP chat, nametags, or HUD.\n"
			"; In-game: /md aa <0|1|2|3|off|med|high|taa> (or /md taa)\n"
			";   0 - off\n"
			";   1 - SMAA 1x Medium (~0.08 ms)\n"
			";   2 - SMAA 1x High + Depth/AO Edge (~0.12 ms)\n"
			";   3 - SMAA T2x (Temporal AA using GTA SA camera matrices, removes motion shimmer, ~0.18 ms)\n"
			"aa=%d\n\n"

			"; Screen-Space Reflections SSR (water and wet asphalt in rain, ~0.25 ms, 3D-only).\n"
			"; Reads CWeather::Rain and CWeather::WetRoads from GTA SA memory - puddles reflect lamps, cars, and buildings.\n"
			"; In-game: /md ssr <0|1|2|off|med|high>\n"
			";   0 - off\n"
			";   1 - Medium (water + wet roads in rain, 20 steps)\n"
			";   2 - High (maximum sharpness and reflection range, 32 steps)\n"
			"ssr=%d\n\n"

			"; Temporal Screen-Space Global Illumination SSGI (color bounce from walls, headlights, and neon, ~0.22 ms).\n"
			"; Taillights, streetlamps, signs, and sunset light softly bounce onto asphalt, cars, and peds (3D-only).\n"
			"; In-game: /md gi <0..100> (or /md ssgi 0.35)\n"
			"gi=%.2f\n"
			"; 1 - evaluate SSGI at display resolution when SSAA is active (saves GPU time).\n"
			"ssgi_display_res=%d\n\n"

			"; Rain FX: puddle reflections when SSR is enabled, ripples, and screen droplets in rain.\n"
			"; Automatically turns off in interiors.\n"
			"; In-game: /md rainfx <0|1> (or /md drops)\n"
			"rain_fx=%d\n"
			"; Time-of-day lighting for weather IDs 8 and 16. Command: /md rainlight.\n"
			"rain_lighting=%d\n"
			"; Screen lens droplets separately: /md raindrops <0|1>\n"
			"rain_lens_drops=%d\n\n"
			"; Wheel water spray: 0 - off, 1 - original, 2 - reduced spray in standing water (/md spray).\n"
			"wheel_spray=%d\n\n"

			"; Physically-based 3D-only HDR Bloom (5-level Dual-Kawase) + anamorphic flares (~0.12 ms).\n"
			"; Glows ONLY on bright 3D lights (headlights, taillights, lamps, sun), 0%% bleed on chat/HUD.\n"
			"; In-game: /md bloom <0..100> (or /md bloom 0.30)\n"
			"bloom=%.2f\n"
			"; Anamorphic horizontal flares on bright headlights/sun (only when bloom > 0).\n"
			"; In-game: /md flare <0|1>\n"
			"bloom_flare=%d\n\n"

			"; Volumetric Aerial Perspective / Depth Fog (~0.04 ms, auto-off in interiors).\n"
			"; Smoothly blends GTA SA's hard draw-distance edge and distant LODs into the sky color.\n"
			"; In-game: /md fog <0..100> (or /md fog 0.25)\n"
			"fog=%.2f\n\n"

			"; Filmic tonemapping (ACES Filmic / AgX) + 3D-only local contrast Clarity (~0.02 ms).\n"
			"; Prevents blown-out skies and muddy shadows while adding material depth.\n"
			"; In-game: /md tonemap <0|1|2|off|aces|agx> (or /md tm)\n"
			";   0 - off (classic look)\n"
			";   1 - ACES Filmic (balanced cinematic look, recommended)\n"
			";   2 - AgX Punchy (modern, preserves vivid red taillights and neon)\n"
			"tonemap=%d\n"
			"; Local material micro-contrast Clarity (0.00..1.00, without oversharpening).\n"
			"; In-game: /md clarity <0..100> (or /md clarity 0.20)\n"
			"clarity=%.2f\n\n"

			"; AMD FidelityFX CAS adaptive sharpening (~0.02 ms on GPU).\n"
			"; With depth_cas=1 applies ONLY to the 3D world, leaving 2D chat, nametags, and dialogs untouched.\n"
			"; In-game: /md sharp <0..100> (or /md cas 0.35)\n"
			";   0.00       - off (default)\n"
			";   0.15..0.50 - subtle/medium sharpening (great for 4:3 and roads)\n"
			";   0.50..1.00 - strong sharpening\n"
			"sharpness=%.2f\n"
			"; Protect 2D UI (chat, dialogs, radar, ImGui) from CAS, XeGTAO, and 3D LUT.\n"
			"; In-game: /md depthcas <0|1> (or /md 2dfix)\n"
			";   1 - enabled (recommended, 100%% original 2D text)\n"
			";   0 - disabled\n"
			"depth_cas=%d\n\n"

			"; Forced D3D9 anisotropic texture filtering (crisp roads to the horizon, 0 FPS loss).\n"
			"; Not exposed in stock GTA SA settings.\n"
			"; In-game: /md af <0|2|4|8|16>\n"
			";   0 - off (stock)\n"
			";   2, 4, 8, 16 - anisotropy level (16 recommended)\n"
			"anisotropy=%d\n\n"

			"; Per-Game Digital Vibrance (color saturation ONLY inside the game window).\n"
			"; Desktop, browser, and videos keep normal colors on Alt-Tab.\n"
			"; In-game: /md vib <-100..100> (or /md vibrance 0.25)\n"
			";   0.00        - stock colors\n"
			";   0.15..0.60  - vibrant colors without clipping red HUD\n"
			";  -1.00..1.00  - full range (-1 = B&W, +1 = max)\n"
			"vibrance=%.2f\n\n"

			"; 10-bit sky/fog debanding (removes 8-bit gradient banding in ~0.01 ms without blurring textures).\n"
			"; In-game: /md deband <0|1>\n"
			";   1 - enabled (recommended)\n"
			";   0 - disabled\n"
			"deband=%d\n"
			"; 10-bit color output (DXGI_FORMAT_R10G10B10A2_UNORM, auto-fallback to 8-bit if unsupported).\n"
			"; In-game: /md 10bit <0|1>\n"
			";   1 - enabled (recommended)\n"
			";   0 - 8-bit output (B8G8R8A8)\n"
			"output_10bit=%d\n\n"

			"; 3D LUT color grading (.cube files like ReShade/Lightroom in 0.01 ms, 0 FPS loss).\n"
			"; Place any .cube file in ModernDisplay\\luts\\ and set its name (without .cube).\n"
			"; In-game: /md lut <name|off|list>\n"
			"; Empty or off - disabled.\n"
			"lut=%s\n\n"

			"; Internal SSAA supersampling (1.25x-2.0x render + crisp Catmull-Rom downscale on D3D11).\n"
			"; Cleans up fences, wires, and foliage where MSAA fails, and works alongside XeGTAO.\n"
			"; In-game: /md ssaa <1.0-2.0>\n"
			";   1.00       - native resolution (default, max FPS)\n"
			";   1.25..1.50 - clarity sweet spot (1440p on a 1080p display)\n"
			";   2.00       - 4K SSAA (maximum quality)\n"
			"render_scale=%.2f\n\n"

			"; Hardware game brightness LUT (fixes GTA SA's in-game Brightness slider).\n"
			"; Works in windowed and borderless without affecting Windows desktop gamma on Alt-Tab.\n"
			"; In-game: /md gamma <0|1>\n"
			";   1 - enabled (recommended)\n"
			";   0 - disabled\n"
			"gamma=%d\n\n"

			"[Display]\n"
			"; Scaling mode when game resolution differs from monitor (e.g. 4:3).\n"
			"; In-game: /md scale <stretch|aspect|integer>\n"
			";   stretch - stretch to fill screen (classic 4:3 SA-MP)\n"
			";   aspect  - preserve aspect ratio (black sidebars)\n"
			";   integer - pixel-exact integer scaling (no interpolation)\n"
			"scaling=%s\n\n"

			"; Minimal top-center telemetry HUD (FPS, frametime, Reflex PC Lat, iFlip).\n"
			"; In-game: /md hud (or hotkey Alt+F10)\n"
			";   0 - off\n"
			";   1 - on\n"
			"overlay=%d\n\n"

			"; Force starting the game in windowed mode.\n"
			"; In-game: /md windowed <0|1> (toggle live anytime with Alt+Enter)\n"
			";   0 - auto (follows game, Alt+Enter, SAMPFUNCS window-mode, WindowedMode.asi)\n"
			";   1 - always start in a bordered window\n"
			"windowed=%d\n\n"

			"; Virtual UI and mouse coordinate projection for windowed mode and 4:3.\n"
			"; Keeps full native render resolution without cropping while SA-MP dialogs, TAB, ImGui, and mouse clicks\n"
			"; stay pixel-accurate at any window size.\n"
			"; In-game: /md uifix <0|1>\n"
			";   1 - enabled (recommended)\n"
			";   0 - disabled (raw Win32 coordinates)\n"
			"window_ui_fix=%d\n",
			1,
			ReflexModeName(g_cfg.reflex),
			g_cfg.asyncPresent ? 1 : 0,
			g_cfg.directRenderTargets ? 1 : 0,
			g_cfg.stateCache ? 1 : 0,
			g_cfg.smartManaged ? 1 : 0,
			g_cfg.compressedMipCache ? 1 : 0,
			g_cfg.asyncScreenshot ? 1 : 0,
			fpsStr,
			g_cfg.bgFpsLimit,
			g_cfg.aoQuality,
			g_cfg.aoStrength,
			g_cfg.aaQuality,
			g_cfg.ssrQuality,
			g_cfg.giStrength,
			g_cfg.ssgiDisplayRes ? 1 : 0,
			g_cfg.rainFx ? 1 : 0,
			g_cfg.rainLighting ? 1 : 0,
			g_cfg.rainLensDrops ? 1 : 0,
			g_cfg.wheelSpray,
			g_cfg.bloomStrength,
			g_cfg.bloomFlare ? 1 : 0,
			g_cfg.fogStrength,
			g_cfg.tonemapMode,
			g_cfg.clarity,
			g_cfg.sharpness,
			g_cfg.depthCas ? 1 : 0,
			g_cfg.anisotropy,
			g_cfg.vibrance,
			g_cfg.deband ? 1 : 0,
			g_cfg.output10Bit ? 1 : 0,
			g_cfg.lutName[0] ? g_cfg.lutName : "",
			g_cfg.renderScale,
			g_cfg.gammaLut ? 1 : 0,
			ScalingModeName(g_cfg.scaling),
			g_cfg.overlay ? 1 : 0,
			g_cfg.forceWindowed ? 1 : 0,
			g_cfg.windowUiFix ? 1 : 0);
		fclose(f);
		return;
	}

	fprintf(f,
		"; ============================================================================\n"
		"; " MD_NAME " v" MD_VERSION " - (c) 2026 " MD_AUTHOR " (" MD_AUTHOR_URL ")\n"
		"; D3D9Ex -> D3D11 DXGI Flip Model + Low-Latency Suite + CAS + Frame Pacer\n"
		";\n"
		"; Все настройки ниже можно менять прямо в игре без перезапуска:\n"
		";   SA-MP:    команда /md в чате (напиши /md для справки и текущих значений)\n"
		";   Хоткеи:   Alt+Enter — переключение окно / borderless\n"
		";             Alt+F10   — вкл/выкл минималистичный HUD задержки и FPS\n"
		"; ============================================================================\n\n"

		"[Language]\n"
		"; 0 = Русский (по умолчанию), 1 = English\n"
		"; При смене на 1 при следующем запуске все пояснения в .ini перезапишутся на английский (настройки сохранятся).\n"
		"; Команда в игре: /md lang <ru|en>\n"
		"english=%d\n\n"

		"[Performance]\n"
		"; Аппаратное снижение задержки ввода (Universal Low-Latency Suite — работает на всех GPU).\n"
		"; На NVIDIA автоматически использует родной NVIDIA Reflex (nvapi.dll).\n"
		"; На AMD Radeon и Intel Arc/UHD/Xe автоматически включает Universal DX11 Low-Latency (ULL-DX11):\n"
		"; очередь кадров MaxLatency=1 + синхронизация перед опросом мыши/клавиатуры + HW-телеметрия задержки.\n"
		"; Команда в игре: /md reflex <off|on|boost>\n"
		";   off   — выключен (максимум «пустых» FPS без пейсинга задержки)\n"
		";   on    — минимальная очередь кадров и низкий input lag\n"
		";   boost — то же + повышенные частоты GPU (NVIDIA) / сверхточный pre-input sync (AMD/Intel, рекомендуется)\n"
		"reflex=%s\n\n"

		"; Асинхронный поток вывода D3D11 (двойной ping-pong буфер).\n"
		"; Выносит пост-обработку и DXGI Flip Present в отдельный поток (+20-35%% FPS в CPU-упоре).\n"
		"; Команда в игре: /md async <0|1>\n"
		";   1 — включён (рекомендуется)\n"
		";   0 — выключен (синхронный вывод в основном потоке игры, для отладки)\n"
		"async_present=%d\n"
		"; Эксперимент: общие render targets при async present без MSAA и с discard swap effect.\n"
		"direct_render_targets=%d\n\n"

		"; Глубокий SSE2-кэш состояний, матриц, света и шейдерных констант D3D9.\n"
		"; Отсекает десятки тысяч лишних вызовов RenderWare/SA-MP в драйвер за секунду (в 2-5x быстрее),\n"
		"; обрезает неизменённые шейдерные константы через SSE2 и пропускает пустые Draw с 0 примитивов.\n"
		"; Главный источник прироста FPS в толпе игроков SA-MP.\n"
		"; Команда в игре: /md cache <0|1>\n"
		";   1 — включён (рекомендуется, максимальный FPS)\n"
		";   0 — выключен (все вызовы D3D9 идут напрямую в драйвер — для отладки редких графических модов)\n"
		"state_cache=%d\n\n"

		"; Умное обновление D3DPOOL_MANAGED текстур (Dirty-Rect + пропуск D3DLOCK_READONLY).\n"
		"; Объединяет правки шрифтов чата, текстдравов и 3D-текстов в один минимальный прямоугольник\n"
		"; (в 6x быстрее при спаме в чат, 0 лишних загрузок в GPU при чтении текстур).\n"
		"; Команда в игре: /md managed <0|1>\n"
		";   1 — включено (рекомендуется, убирает микрофризы от чата и 3D-текстов)\n"
		";   0 — выключено (классическая полная заливка текстуры на каждый UnlockRect)\n"
		"smart_managed=%d\n"
		"; Кэш сжатых DXT mip-уровней. Включается при следующем запуске.\n"
		"compressed_mip_cache=%d\n\n"

		"; Мгновенные F8-скриншоты в SA-MP без фриза игры (фоновое сжатие PNG через Windows WIC).\n"
		"; Снимает кадр за <1 мс вместо 250-600 мс заморозки и сжимает PNG в фоновом потоке.\n"
		"; Команда в игре: /md shot <0|1> (или /md f8)\n"
		";   1 — включены (рекомендуется, 0 фриза при нажатии F8)\n"
		";   0 — выключены (стандартное синхронное сохранение SA-MP)\n"
		"async_screenshot=%d\n\n"

		"; Высокоточный ограничитель FPS (Win10 High-Resolution Waitable Timer + QPC spin-wait).\n"
		"; Даёт идеально ровный график фреймтайма (джиттер ~0.005 мс вместо ~0.7 мс у обычного Sleep).\n"
		"; Команда в игре: /md fps <off|auto|20..1000>\n"
		";   0       — выключен (без ограничения FPS)\n"
		";   auto    — авто-лимит (герцовка монитора - 3, например 141 на 144 Гц) — идеально для G-Sync/FreeSync\n"
		";   20..1000 — точный лимит кадров (например: 144, 200, 240, 300)\n"
		"fps_limit=%s\n\n"

		"; Лимит FPS в фоне (когда игра свёрнута или кликнул в другое окно / Discord / браузер).\n"
		"; Видеокарта остывает и не шумит в фоне, при этом SA-MP не ставится на паузу.\n"
		"; Команда в игре: /md bgfps <0|10..240>\n"
		";   0       — выключен\n"
		";   10..240 — целевой FPS в фоне (рекомендуется 30)\n"
		"bg_fps_limit=%d\n\n"

		"[Graphics]\n"
		"; Встроенный D3D11 XeGTAO (Ambient Occlusion на NVIDIA, AMD и Intel без NVIDIA Inspector).\n"
		"; Считает контактные тени по реальному Z-буферу игры и вообще не просвечивает сквозь чат и HUD.\n"
		"; Примечание: для чтения Z-буфера в D3D9 требуется MSAA=0 (при ao>0 MSAA автоматически отключается).\n"
		"; Команда в игре: /md ao <0|1|2|3|off|low|med|high>\n"
		";   0 — выключен (по умолчанию, сохраняет родной MSAA)\n"
		";   1 — Low (быстрый, ~0.15 мс)\n"
		";   2 — Medium (оптимальный баланс, ~0.25 мс)\n"
		";   3 — High (максимум деталей, ~0.40 мс)\n"
		"ao=%d\n"
		"; Сила затемнения XeGTAO (0.10..2.00, по умолчанию 0.85).\n"
		"; Команда в игре: /md aostr <0.1-2.0> (или /md ao 1.2)\n"
		"ao_strength=%.2f\n\n"

		"; 3D-сглаживание SMAA 1x / Temporal AA (SMAA T2x) — убирает лесенки и мерцание проводов/рельс без мыла.\n"
		"; Работает строго по 3D-маске и вообще не трогает чат, ники и HUD SA-MP.\n"
		"; Команда в игре: /md aa <0|1|2|3|off|med|high|taa> (или /md taa)\n"
		";   0 — выключено\n"
		";   1 — SMAA 1x Medium (~0.08 мс)\n"
		";   2 — SMAA 1x High + Depth/AO Edge (~0.12 мс)\n"
		";   3 — SMAA T2x (Temporal AA по матрицам камеры GTA SA, убирает мерцание в движении, ~0.18 мс)\n"
		"aa=%d\n\n"

		"; Отражения в экранном пространстве SSR (вода и мокрый асфальт в дождь, ~0.25 мс, 3D-only).\n"
		"; Читает CWeather::Rain и CWeather::WetRoads из памяти GTA SA — в дождь дорога и лужи отражают фонари, машины и здания.\n"
		"; Команда в игре: /md ssr <0|1|2|off|med|high>\n"
		";   0 — выключено\n"
		";   1 — Medium (вода + мокрый асфальт в дождь, 20 шагов)\n"
		";   2 — High (максимум чёткости и дальности отражений, 32 шага)\n"
		"ssr=%d\n\n"

		"; Глобальное освещение в экранном пространстве Temporal SSGI (живой отскок цвета от стен, фар и неона, ~0.22 мс).\n"
		"; Красные стоп-сигналы, фонари, вывески и закатное солнце мягко подсвечивают асфальт, машины и персонажа (3D-only).\n"
		"; Команда в игре: /md gi <0..100> (или /md ssgi 0.35)\n"
		"gi=%.2f\n"
		"; 1 — вычислять SSGI от разрешения экрана при SSAA (экономит время GPU).\n"
		"ssgi_display_res=%d\n\n"

		"; Дождь: отражения в лужах на дорогах при включённом SSR, рябь и капли по краям экрана.\n"
		"; Автоматически выключается в интерьерах.\n"
		"; Команда в игре: /md rainfx <0|1> (или /md drops)\n"
		"rain_fx=%d\n"
		"; Освещение погоды 8 и 16 по времени суток. Команда /md rainlight.\n"
		"rain_lighting=%d\n"
		"; Капли на объективе отдельно: /md raindrops <0|1>\n"
		"rain_lens_drops=%d\n\n"
		"; Брызги колёс: 0 — выкл, 1 — оригинал, 2 — меньше брызг при проезде по воде (/md spray).\n"
		"wheel_spray=%d\n\n"

		"; Физический 3D-only HDR Bloom (5-уровневый Dual-Kawase) + анаморфные блики фар/солнца (~0.12 мс).\n"
		"; Подсвечивает ТОЛЬКО яркие огни 3D-мира (фары, стопы, фонари, солнце), 0%% свечения на чате и HUD.\n"
		"; Команда в игре: /md bloom <0..100> (или /md bloom 0.30)\n"
		"bloom=%.2f\n"
		"; Анаморфные горизонтальные полосы от ярких фар и солнца (только при bloom > 0).\n"
		"; Команда в игре: /md flare <0|1>\n"
		"bloom_flare=%d\n\n"

		"; Объёмная воздушная дымка / Aerial Perspective по Z-буферу (~0.04 мс, авто-выкл в интерьерах).\n"
		"; Мягко растворяет резкую границу прорисовки GTA SA и дальние LOD-здания в живой цвет неба.\n"
		"; Команда в игре: /md fog <0..100> (или /md fog 0.25)\n"
		"fog=%.2f\n\n"

		"; Киношный тонмаппинг (ACES Filmic / AgX) + локальный контраст Clarity (3D-only, ~0.02 мс).\n"
		"; Небо не выбивается в плоский белый, тени не проваливаются в грязь, материалы получают объём.\n"
		"; Команда в игре: /md tonemap <0|1|2|off|aces|agx> (или /md tm)\n"
		";   0 — выключен (классический вид)\n"
		";   1 — ACES Filmic (киношный баланс, рекомендуется)\n"
		";   2 — AgX Punchy (современный, сохраняет сочные красные стоп-сигналы и неон)\n"
		"tonemap=%d\n"
		"; Локальный микроконтраст материалов Clarity (0.00..1.00, без перешарпа и лесенок).\n"
		"; Команда в игре: /md clarity <0..100> (или /md clarity 0.20)\n"
		"clarity=%.2f\n\n"

		"; Адаптивная резкость AMD FidelityFX CAS (выполняется на GPU за ~0.02 мс).\n"
		"; При depth_cas=1 применяется ТОЛЬКО к 3D-миру, вообще не трогая 2D-чат, ники и диалоги SA-MP.\n"
		"; Команда в игре: /md sharp <0..100> (или /md cas 0.35)\n"
		";   0.00       — выключена (по умолчанию)\n"
		";   0.15..0.50 — мягкая/средняя резкость (хорошо для 4:3 и дорог)\n"
		";   0.50..1.00 — сильная резкость\n"
		"sharpness=%.2f\n"
		"; Защита 2D-интерфейса (чат, диалоги, радар, ImGui) от CAS, XeGTAO и 3D LUT.\n"
		"; Команда в игре: /md depthcas <0|1> (или /md 2dfix)\n"
		";   1 — включена (рекомендуется, 2D-текст на 100%% оригинальный)\n"
		";   0 — выключена\n"
		"depth_cas=%d\n\n"

		"; Принудительная анизотропная фильтрация текстур D3D9 (дороги и земля чёткие до горизонта, 0 потерь FPS).\n"
		"; В стандартных настройках GTA SA её нет вообще.\n"
		"; Команда в игре: /md af <0|2|4|8|16>\n"
		";   0  — выключена (стандартная игра)\n"
		";   2, 4, 8, 16 — уровень анизотропной фильтрации (рекомендуется 16)\n"
		"anisotropy=%d\n\n"

		"; Per-Game Digital Vibrance (насыщенность цветов ТОЛЬКО внутри окна игры).\n"
		"; При Alt-Tab рабочий стол, браузер и видео остаются с обычными цветами.\n"
		"; Команда в игре: /md vib <-100..100> (или /md vibrance 0.25)\n"
		";   0.00        — стандартные цвета игры\n"
		";   0.15..0.60  — сочные цвета без пережарки красного HUD\n"
		";  -1.00..1.00  — полный диапазон (-1 = ч/б, +1 = максимум)\n"
		"vibrance=%.2f\n\n"

		"; 10-bit Debanding неба и тумана (убирает 8-битные ступеньки градиента за ~0.01 мс, не мылит текстуры).\n"
		"; Команда в игре: /md deband <0|1>\n"
		";   1 — включён (рекомендуется)\n"
		";   0 — выключен\n"
		"deband=%d\n"
		"; 10-битный вывод цвета (DXGI_FORMAT_R10G10B10A2_UNORM, авто-откат на 8-bit при отсутствии поддержки).\n"
		"; Команда в игре: /md 10bit <0|1>\n"
		";   1 — включён (рекомендуется)\n"
		";   0 — 8-битный вывод (B8G8R8A8)\n"
		"output_10bit=%d\n\n"

		"; 3D LUT цветокоррекция (.cube файлы как в ReShade/Lightroom за 0.01 мс, 0 потерь FPS).\n"
		"; Положи любой .cube файл в папку ModernDisplay\\luts\\ и укажи имя (без .cube).\n"
		"; Команда в игре: /md lut <имя|off|list>\n"
		"; Пустое значение или off — выключено.\n"
		"lut=%s\n\n"

		"; Суперсэмплинг SSAA (внутренний рендер 1.25x-2.0x + чёткий Catmull-Rom даунскейл на D3D11).\n"
		"; Убирает лесенки даже на заборах, проводах и листве (где MSAA бессилен) и работает вместе с XeGTAO.\n"
		"; Команда в игре: /md ssaa <1.0-2.0>\n"
		";   1.00       — родное разрешение (по умолчанию, максимум FPS)\n"
		";   1.25..1.50 — золотая середина чёткости (1440p на 1080p мониторе)\n"
		";   2.00       — 4K SSAA (максимальное качество)\n"
		"render_scale=%.2f\n\n"

		"; Аппаратный LUT яркости игры (чинит ползунок «Яркость» в настройках GTA SA).\n"
		"; Работает в окне и borderless, не ломая гамму рабочего стола Windows при Alt-Tab.\n"
		"; Команда в игре: /md gamma <0|1>\n"
		";   1 — включён (рекомендуется)\n"
		";   0 — выключен\n"
		"gamma=%d\n\n"

		"[Display]\n"
		"; Режим масштабирования картинки, если разрешение игры отличается от монитора (например 4:3).\n"
		"; Команда в игре: /md scale <stretch|aspect|integer>\n"
		";   stretch — растянуть на весь экран (классика для 4:3 в SA-MP)\n"
		";   aspect  — сохранить пропорции (чёрные полосы по бокам)\n"
		";   integer — строго пиксель-в-пиксель (максимальная чёткость без интерполяции)\n"
		"scaling=%s\n\n"

		"; Минималистичный HUD телеметрии по центру вверху (FPS, фреймтайм, Reflex PC Lat, iFlip).\n"
		"; Команда в игре: /md hud (или хоткей Alt+F10)\n"
		";   0 — выключен\n"
		";   1 — включён\n"
		"overlay=%d\n\n"

		"; Принудительный запуск игры в оконном режиме.\n"
		"; Команда в игре: /md windowed <0|1> (переключить окно/полный экран прямо сейчас: Alt+Enter)\n"
		";   0 — авто (следует за игрой, Alt+Enter, SAMPFUNCS window-mode и WindowedMode.asi)\n"
		";   1 — всегда открывать игру сразу в окне с рамкой\n"
		"windowed=%d\n\n"

		"; Виртуальная проекция координат UI и мыши для оконного режима и 4:3.\n"
		"; Держит игру в полном родном разрешении без обрезки, а диалоги SA-MP, TAB, ImGui и клики мышью\n"
		"; идеально попадают в пиксель при любом размере окна (включая разворот на весь экран и 4:3).\n"
		"; Команда в игре: /md uifix <0|1>\n"
		";   1 — включена (рекомендуется)\n"
		";   0 — выключена (нативные Win32-координаты без коррекции)\n"
		"window_ui_fix=%d\n",
		0,
		ReflexModeName(g_cfg.reflex),
		g_cfg.asyncPresent ? 1 : 0,
		g_cfg.directRenderTargets ? 1 : 0,
		g_cfg.stateCache ? 1 : 0,
		g_cfg.smartManaged ? 1 : 0,
		g_cfg.compressedMipCache ? 1 : 0,
		g_cfg.asyncScreenshot ? 1 : 0,
		fpsStr,
		g_cfg.bgFpsLimit,
		g_cfg.aoQuality,
		g_cfg.aoStrength,
		g_cfg.aaQuality,
		g_cfg.ssrQuality,
		g_cfg.giStrength,
		g_cfg.ssgiDisplayRes ? 1 : 0,
		g_cfg.rainFx ? 1 : 0,
		g_cfg.rainLighting ? 1 : 0,
		g_cfg.rainLensDrops ? 1 : 0,
		g_cfg.wheelSpray,
		g_cfg.bloomStrength,
		g_cfg.bloomFlare ? 1 : 0,
		g_cfg.fogStrength,
		g_cfg.tonemapMode,
		g_cfg.clarity,
		g_cfg.sharpness,
		g_cfg.depthCas ? 1 : 0,
		g_cfg.anisotropy,
		g_cfg.vibrance,
		g_cfg.deband ? 1 : 0,
		g_cfg.output10Bit ? 1 : 0,
		g_cfg.lutName[0] ? g_cfg.lutName : "",
		g_cfg.renderScale,
		g_cfg.gammaLut ? 1 : 0,
		ScalingModeName(g_cfg.scaling),
		g_cfg.overlay ? 1 : 0,
		g_cfg.forceWindowed ? 1 : 0,
		g_cfg.windowUiFix ? 1 : 0);

	fclose(f);
}

void ConfigLoad()
{
	char path[MAX_PATH];
	BuildIniPath(path, sizeof(path));

	FILE *f = nullptr;
	if (fopen_s(&f, path, "r") != 0 || !f) {
		ConfigSave();
		ConfigNotifyChanged();
		return;
	}

	char line[256];
	bool firstLine = true;
	bool hasLang = false, sawRuComments = false, sawEnComments = false;
	bool hasStateCache = false, hasSmartManaged = false, hasAsyncShot = false, hasWinUiFix = false, hasLegacyKey = false;
	bool hasAo = false, hasAa = false, hasAf = false, hasDeband = false, hasScale = false;
	bool hasSsr = false, hasGi = false, hasRainFx = false, hasRainLighting = false, hasBloom = false, hasFog = false, hasTonemap = false, hasClarity = false;
	while (fgets(line, sizeof(line), f)) {
		if (!sawRuComments && strstr(line, "Все настройки ниже")) sawRuComments = true;
		if (!sawEnComments && strstr(line, "All settings below")) sawEnComments = true;
		char *s = line;
		if (firstLine) {
			firstLine = false;
			// Skip UTF-8 BOM if present
			if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
				s += 3;
		}
		if (char *c = strchr(s, ';')) *c = 0;
		if (char *c = strchr(s, '#')) *c = 0;
		TrimStr(s);
		if (!s[0] || s[0] == '[') continue;

		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = s;
		char *v = eq + 1;
		TrimStr(k);
		TrimStr(v);
		if (!k[0] || !v[0]) continue;

		if (_stricmp(k, "english") == 0) {
			g_cfg.english = (atoi(v) != 0);
			hasLang = true;
		} else if (_stricmp(k, "reflex") == 0 || _stricmp(k, "low_latency") == 0) {
			if (_stricmp(v, "off") == 0 || strcmp(v, "0") == 0)       g_cfg.reflex = ReflexMode::Off;
			else if (_stricmp(v, "on") == 0 || strcmp(v, "1") == 0)  g_cfg.reflex = ReflexMode::On;
			else                                                     g_cfg.reflex = ReflexMode::Boost;
		} else if (_stricmp(k, "async_present") == 0) {
			g_cfg.asyncPresent = (atoi(v) != 0);
		} else if (_stricmp(k, "direct_render_targets") == 0) {
			g_cfg.directRenderTargets = (atoi(v) != 0);
		} else if (_stricmp(k, "state_cache") == 0) {
			g_cfg.stateCache = (atoi(v) != 0);
			hasStateCache = true;
		} else if (_stricmp(k, "smart_managed") == 0) {
			g_cfg.smartManaged = (atoi(v) != 0);
			hasSmartManaged = true;
		} else if (_stricmp(k, "compressed_mip_cache") == 0) {
			g_cfg.compressedMipCache = (atoi(v) != 0);
		} else if (_stricmp(k, "async_screenshot") == 0) {
			g_cfg.asyncScreenshot = (atoi(v) != 0);
			hasAsyncShot = true;
		} else if (_stricmp(k, "zero_copy") == 0) {
			hasLegacyKey = true;
		} else if (_stricmp(k, "ao") == 0) {
			if (_stricmp(v, "low") == 0)       g_cfg.aoQuality = 1;
			else if (_stricmp(v, "med") == 0 || _stricmp(v, "medium") == 0) g_cfg.aoQuality = 2;
			else if (_stricmp(v, "high") == 0) g_cfg.aoQuality = 3;
			else                               g_cfg.aoQuality = ClampI(atoi(v), 0, 3);
			hasAo = true;
		} else if (_stricmp(k, "ao_strength") == 0) {
			g_cfg.aoStrength = ClampF((float)atof(v), 0.1f, 2.0f);
		} else if (_stricmp(k, "aa") == 0 || _stricmp(k, "smaa") == 0 || _stricmp(k, "taa") == 0) {
			if (_stricmp(v, "off") == 0) g_cfg.aaQuality = 0;
			else if (_stricmp(v, "med") == 0 || _stricmp(v, "medium") == 0 || _stricmp(v, "on") == 0) g_cfg.aaQuality = 1;
			else if (_stricmp(v, "high") == 0) g_cfg.aaQuality = 2;
			else if (_stricmp(v, "taa") == 0 || _stricmp(v, "t2x") == 0 || _stricmp(v, "ultra") == 0) g_cfg.aaQuality = 3;
			else g_cfg.aaQuality = ClampI(atoi(v), 0, 3);
			hasAa = true;
		} else if (_stricmp(k, "ssr") == 0) {
			if (_stricmp(v, "off") == 0) g_cfg.ssrQuality = 0;
			else if (_stricmp(v, "med") == 0 || _stricmp(v, "medium") == 0 || _stricmp(v, "on") == 0) g_cfg.ssrQuality = 1;
			else if (_stricmp(v, "high") == 0 || _stricmp(v, "ultra") == 0) g_cfg.ssrQuality = 2;
			else g_cfg.ssrQuality = ClampI(atoi(v), 0, 2);
			hasSsr = true;
		} else if (_stricmp(k, "gi") == 0 || _stricmp(k, "ssgi") == 0) {
			float g = (float)atof(v);
			if (g > 1.0f) g *= 0.01f;
			g_cfg.giStrength = ClampF(g, 0.0f, 1.0f);
			hasGi = true;
		} else if (_stricmp(k, "ssgi_display_res") == 0) {
			g_cfg.ssgiDisplayRes = atoi(v) != 0;
		} else if (_stricmp(k, "rain_fx") == 0 || _stricmp(k, "rainfx") == 0 || _stricmp(k, "drops") == 0) {
			g_cfg.rainFx = (atoi(v) != 0);
			hasRainFx = true;
		} else if (_stricmp(k, "rain_lighting") == 0) {
			g_cfg.rainLighting = atoi(v) != 0;
			hasRainLighting = true;
		} else if (_stricmp(k, "rain_lens_drops") == 0) {
			g_cfg.rainLensDrops = atoi(v) != 0;
		} else if (_stricmp(k, "wheel_spray") == 0) {
			g_cfg.wheelSpray = ClampI(atoi(v), 0, 2);
		} else if (_stricmp(k, "bloom") == 0) {
			float bl = (float)atof(v);
			if (bl > 1.0f) bl *= 0.01f;
			g_cfg.bloomStrength = ClampF(bl, 0.0f, 1.0f);
			hasBloom = true;
		} else if (_stricmp(k, "bloom_flare") == 0 || _stricmp(k, "flare") == 0) {
			g_cfg.bloomFlare = (atoi(v) != 0);
		} else if (_stricmp(k, "fog") == 0) {
			float fg = (float)atof(v);
			if (fg > 1.0f) fg *= 0.01f;
			g_cfg.fogStrength = ClampF(fg, 0.0f, 1.0f);
			hasFog = true;
		} else if (_stricmp(k, "tonemap") == 0 || _stricmp(k, "tm") == 0) {
			if (_stricmp(v, "off") == 0) g_cfg.tonemapMode = 0;
			else if (_stricmp(v, "aces") == 0 || _stricmp(v, "on") == 0) g_cfg.tonemapMode = 1;
			else if (_stricmp(v, "agx") == 0) g_cfg.tonemapMode = 2;
			else g_cfg.tonemapMode = ClampI(atoi(v), 0, 2);
			hasTonemap = true;
		} else if (_stricmp(k, "clarity") == 0) {
			float cl = (float)atof(v);
			if (cl > 1.0f) cl *= 0.01f;
			g_cfg.clarity = ClampF(cl, 0.0f, 1.0f);
			hasClarity = true;
		} else if (_stricmp(k, "depth_cas") == 0) {
			g_cfg.depthCas = (atoi(v) != 0);
		} else if (_stricmp(k, "anisotropy") == 0 || _stricmp(k, "af") == 0) {
			int af = atoi(v);
			if (af <= 0) g_cfg.anisotropy = 0;
			else if (af <= 2) g_cfg.anisotropy = 2;
			else if (af <= 4) g_cfg.anisotropy = 4;
			else if (af <= 8) g_cfg.anisotropy = 8;
			else g_cfg.anisotropy = 16;
			hasAf = true;
		} else if (_stricmp(k, "deband") == 0) {
			g_cfg.deband = (atoi(v) != 0);
			hasDeband = true;
		} else if (_stricmp(k, "output_10bit") == 0) {
			g_cfg.output10Bit = (atoi(v) != 0);
		} else if (_stricmp(k, "lut") == 0) {
			if (_stricmp(v, "off") == 0 || strcmp(v, "0") == 0) g_cfg.lutName[0] = 0;
			else strncpy_s(g_cfg.lutName, v, _TRUNCATE);
		} else if (_stricmp(k, "render_scale") == 0 || _stricmp(k, "ssaa") == 0) {
			g_cfg.renderScale = ClampF((float)atof(v), 1.0f, 2.0f);
			hasScale = true;
		} else if (_stricmp(k, "fps_limit") == 0) {
			if (_stricmp(v, "auto") == 0 || _stricmp(v, "vrr") == 0 || atoi(v) < 0) {
				g_cfg.fpsLimit = -1;
			} else {
				int n = atoi(v);
				g_cfg.fpsLimit = (n <= 0) ? 0 : ClampI(n, 20, 1000);
			}
		} else if (_stricmp(k, "bg_fps_limit") == 0) {
			int n = atoi(v);
			g_cfg.bgFpsLimit = (n <= 0) ? 0 : ClampI(n, 10, 240);
		} else if (_stricmp(k, "sharpness") == 0) {
			float sh = (float)atof(v);
			if (sh > 1.0f) sh *= 0.01f;
			g_cfg.sharpness = ClampF(sh, 0.0f, 1.0f);
		} else if (_stricmp(k, "vibrance") == 0) {
			float vb = (float)atof(v);
			if (vb > 1.0f || vb < -1.0f) vb *= 0.01f;
			g_cfg.vibrance = ClampF(vb, -1.0f, 1.0f);
		} else if (_stricmp(k, "fxaa") == 0) {
			hasLegacyKey = true;
		} else if (_stricmp(k, "scaling") == 0) {
			if (_stricmp(v, "aspect") == 0 || strcmp(v, "1") == 0)       g_cfg.scaling = ScalingMode::Aspect;
			else if (_stricmp(v, "integer") == 0 || strcmp(v, "2") == 0) g_cfg.scaling = ScalingMode::Integer;
			else                                                         g_cfg.scaling = ScalingMode::Stretch;
		} else if (_stricmp(k, "gamma") == 0) {
			g_cfg.gammaLut = (atoi(v) != 0);
		} else if (_stricmp(k, "overlay") == 0) {
			g_cfg.overlay = (atoi(v) != 0);
		} else if (_stricmp(k, "windowed") == 0) {
			g_cfg.forceWindowed = (atoi(v) != 0);
		} else if (_stricmp(k, "window_ui_fix") == 0) {
			g_cfg.windowUiFix = (atoi(v) != 0);
			hasWinUiFix = true;
		}
	}
	fclose(f);
	// Auto-upgrade older INIs or rewrite comments when language changed
	bool langMismatch = (g_cfg.english && !sawEnComments) || (!g_cfg.english && !sawRuComments);
	if (!hasRainLighting) g_cfg.rainLighting = g_cfg.rainFx;
	if (!hasLang || langMismatch || !hasStateCache || !hasSmartManaged || !hasAsyncShot || !hasWinUiFix || hasLegacyKey ||
	    !hasAo || !hasAa || !hasAf || !hasDeband || !hasScale ||
	    !hasSsr || !hasGi || !hasRainFx || !hasRainLighting || !hasBloom || !hasFog || !hasTonemap || !hasClarity)
		ConfigSave();
	ConfigNotifyChanged();

	char fpsStr[16];
	FormatFpsLimit(g_cfg.fpsLimit, fpsStr, sizeof(fpsStr));
	Log("Config: reflex=%s, async=%d, cache=%d, managed=%d, shot=%d, fps=%s, bgfps=%d, ao=%d(%.2f), aa=%d, ssr=%d, gi=%.2f, rainfx=%d, bloom=%.2f(flare=%d), fog=%.2f, tm=%d, clarity=%.2f, cas=%.2f(3d=%d), af=%dx, vib=%.2f, deband=%d, 10b=%d, lut='%s', ssaa=%.2f, scale=%s, gamma=%d, hud=%d, win=%d",
		ReflexModeName(g_cfg.reflex), g_cfg.asyncPresent ? 1 : 0, g_cfg.stateCache ? 1 : 0, g_cfg.smartManaged ? 1 : 0,
		g_cfg.asyncScreenshot ? 1 : 0, fpsStr, g_cfg.bgFpsLimit,
		g_cfg.aoQuality, g_cfg.aoStrength, g_cfg.aaQuality, g_cfg.ssrQuality,
		g_cfg.giStrength, g_cfg.rainFx ? 1 : 0,
		g_cfg.bloomStrength, g_cfg.bloomFlare ? 1 : 0, g_cfg.fogStrength, g_cfg.tonemapMode, g_cfg.clarity,
		g_cfg.sharpness, g_cfg.depthCas ? 1 : 0, g_cfg.anisotropy,
		g_cfg.vibrance, g_cfg.deband ? 1 : 0, g_cfg.output10Bit ? 1 : 0, g_cfg.lutName, g_cfg.renderScale,
		ScalingModeName(g_cfg.scaling), g_cfg.gammaLut ? 1 : 0, g_cfg.overlay ? 1 : 0,
		g_cfg.forceWindowed ? 1 : 0);
}
