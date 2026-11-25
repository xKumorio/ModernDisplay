// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "async_screenshot.h"
#include "../core/config.h"
#include "../core/log.h"
#include "../core/patch.h"

#include <d3d9.h>
#include <wincodec.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include <string>

namespace {

constexpr DWORD kD3DXIFF_PNG = 3;
constexpr int   kMaxQueueJobs = 8;

typedef HRESULT(WINAPI *D3DXSaveSurfaceToFileA_t)(
	LPCSTR pDestFile,
	DWORD DestFormat,
	IDirect3DSurface9 *pSrcSurface,
	CONST PALETTEENTRY *pSrcPalette,
	CONST RECT *pSrcRect);

struct ShotJob {
	std::string path;
	BYTE       *pixels = nullptr;
	UINT        width = 0;
	UINT        height = 0;
};

D3DXSaveSurfaceToFileA_t g_origSaveSurface = nullptr;
bool                     g_hooked = false;
bool                     g_csInit = false;
CRITICAL_SECTION         g_cs = {};
CONDITION_VARIABLE       g_cvWork = {};
CONDITION_VARIABLE       g_cvEmpty = {};
HANDLE                   g_thread = nullptr;
bool                     g_stop = false;
ShotJob                  g_queue[kMaxQueueJobs] = {};
int                      g_head = 0;
int                      g_tail = 0;
int                      g_count = 0;
bool                     g_busy = false;
volatile LONG            g_wicUnavailable = 0;
int                      g_errLogs = 0;

bool SavePngWithWic(IWICImagingFactory *factory, const ShotJob &job)
{
	if (!factory || !job.pixels || job.width == 0 || job.height == 0 || job.path.empty())
		return false;

	wchar_t wpath[MAX_PATH * 2] = {};
	int wn = MultiByteToWideChar(CP_ACP, 0, job.path.c_str(), -1, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])));
	if (wn <= 1) return false;

	IWICStream *stream = nullptr;
	IWICBitmapEncoder *encoder = nullptr;
	IWICBitmapFrameEncode *frame = nullptr;
	IWICBitmap *srcBmp = nullptr;
	bool ok = false;

	do {
		if (FAILED(factory->CreateStream(&stream)) || !stream) break;
		if (FAILED(stream->InitializeFromFilename(wpath, GENERIC_WRITE))) break;
		if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || !encoder) break;
		if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
		if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || !frame) break;
		if (FAILED(frame->Initialize(nullptr))) break;
		if (FAILED(frame->SetSize(job.width, job.height))) break;

		// Write opaque 24-bit BGR PNG from 32-bit BGRX buffer (faster, ~25% smaller, avoids zero-alpha artifacts)
		WICPixelFormatGUID dstFmt = GUID_WICPixelFormat24bppBGR;
		if (FAILED(frame->SetPixelFormat(&dstFmt))) break;

		const UINT stride = job.width * 4u;
		const UINT bufSize = stride * job.height;
		if (FAILED(factory->CreateBitmapFromMemory(job.width, job.height, GUID_WICPixelFormat32bppBGR,
		                                           stride, bufSize, job.pixels, &srcBmp)) || !srcBmp)
			break;

		if (FAILED(frame->WriteSource(srcBmp, nullptr))) break;
		if (FAILED(frame->Commit())) break;
		if (FAILED(encoder->Commit())) break;
		ok = true;
	} while (false);

	if (srcBmp)  srcBmp->Release();
	if (frame)   frame->Release();
	if (encoder) encoder->Release();
	if (stream)  stream->Release();
	return ok;
}

DWORD WINAPI ScreenshotWorkerProc(LPVOID)
{
	typedef HRESULT(WINAPI *CoInitializeEx_t)(LPVOID, DWORD);
	typedef HRESULT(WINAPI *CoCreateInstance_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
	typedef void(WINAPI *CoUninitialize_t)();

	HMODULE ole32 = LoadLibraryA("ole32.dll");
	auto pCoInit   = ole32 ? (CoInitializeEx_t)GetProcAddress(ole32, "CoInitializeEx") : nullptr;
	auto pCoCreate = ole32 ? (CoCreateInstance_t)GetProcAddress(ole32, "CoCreateInstance") : nullptr;
	auto pCoUninit = ole32 ? (CoUninitialize_t)GetProcAddress(ole32, "CoUninitialize") : nullptr;

	HRESULT comHr = pCoInit ? pCoInit(nullptr, COINIT_MULTITHREADED) : E_FAIL;
	IWICImagingFactory *factory = nullptr;
	if (pCoCreate && (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE)) {
		pCoCreate(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		          __uuidof(IWICImagingFactory), (void **)&factory);
	}
	if (!factory) {
		InterlockedExchange(&g_wicUnavailable, 1);
		Log("Screenshot: WIC factory unavailable - falling back to sync D3DX PNG");
	}

	for (;;) {
		ShotJob job;
		EnterCriticalSection(&g_cs);
		while (g_count == 0 && !g_stop)
			SleepConditionVariableCS(&g_cvWork, &g_cs, INFINITE);
		if (g_count == 0 && g_stop) {
			LeaveCriticalSection(&g_cs);
			break;
		}
		job = std::move(g_queue[g_head]);
		g_queue[g_head] = {};
		g_head = (g_head + 1) % kMaxQueueJobs;
		--g_count;
		g_busy = true;
		LeaveCriticalSection(&g_cs);

		if (!SavePngWithWic(factory, job) && g_errLogs < 4) {
			++g_errLogs;
			Log("Screenshot: WIC PNG save failed for '%s'", job.path.c_str());
		}
		delete[] job.pixels;

		EnterCriticalSection(&g_cs);
		g_busy = false;
		if (g_count == 0)
			WakeAllConditionVariable(&g_cvEmpty);
		LeaveCriticalSection(&g_cs);
	}

	if (factory) factory->Release();
	if (SUCCEEDED(comHr) && pCoUninit) pCoUninit();
	if (ole32) FreeLibrary(ole32);
	return 0;
}

void EnsureWorkerStarted()
{
	if (!g_csInit) {
		InitializeCriticalSection(&g_cs);
		InitializeConditionVariable(&g_cvWork);
		InitializeConditionVariable(&g_cvEmpty);
		g_csInit = true;
	}
	if (!g_thread) {
		g_stop = false;
		g_thread = CreateThread(nullptr, 0, ScreenshotWorkerProc, nullptr, 0, nullptr);
		if (g_thread)
			SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
	}
}

HRESULT WINAPI hkD3DXSaveSurfaceToFileA(
	LPCSTR pDestFile,
	DWORD DestFormat,
	IDirect3DSurface9 *pSrcSurface,
	CONST PALETTEENTRY *pSrcPalette,
	CONST RECT *pSrcRect)
{
	if (g_cfg.asyncScreenshot && !g_wicUnavailable && DestFormat == kD3DXIFF_PNG &&
	    pDestFile && pDestFile[0] && pSrcSurface && !pSrcPalette) {
		D3DSURFACE_DESC desc = {};
		if (SUCCEEDED(pSrcSurface->GetDesc(&desc)) &&
		    (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8) &&
		    desc.Width > 0 && desc.Height > 0 && desc.Width <= 8192 && desc.Height <= 8192) {
			LONG x0 = 0, y0 = 0, x1 = (LONG)desc.Width, y1 = (LONG)desc.Height;
			if (pSrcRect) {
				if (pSrcRect->left   > x0) x0 = pSrcRect->left;
				if (pSrcRect->top    > y0) y0 = pSrcRect->top;
				if (pSrcRect->right  < x1) x1 = pSrcRect->right;
				if (pSrcRect->bottom < y1) y1 = pSrcRect->bottom;
			}
			if (x1 > x0 && y1 > y0) {
				const UINT cropW = (UINT)(x1 - x0);
				const UINT cropH = (UINT)(y1 - y0);
				D3DLOCKED_RECT lr = {};
				if (SUCCEEDED(pSrcSurface->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
					BYTE *copy = nullptr;
					if (lr.pBits && lr.Pitch >= (INT)(desc.Width * 4u)) {
						EnsureWorkerStarted();
						const size_t rowBytes = (size_t)cropW * 4u;
						const size_t totalBytes = rowBytes * (size_t)cropH;
						copy = new (std::nothrow) BYTE[totalBytes];
						if (copy) {
							const BYTE *srcBase = (const BYTE *)lr.pBits + (size_t)y0 * (size_t)lr.Pitch + (size_t)x0 * 4u;
							if (x0 == 0 && cropW == desc.Width && (size_t)lr.Pitch == rowBytes) {
								memcpy(copy, srcBase, totalBytes);
							} else {
								const size_t srcPitch = (size_t)lr.Pitch;
								for (UINT y = 0; y < cropH; ++y)
									memcpy(copy + (size_t)y * rowBytes, srcBase + (size_t)y * srcPitch, rowBytes);
							}
						}
					}
					pSrcSurface->UnlockRect();

					if (copy) {
						bool queued = false;
						if (g_thread) {
							EnterCriticalSection(&g_cs);
							if (g_count < kMaxQueueJobs) {
								g_queue[g_tail] = { std::string(pDestFile), copy, cropW, cropH };
								g_tail = (g_tail + 1) % kMaxQueueJobs;
								++g_count;
								queued = true;
								WakeConditionVariable(&g_cvWork);
							}
							LeaveCriticalSection(&g_cs);
						}
						if (queued)
							return S_OK;
						delete[] copy;
					}
				}
			}
		}
	}
	return g_origSaveSurface ? g_origSaveSurface(pDestFile, DestFormat, pSrcSurface, pSrcPalette, pSrcRect) : E_FAIL;
}

} // namespace

void TryInstallAsyncScreenshotHook()
{
	if (g_hooked) return;
	HMODULE mod = GetModuleHandleA("d3dx9_25.dll");
	if (!mod) return;

	void *fn = (void *)GetProcAddress(mod, "D3DXSaveSurfaceToFileA");
	if (!fn) return;

	void *tramp = nullptr;
	if (HookHotpatchFn(fn, (void *)hkD3DXSaveSurfaceToFileA, &tramp) ||
	    HookInlineFn(fn, (void *)hkD3DXSaveSurfaceToFileA, &tramp)) {
		g_origSaveSurface = (D3DXSaveSurfaceToFileA_t)tramp;
		g_hooked = true;
		if (g_cfg.asyncScreenshot)
			EnsureWorkerStarted();
		Log("Screenshot: async WIC PNG hook ready (d3dx9_25!D3DXSaveSurfaceToFileA)");
	} else {
		g_hooked = true; // Avoid retrying every frame if already hooked by another mod
	}
}

void AsyncScreenshotFlush()
{
	if (!g_csInit || !g_thread) return;
	EnterCriticalSection(&g_cs);
	DWORD deadline = GetTickCount() + 4000;
	while ((g_count > 0 || g_busy) && (int)(deadline - GetTickCount()) > 0)
		SleepConditionVariableCS(&g_cvEmpty, &g_cs, 100);
	g_stop = true;
	WakeAllConditionVariable(&g_cvWork);
	LeaveCriticalSection(&g_cs);

	WaitForSingleObject(g_thread, 2000);
	CloseHandle(g_thread);
	g_thread = nullptr;
}
