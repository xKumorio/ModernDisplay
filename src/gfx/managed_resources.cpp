// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "managed_resources.h"
#include "texture_mip_cache.h"
#include "../core/config.h"
#include "../core/log.h"
#include <stdint.h>
#include <string.h>
#include <deque>
#include <vector>

volatile LONG g_dirtyManagedCount = 0;

static int g_texErrLogs = 0;
static ManagedTexDestroyCb_t g_destroyCb = nullptr;

static const void *g_vtblManagedTex = nullptr;
static const void *g_vtblManagedCube = nullptr;
static const void *g_vtblManagedVol = nullptr;
static const void *g_vtblManagedSurf = nullptr;

void SetManagedTexDestroyCallback(ManagedTexDestroyCb_t cb)
{
	g_destroyCb = cb;
}

static bool IsBlockCompressedFormat(D3DFORMAT fmt)
{
	return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3 ||
	       fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5 ||
	       fmt == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1') ||
	       fmt == (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2');
}

static bool IsPow2(UINT v) { return v != 0 && (v & (v - 1u)) == 0; }

static bool CanAutoMipDirect(D3DFORMAT fmt)
{
	return fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8 ||
	       fmt == D3DFMT_R5G6B5   || fmt == D3DFMT_A1R5G5B5 ||
	       fmt == D3DFMT_A4R4G4B4;
}

static bool CanAutoMipDxt(D3DFORMAT fmt)
{
	return fmt == D3DFMT_DXT1 || fmt == D3DFMT_DXT3 || fmt == D3DFMT_DXT5;
}

static inline uint32_t PackBgra(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static void DecodeDxtToBgra8(D3DFORMAT fmt, const uint8_t *src, int srcPitch,
                             uint8_t *dst, int dstPitch, UINT width, UINT height)
{
	const UINT bxCount = (width + 3u) / 4u;
	const UINT byCount = (height + 3u) / 4u;
	const bool isDxt1 = (fmt == D3DFMT_DXT1);
	const bool isDxt3 = (fmt == D3DFMT_DXT3);
	const size_t blockBytes = isDxt1 ? 8u : 16u;

	for (UINT by = 0; by < byCount; ++by) {
		const uint8_t *rowSrc = src + (size_t)by * (size_t)srcPitch;
		for (UINT bx = 0; bx < bxCount; ++bx) {
			const uint8_t *blk = rowSrc + (size_t)bx * blockBytes;
			uint8_t alpha16[16];
			const uint8_t *cblk = blk;
			if (!isDxt1) {
				cblk = blk + 8;
				if (isDxt3) {
					for (int i = 0; i < 8; ++i) {
						uint8_t b = blk[i];
						uint8_t a0 = b & 0x0Fu;
						uint8_t a1 = (b >> 4) & 0x0Fu;
						alpha16[i * 2 + 0] = (uint8_t)((a0 << 4) | a0);
						alpha16[i * 2 + 1] = (uint8_t)((a1 << 4) | a1);
					}
				} else {
					// DXT5 interpolated 3-bit alpha
					uint8_t aLut[8];
					aLut[0] = blk[0];
					aLut[1] = blk[1];
					if (aLut[0] > aLut[1]) {
						for (int k = 1; k <= 6; ++k)
							aLut[k + 1] = (uint8_t)(((7 - k) * aLut[0] + k * aLut[1] + 3) / 7);
					} else {
						for (int k = 1; k <= 4; ++k)
							aLut[k + 1] = (uint8_t)(((5 - k) * aLut[0] + k * aLut[1] + 2) / 5);
						aLut[6] = 0;
						aLut[7] = 255;
					}
					uint64_t bits = 0;
					for (int i = 0; i < 6; ++i)
						bits |= ((uint64_t)blk[2 + i]) << (i * 8);
					for (int i = 0; i < 16; ++i)
						alpha16[i] = aLut[(bits >> (i * 3)) & 7u];
				}
			}

			uint16_t c0 = (uint16_t)(cblk[0] | (cblk[1] << 8));
			uint16_t c1 = (uint16_t)(cblk[2] | (cblk[3] << 8));
			uint32_t r0 = ((c0 >> 11) & 31u) * 255u / 31u;
			uint32_t g0 = ((c0 >> 5)  & 63u) * 255u / 63u;
			uint32_t b0 = (c0 & 31u)         * 255u / 31u;
			uint32_t r1 = ((c1 >> 11) & 31u) * 255u / 31u;
			uint32_t g1 = ((c1 >> 5)  & 63u) * 255u / 63u;
			uint32_t b1 = (c1 & 31u)         * 255u / 31u;

			uint32_t lut[4];
			lut[0] = PackBgra(r0, g0, b0, 255u);
			lut[1] = PackBgra(r1, g1, b1, 255u);
			if (!isDxt1 || c0 > c1) {
				lut[2] = PackBgra((2u * r0 + r1 + 1u) / 3u, (2u * g0 + g1 + 1u) / 3u, (2u * b0 + b1 + 1u) / 3u, 255u);
				lut[3] = PackBgra((r0 + 2u * r1 + 1u) / 3u, (g0 + 2u * g1 + 1u) / 3u, (b0 + 2u * b1 + 1u) / 3u, 255u);
			} else {
				uint32_t rm = (r0 + r1) >> 1, gm = (g0 + g1) >> 1, bm = (b0 + b1) >> 1;
				lut[2] = PackBgra(rm, gm, bm, 255u);
				// Keep average RGB on transparent pixels to prevent black mipmap fringes around fences/leaves
				lut[3] = PackBgra(rm, gm, bm, 0u);
			}

			uint32_t idxBits = (uint32_t)cblk[4] | ((uint32_t)cblk[5] << 8) |
			                   ((uint32_t)cblk[6] << 16) | ((uint32_t)cblk[7] << 24);
			for (UINT py = 0; py < 4u; ++py) {
				UINT y = by * 4u + py;
				if (y >= height) break;
				auto *dstRow = (uint32_t *)(dst + (size_t)y * (size_t)dstPitch);
				for (UINT px = 0; px < 4u; ++px) {
					UINT x = bx * 4u + px;
					if (x >= width) break;
					UINT pi = py * 4u + px;
					uint32_t col = lut[(idxBits >> (pi * 2u)) & 3u];
					if (!isDxt1)
						col = (col & 0x00FFFFFFu) | ((uint32_t)alpha16[pi] << 24);
					dstRow[x] = col;
				}
			}
		}
	}
}

static volatile LONG g_autoMipCount = 0;

class ManagedTexture9;

// ============================================================================
// Asynchronous Background DXT -> BGRA8 Mipmap Worker Pool
// 100% D3D9 single-thread safe: workers run pure CPU DXT decode on background cores
// while the main thread uploads up to N ready mipmapped textures per frame using
// a single reusable SYSTEMMEM scratch surface (0ms main-thread driving/streaming stutter!).
// ============================================================================
struct PendingDxtJob {
	ManagedTexture9     *tex = nullptr; // Weak-guarded by tex->IsAlive() + AddRef/Release on main thread? Wait: AddRef'd!
	IDirect3DDevice9Ex  *devEx = nullptr;
	D3DFORMAT            fmt = D3DFMT_UNKNOWN;
	UINT                 width = 0;
	UINT                 height = 0;
	int                  srcPitch = 0;
	uint32_t             gen = 0;
	bool                 compressedMipCache = false;
	std::vector<uint8_t> dxtBytes;
};

struct ReadyMipJob {
	ManagedTexture9     *tex = nullptr;
	IDirect3DDevice9Ex  *devEx = nullptr;
	UINT                 width = 0;
	UINT                 height = 0;
	uint32_t             gen = 0;
	D3DFORMAT            fmt = D3DFMT_UNKNOWN;
	std::vector<uint8_t> bgra8;
	std::vector<uint8_t> compressed;
};

static SRWLOCK                   g_mipLock = SRWLOCK_INIT;
static CONDITION_VARIABLE        g_mipCv = CONDITION_VARIABLE_INIT;
static std::deque<PendingDxtJob> g_mipPending;
static std::deque<ReadyMipJob>   g_mipReady;
static HANDLE                    g_mipThreads[2] = {};
static bool                      g_mipWorkersStarted = false;
static bool                      g_mipWorkersStop = false;

// Reusable single SYSTEMMEM staging surface on main thread (zero per-texture CreateOffscreenPlainSurface churn!)
static IDirect3DDevice9Ex       *g_mipScratchDev = nullptr;
static IDirect3DSurface9        *g_mipScratchSurf = nullptr;
static UINT                      g_mipScratchW = 0;
static UINT                      g_mipScratchH = 0;

static void EnqueueAsyncDxtMip(PendingDxtJob &&job);
static bool IsTextureAliveForMip(const ManagedTexture9 *tex);
static void ApplyDecodedMipToTexture(ManagedTexture9 *tex, IDirect3DDevice9Ex *devEx,
                                     UINT w, UINT h, uint32_t gen, const std::vector<uint8_t> &bgra8);
static void ReleaseManagedTexRef(ManagedTexture9 *tex);

// Intrusive doubly-linked list of dirty managed resources (zero heap allocations, O(1) link/unlink).
struct ManagedDirtyNode {
	ManagedDirtyNode *prev = nullptr;
	ManagedDirtyNode *next = nullptr;
	bool              inList = false;
	virtual void SyncIfUnlocked() = 0;
	virtual ~ManagedDirtyNode() = default;
};

static SRWLOCK           g_dirtyLock = SRWLOCK_INIT;
static ManagedDirtyNode *g_dirtyHead = nullptr;

static void DirtyListAdd(ManagedDirtyNode *n)
{
	AcquireSRWLockExclusive(&g_dirtyLock);
	if (!n->inList) {
		n->prev = nullptr;
		n->next = g_dirtyHead;
		if (g_dirtyHead) g_dirtyHead->prev = n;
		g_dirtyHead = n;
		n->inList = true;
		InterlockedIncrement(&g_dirtyManagedCount);
	}
	ReleaseSRWLockExclusive(&g_dirtyLock);
}

static void DirtyListRemove(ManagedDirtyNode *n)
{
	AcquireSRWLockExclusive(&g_dirtyLock);
	if (n->inList) {
		if (n->prev) n->prev->next = n->next;
		else         g_dirtyHead   = n->next;
		if (n->next) n->next->prev = n->prev;
		n->prev = n->next = nullptr;
		n->inList = false;
		InterlockedDecrement(&g_dirtyManagedCount);
	}
	ReleaseSRWLockExclusive(&g_dirtyLock);
}

class IManagedParent {
public:
	virtual void MarkDirtyRect(UINT level, const RECT *pRect) = 0;
	virtual void OnLockBegin() = 0;
	virtual void OnLockEnd() = 0;
	virtual void SyncGpu() = 0;
	virtual IUnknown *ParentUnknown() = 0;
};

class ManagedSurface9 final : public IDirect3DSurface9 {
public:
	ManagedSurface9(IDirect3DDevice9 *proxyDev, IManagedParent *parent, UINT level,
	                IDirect3DSurface9 *sysSurf, IDirect3DSurface9 *gpuSurf)
		: m_proxyDev(proxyDev), m_parent(parent), m_level(level), m_sysSurf(sysSurf), m_gpuSurf(gpuSurf), m_ref(1)
	{
		if (!g_vtblManagedSurf) g_vtblManagedSurf = *(void *const *)this;
		m_parent->ParentUnknown()->AddRef();
	}

	~ManagedSurface9()
	{
		if (m_sysSurf) m_sysSurf->Release();
		if (m_gpuSurf) m_gpuSurf->Release();
		if (m_parent)  m_parent->ParentUnknown()->Release();
	}

	IDirect3DSurface9 *GpuSurf() { m_parent->SyncGpu(); return m_gpuSurf; }
	IDirect3DSurface9 *SysSurf() { return m_sysSurf; }
	void MarkDirty() { m_parent->MarkDirtyRect(m_level, nullptr); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 || riid == IID_IDirect3DSurface9) {
			*ppvObj = this;
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override
	{
		if (!ppDevice) return D3DERR_INVALIDCALL;
		*ppDevice = m_proxyDev;
		m_proxyDev->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override { return m_gpuSurf->SetPrivateData(refguid, pData, SizeOfData, Flags); }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return m_gpuSurf->GetPrivateData(refguid, pData, pSizeOfData); }
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return m_gpuSurf->FreePrivateData(refguid); }
	DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return m_gpuSurf->SetPriority(PriorityNew); }
	DWORD   STDMETHODCALLTYPE GetPriority() override { return m_gpuSurf->GetPriority(); }
	void    STDMETHODCALLTYPE PreLoad() override { m_parent->SyncGpu(); m_gpuSurf->PreLoad(); }
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }

	HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) override
	{
		return m_parent->ParentUnknown()->QueryInterface(riid, ppContainer);
	}
	HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override
	{
		HRESULT hr = m_sysSurf->GetDesc(pDesc);
		if (SUCCEEDED(hr) && pDesc) pDesc->Pool = D3DPOOL_MANAGED;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
	{
		const bool smart = g_cfg.smartManaged;
		DWORD effFlags = (smart && !(Flags & D3DLOCK_READONLY)) ? (Flags | D3DLOCK_NO_DIRTY_UPDATE) : Flags;
		HRESULT hr = m_sysSurf->LockRect(pLockedRect, pRect, effFlags);
		if (FAILED(hr) && effFlags != Flags)
			hr = m_sysSurf->LockRect(pLockedRect, pRect, Flags);
		if (SUCCEEDED(hr)) {
			m_parent->OnLockBegin();
			if (!smart)
				m_parent->MarkDirtyRect(m_level, nullptr);
			else if (!(Flags & D3DLOCK_READONLY))
				m_parent->MarkDirtyRect(m_level, pRect);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect() override
	{
		HRESULT hr = m_sysSurf->UnlockRect();
		if (SUCCEEDED(hr)) {
			m_parent->OnLockEnd();
			if (!g_cfg.smartManaged) m_parent->SyncGpu();
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override
	{
		HRESULT hr = m_sysSurf->GetDC(phdc);
		if (SUCCEEDED(hr)) {
			m_parent->OnLockBegin();
			m_parent->MarkDirtyRect(m_level, nullptr);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override
	{
		HRESULT hr = m_sysSurf->ReleaseDC(hdc);
		if (SUCCEEDED(hr)) m_parent->OnLockEnd();
		return hr;
	}

private:
	IDirect3DDevice9  *m_proxyDev;
	IManagedParent    *m_parent;
	UINT               m_level;
	IDirect3DSurface9 *m_sysSurf;
	IDirect3DSurface9 *m_gpuSurf;
	volatile LONG      m_ref;
};

class ManagedTexture9 final : public IDirect3DTexture9, public IManagedParent, public ManagedDirtyNode {
public:
	ManagedTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
	                IDirect3DTexture9 *sysTex, IDirect3DTexture9 *gpuTex,
	                UINT width, UINT height, UINT levels, bool blockCompressed,
	                D3DFORMAT asyncDxtFmt = D3DFMT_UNKNOWN, bool autoMipDirect = false)
		: m_devEx(devEx), m_proxyDev(proxyDev), m_sysTex(sysTex), m_gpuTex(gpuTex),
		  m_width(width), m_height(height), m_levels(levels), m_blockCompressed(blockCompressed),
		  m_asyncDxtFmt(asyncDxtFmt), m_autoMipDirect(autoMipDirect),
		  m_ref(1), m_dirty(true), m_dirtyAll(true)
	{
		if (!g_vtblManagedTex) g_vtblManagedTex = *(void *const *)(IDirect3DTexture9 *)this;
		DirtyListAdd(this);
	}
	~ManagedTexture9() override
	{
		m_dirty = false;
		DirtyListRemove(this);
		if (g_destroyCb) g_destroyCb(static_cast<IDirect3DTexture9 *>(this));
		if (m_sysTex) m_sysTex->Release();
		if (m_gpuTex) m_gpuTex->Release();
	}

	void OnLockBegin() override { ++m_lockCount; }
	void OnLockEnd() override { if (m_lockCount > 0) --m_lockCount; }
	void SyncIfUnlocked() override { if (m_lockCount == 0) SyncGpu(); }

	void MarkDirtyRect(UINT level, const RECT *pRect) override
	{
		if (!m_dirty) {
			m_dirty = true;
			DirtyListAdd(this);
		}
		if (m_dirtyAll) return;

		if (level != 0 || !pRect || m_blockCompressed || m_levels > 1) {
			m_dirtyAll = true;
			m_hasDirtyRect = false;
			return;
		}

		RECT r = *pRect;
		if (r.left < 0) r.left = 0;
		if (r.top  < 0) r.top  = 0;
		if (r.right  > (LONG)m_width)  r.right  = (LONG)m_width;
		if (r.bottom > (LONG)m_height) r.bottom = (LONG)m_height;
		if (r.left >= r.right || r.top >= r.bottom) {
			m_dirtyAll = true;
			m_hasDirtyRect = false;
			return;
		}
		if (!m_hasDirtyRect) {
			m_dirtyRect = r;
			m_hasDirtyRect = true;
		} else {
			if (r.left   < m_dirtyRect.left)   m_dirtyRect.left   = r.left;
			if (r.top    < m_dirtyRect.top)    m_dirtyRect.top    = r.top;
			if (r.right  > m_dirtyRect.right)  m_dirtyRect.right  = r.right;
			if (r.bottom > m_dirtyRect.bottom) m_dirtyRect.bottom = r.bottom;
		}
	}

	bool HasExternalRefs() const { return InterlockedCompareExchange(const_cast<volatile LONG *>(&m_ref), 0, 0) > 1; }
	bool CanApplyMip(uint32_t gen) const { return gen == m_mipGen && !m_dirty && m_lockCount == 0; }

	void SwapToMipmappedGpuTex(IDirect3DTexture9 *newMipTex, uint32_t jobGen)
	{
		if (!newMipTex) return;
		if (!CanApplyMip(jobGen)) {
			newMipTex->Release();
			return;
		}
		if (m_gpuTex) m_gpuTex->Release();
		m_gpuTex = newMipTex;
		m_gpuHasGeneratedMips = true;
		// Notify proxy device so cached Stage binding re-binds the upgraded mipmapped GPU texture cleanly
		if (g_destroyCb) g_destroyCb(static_cast<IDirect3DTexture9 *>(this));
	}

	void SyncGpu() override
	{
		if (!m_dirty || m_lockCount > 0) return;
		m_dirty = false;
		DirtyListRemove(this);

		if (m_asyncDxtFmt != D3DFMT_UNKNOWN) {
			m_dirtyAll = false;
			m_hasDirtyRect = false;
			const uint32_t gen = ++m_mipGen;

			// If still backed by original 1-level DXT GPU texture, upload immediately (~0.01ms) so Frame 0 has 0 missing textures!
			if (!m_gpuHasGeneratedMips) {
				m_sysTex->AddDirtyRect(nullptr);
				m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
			}

			// Copy compact raw DXT bytes and offload CPU decode to background worker thread (0ms main-thread streaming hitch!)
			D3DLOCKED_RECT lrSrc = {};
			if (SUCCEEDED(m_sysTex->LockRect(0, &lrSrc, nullptr, D3DLOCK_READONLY)) && lrSrc.pBits && lrSrc.Pitch > 0) {
				const UINT byCount = (m_height + 3u) / 4u;
				const size_t totalBytes = (size_t)byCount * (size_t)lrSrc.Pitch;
				PendingDxtJob job;
				job.tex = this;
				job.devEx = m_devEx;
				job.fmt = m_asyncDxtFmt;
				job.width = m_width;
				job.height = m_height;
				job.srcPitch = lrSrc.Pitch;
				job.gen = gen;
				job.compressedMipCache = g_cfg.compressedMipCache;
				job.dxtBytes.resize(totalBytes);
				memcpy(job.dxtBytes.data(), lrSrc.pBits, totalBytes);
				m_sysTex->UnlockRect(0);
				this->AddRef(); // Released strictly on main thread in DrainAsyncMipUploads / ShutdownManagedMipWorker
				EnqueueAsyncDxtMip(std::move(job));
			}
			return;
		}

		if (!m_dirtyAll && m_hasDirtyRect) {
			RECT r = m_dirtyRect;
			m_hasDirtyRect = false;
			if (FAILED(m_sysTex->AddDirtyRect(&r)))
				m_sysTex->AddDirtyRect(nullptr);
		} else {
			m_dirtyAll = false;
			m_hasDirtyRect = false;
			m_sysTex->AddDirtyRect(nullptr);
		}
		HRESULT hr = m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
		if (SUCCEEDED(hr) && m_autoMipDirect)
			m_gpuTex->GenerateMipSubLevels();
		if (FAILED(hr) && g_texErrLogs < 16) {
			++g_texErrLogs;
			D3DSURFACE_DESC d = {};
			m_sysTex->GetLevelDesc(0, &d);
			Log("D3D9Ex: UpdateTexture failed hr=0x%08lX (%ux%u fmt=%u lvls=%u)", hr, d.Width, d.Height, (unsigned)d.Format, m_sysTex->GetLevelCount());
		}
	}

	IUnknown *ParentUnknown() override { return static_cast<IDirect3DTexture9 *>(this); }
	IDirect3DTexture9 *GpuTex() { if (m_dirty) SyncGpu(); return m_gpuTex; }
	IDirect3DTexture9 *SysTex() { return m_sysTex; }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 ||
		    riid == IID_IDirect3DBaseTexture9 || riid == IID_IDirect3DTexture9) {
			*ppvObj = static_cast<IDirect3DTexture9 *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override
	{
		if (!ppDevice) return D3DERR_INVALIDCALL;
		*ppDevice = m_proxyDev;
		m_proxyDev->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override { return m_gpuTex->SetPrivateData(refguid, pData, SizeOfData, Flags); }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return m_gpuTex->GetPrivateData(refguid, pData, pSizeOfData); }
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return m_gpuTex->FreePrivateData(refguid); }
	DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return m_gpuTex->SetPriority(PriorityNew); }
	DWORD   STDMETHODCALLTYPE GetPriority() override { return m_gpuTex->GetPriority(); }
	void    STDMETHODCALLTYPE PreLoad() override { SyncGpu(); m_gpuTex->PreLoad(); }
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }

	DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return m_gpuTex->SetLOD(LODNew); }
	DWORD   STDMETHODCALLTYPE GetLOD() override { return m_gpuTex->GetLOD(); }
	DWORD   STDMETHODCALLTYPE GetLevelCount() override { return m_sysTex->GetLevelCount(); }
	HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { return m_gpuTex->SetAutoGenFilterType(FilterType); }
	D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return m_gpuTex->GetAutoGenFilterType(); }
	void    STDMETHODCALLTYPE GenerateMipSubLevels() override { SyncGpu(); m_gpuTex->GenerateMipSubLevels(); }

	HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override
	{
		HRESULT hr = m_sysTex->GetLevelDesc(Level, pDesc);
		if (SUCCEEDED(hr) && pDesc) pDesc->Pool = D3DPOOL_MANAGED;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override
	{
		if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *sysS = nullptr, *gpuS = nullptr;
		HRESULT hr = m_sysTex->GetSurfaceLevel(Level, &sysS);
		if (FAILED(hr)) return hr;
		hr = m_gpuTex->GetSurfaceLevel(Level, &gpuS);
		if (FAILED(hr)) { sysS->Release(); return hr; }
		*ppSurfaceLevel = new ManagedSurface9(m_proxyDev, this, Level, sysS, gpuS);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
	{
		const bool smart = g_cfg.smartManaged;
		DWORD effFlags = (smart && !(Flags & D3DLOCK_READONLY)) ? (Flags | D3DLOCK_NO_DIRTY_UPDATE) : Flags;
		HRESULT hr = m_sysTex->LockRect(Level, pLockedRect, pRect, effFlags);
		if (FAILED(hr) && effFlags != Flags)
			hr = m_sysTex->LockRect(Level, pLockedRect, pRect, Flags);
		if (SUCCEEDED(hr)) {
			++m_lockCount;
			if (!smart)
				MarkDirtyRect(Level, nullptr);
			else if (!(Flags & D3DLOCK_READONLY))
				MarkDirtyRect(Level, pRect);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override
	{
		HRESULT hr = m_sysTex->UnlockRect(Level);
		if (SUCCEEDED(hr)) {
			if (m_lockCount > 0) --m_lockCount;
			if (!g_cfg.smartManaged) SyncGpu();
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT *pDirtyRect) override
	{
		MarkDirtyRect(0, pDirtyRect);
		return S_OK;
	}

private:
	IDirect3DDevice9Ex *m_devEx;
	IDirect3DDevice9   *m_proxyDev;
	IDirect3DTexture9  *m_sysTex;
	IDirect3DTexture9  *m_gpuTex;
	UINT                m_width;
	UINT                m_height;
	UINT                m_levels;
	bool                m_blockCompressed;
	D3DFORMAT           m_asyncDxtFmt = D3DFMT_UNKNOWN;
	bool                m_autoMipDirect = false;
	bool                m_gpuHasGeneratedMips = false;
	uint32_t            m_mipGen = 0;
	volatile LONG       m_ref;
	int                 m_lockCount = 0;
	bool                m_dirty;
	bool                m_dirtyAll;
	bool                m_hasDirtyRect = false;
	RECT                m_dirtyRect = {};
};

class ManagedCubeTexture9 final : public IDirect3DCubeTexture9, public IManagedParent, public ManagedDirtyNode {
public:
	ManagedCubeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
	                    IDirect3DCubeTexture9 *sysTex, IDirect3DCubeTexture9 *gpuTex)
		: m_devEx(devEx), m_proxyDev(proxyDev), m_sysTex(sysTex), m_gpuTex(gpuTex), m_ref(1), m_dirty(true)
	{
		if (!g_vtblManagedCube) g_vtblManagedCube = *(void *const *)(IDirect3DCubeTexture9 *)this;
		DirtyListAdd(this);
	}
	~ManagedCubeTexture9() override
	{
		m_dirty = false;
		DirtyListRemove(this);
		if (g_destroyCb) g_destroyCb(static_cast<IDirect3DCubeTexture9 *>(this));
		if (m_sysTex) m_sysTex->Release();
		if (m_gpuTex) m_gpuTex->Release();
	}

	void OnLockBegin() override { ++m_lockCount; }
	void OnLockEnd() override { if (m_lockCount > 0) --m_lockCount; }
	void SyncIfUnlocked() override { if (m_lockCount == 0) SyncGpu(); }

	void MarkDirtyRect(UINT, const RECT *) override
	{
		if (!m_dirty) {
			m_dirty = true;
			DirtyListAdd(this);
		}
	}
	void SyncGpu() override
	{
		if (!m_dirty || m_lockCount > 0) return;
		m_dirty = false;
		DirtyListRemove(this);
		for (int f = 0; f < 6; ++f)
			m_sysTex->AddDirtyRect((D3DCUBEMAP_FACES)f, nullptr);
		m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
	}
	IUnknown *ParentUnknown() override { return static_cast<IDirect3DCubeTexture9 *>(this); }
	IDirect3DCubeTexture9 *GpuTex() { if (m_dirty) SyncGpu(); return m_gpuTex; }
	IDirect3DCubeTexture9 *SysTex() { return m_sysTex; }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 ||
		    riid == IID_IDirect3DBaseTexture9 || riid == IID_IDirect3DCubeTexture9) {
			*ppvObj = static_cast<IDirect3DCubeTexture9 *>(this);
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override
	{
		if (!ppDevice) return D3DERR_INVALIDCALL;
		*ppDevice = m_proxyDev;
		m_proxyDev->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override { return m_gpuTex->SetPrivateData(refguid, pData, SizeOfData, Flags); }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return m_gpuTex->GetPrivateData(refguid, pData, pSizeOfData); }
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return m_gpuTex->FreePrivateData(refguid); }
	DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return m_gpuTex->SetPriority(PriorityNew); }
	DWORD   STDMETHODCALLTYPE GetPriority() override { return m_gpuTex->GetPriority(); }
	void    STDMETHODCALLTYPE PreLoad() override { SyncGpu(); m_gpuTex->PreLoad(); }
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
	DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return m_gpuTex->SetLOD(LODNew); }
	DWORD   STDMETHODCALLTYPE GetLOD() override { return m_gpuTex->GetLOD(); }
	DWORD   STDMETHODCALLTYPE GetLevelCount() override { return m_sysTex->GetLevelCount(); }
	HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { return m_gpuTex->SetAutoGenFilterType(FilterType); }
	D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return m_gpuTex->GetAutoGenFilterType(); }
	void    STDMETHODCALLTYPE GenerateMipSubLevels() override { SyncGpu(); m_gpuTex->GenerateMipSubLevels(); }

	HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override
	{
		HRESULT hr = m_sysTex->GetLevelDesc(Level, pDesc);
		if (SUCCEEDED(hr) && pDesc) pDesc->Pool = D3DPOOL_MANAGED;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) override
	{
		if (!ppCubeMapSurface) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *sysS = nullptr, *gpuS = nullptr;
		HRESULT hr = m_sysTex->GetCubeMapSurface(FaceType, Level, &sysS);
		if (FAILED(hr)) return hr;
		hr = m_gpuTex->GetCubeMapSurface(FaceType, Level, &gpuS);
		if (FAILED(hr)) { sysS->Release(); return hr; }
		*ppCubeMapSurface = new ManagedSurface9(m_proxyDev, this, Level, sysS, gpuS);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
	{
		HRESULT hr = m_sysTex->LockRect(FaceType, Level, pLockedRect, pRect, Flags);
		if (SUCCEEDED(hr)) {
			++m_lockCount;
			if (!(Flags & D3DLOCK_READONLY)) MarkDirtyRect(Level, pRect);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override
	{
		HRESULT hr = m_sysTex->UnlockRect(FaceType, Level);
		if (SUCCEEDED(hr)) {
			if (m_lockCount > 0) --m_lockCount;
			if (!g_cfg.smartManaged) SyncGpu();
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, CONST RECT *pDirtyRect) override
	{
		MarkDirtyRect(0, pDirtyRect);
		return m_sysTex->AddDirtyRect(FaceType, pDirtyRect);
	}

private:
	IDirect3DDevice9Ex    *m_devEx;
	IDirect3DDevice9      *m_proxyDev;
	IDirect3DCubeTexture9 *m_sysTex;
	IDirect3DCubeTexture9 *m_gpuTex;
	volatile LONG          m_ref;
	int                    m_lockCount = 0;
	bool                   m_dirty;
};

class ManagedVolumeTexture9 final : public IDirect3DVolumeTexture9, public ManagedDirtyNode {
public:
	ManagedVolumeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
	                      IDirect3DVolumeTexture9 *sysTex, IDirect3DVolumeTexture9 *gpuTex)
		: m_devEx(devEx), m_proxyDev(proxyDev), m_sysTex(sysTex), m_gpuTex(gpuTex), m_ref(1), m_dirty(true)
	{
		if (!g_vtblManagedVol) g_vtblManagedVol = *(void *const *)(IDirect3DVolumeTexture9 *)this;
		DirtyListAdd(this);
	}
	~ManagedVolumeTexture9() override
	{
		m_dirty = false;
		DirtyListRemove(this);
		if (g_destroyCb) g_destroyCb(static_cast<IDirect3DVolumeTexture9 *>(this));
		if (m_sysTex) m_sysTex->Release();
		if (m_gpuTex) m_gpuTex->Release();
	}

	void SyncIfUnlocked() override { if (m_lockCount == 0) SyncGpu(); }

	void MarkDirty()
	{
		if (!m_dirty) {
			m_dirty = true;
			DirtyListAdd(this);
		}
	}
	void SyncGpu()
	{
		if (!m_dirty || m_lockCount > 0) return;
		m_dirty = false;
		DirtyListRemove(this);
		m_sysTex->AddDirtyBox(nullptr);
		m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
	}
	IDirect3DVolumeTexture9 *GpuTex() { if (m_dirty) SyncGpu(); return m_gpuTex; }
	IDirect3DVolumeTexture9 *SysTex() { return m_sysTex; }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 ||
		    riid == IID_IDirect3DBaseTexture9 || riid == IID_IDirect3DVolumeTexture9) {
			*ppvObj = this;
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override
	{
		if (!ppDevice) return D3DERR_INVALIDCALL;
		*ppDevice = m_proxyDev;
		m_proxyDev->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override { return m_gpuTex->SetPrivateData(refguid, pData, SizeOfData, Flags); }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return m_gpuTex->GetPrivateData(refguid, pData, pSizeOfData); }
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return m_gpuTex->FreePrivateData(refguid); }
	DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return m_gpuTex->SetPriority(PriorityNew); }
	DWORD   STDMETHODCALLTYPE GetPriority() override { return m_gpuTex->GetPriority(); }
	void    STDMETHODCALLTYPE PreLoad() override { SyncGpu(); m_gpuTex->PreLoad(); }
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
	DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return m_gpuTex->SetLOD(LODNew); }
	DWORD   STDMETHODCALLTYPE GetLOD() override { return m_gpuTex->GetLOD(); }
	DWORD   STDMETHODCALLTYPE GetLevelCount() override { return m_sysTex->GetLevelCount(); }
	HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { return m_gpuTex->SetAutoGenFilterType(FilterType); }
	D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return m_gpuTex->GetAutoGenFilterType(); }
	void    STDMETHODCALLTYPE GenerateMipSubLevels() override { SyncGpu(); m_gpuTex->GenerateMipSubLevels(); }

	HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) override
	{
		HRESULT hr = m_sysTex->GetLevelDesc(Level, pDesc);
		if (SUCCEEDED(hr) && pDesc) pDesc->Pool = D3DPOOL_MANAGED;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) override { MarkDirty(); return m_sysTex->GetVolumeLevel(Level, ppVolumeLevel); }
	HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags) override
	{
		HRESULT hr = m_sysTex->LockBox(Level, pLockedVolume, pBox, Flags);
		if (SUCCEEDED(hr)) {
			++m_lockCount;
			if (!(Flags & D3DLOCK_READONLY)) MarkDirty();
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) override
	{
		HRESULT hr = m_sysTex->UnlockBox(Level);
		if (SUCCEEDED(hr)) {
			if (m_lockCount > 0) --m_lockCount;
			if (!g_cfg.smartManaged) SyncGpu();
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE AddDirtyBox(CONST D3DBOX *pDirtyBox) override { MarkDirty(); return m_sysTex->AddDirtyBox(pDirtyBox); }

private:
	IDirect3DDevice9Ex      *m_devEx;
	IDirect3DDevice9        *m_proxyDev;
	IDirect3DVolumeTexture9 *m_sysTex;
	IDirect3DVolumeTexture9 *m_gpuTex;
	volatile LONG            m_ref;
	int                      m_lockCount = 0;
	bool                     m_dirty;
};

HRESULT CreateManagedTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
                              UINT width, UINT height, UINT levels, DWORD usage,
                              D3DFORMAT format, IDirect3DTexture9 **outTex)
{
	if (!outTex) return D3DERR_INVALIDCALL;
	IDirect3DTexture9 *sysTex = nullptr, *gpuTex = nullptr;
	DWORD gpuUsage = usage & D3DUSAGE_AUTOGENMIPMAP;
	UINT sysLevels = gpuUsage ? 1u : levels;
	HRESULT hr = devEx->CreateTexture(width, height, sysLevels, 0, format, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
	if (FAILED(hr)) {
		if (g_texErrLogs < 16) {
			++g_texErrLogs;
			Log("D3D9Ex: CreateManagedTexture sys failed hr=0x%08lX (%ux%u lvls=%u usage=0x%lX fmt=%u)", hr, width, height, levels, usage, (unsigned)format);
		}
		return hr;
	}
	D3DFORMAT asyncDxtFmt = D3DFMT_UNKNOWN;
	bool autoMipDirect = false;
	const bool wantAutoMip = (g_cfg.anisotropy >= 2) && (levels == 1u) && (usage == 0u) &&
	                         (width >= 32u && height >= 32u && width <= 2048u && height <= 2048u) &&
	                         IsPow2(width) && IsPow2(height);
	if (wantAutoMip && CanAutoMipDirect(format)) {
		hr = devEx->CreateTexture(width, height, 0, D3DUSAGE_AUTOGENMIPMAP, format, D3DPOOL_DEFAULT, &gpuTex, nullptr);
		if (SUCCEEDED(hr) && gpuTex) {
			gpuTex->SetAutoGenFilterType(D3DTEXF_LINEAR);
			autoMipDirect = true;
		}
	} else if (wantAutoMip && CanAutoMipDxt(format)) {
		// Create fast 1-level DXT GPU texture immediately (0ms hitch on Frame 0), then upgrade asynchronously on worker thread!
		asyncDxtFmt = format;
	}
	if (!gpuTex) {
		hr = devEx->CreateTexture(width, height, levels, gpuUsage, format, D3DPOOL_DEFAULT, &gpuTex, nullptr);
	}
	if (FAILED(hr) || !gpuTex) {
		if (g_texErrLogs < 16) {
			++g_texErrLogs;
			Log("D3D9Ex: CreateManagedTexture gpu failed hr=0x%08lX (%ux%u lvls=%u usage=0x%lX fmt=%u)", hr, width, height, levels, usage, (unsigned)format);
		}
		sysTex->Release();
		return hr;
	}
	if (autoMipDirect || asyncDxtFmt != D3DFMT_UNKNOWN) {
		LONG n = InterlockedIncrement(&g_autoMipCount);
		if (n == 1)
			Log("Mipmaps: async background GPU auto-mipmaps active (2 workers, 0ms main-thread decode hitch)");
	}
	UINT actualLevels = sysTex->GetLevelCount();
	D3DSURFACE_DESC desc0 = {};
	sysTex->GetLevelDesc(0, &desc0);
	*outTex = new ManagedTexture9(devEx, proxyDev, sysTex, gpuTex,
	                              desc0.Width ? desc0.Width : width,
	                              desc0.Height ? desc0.Height : height,
	                              actualLevels ? actualLevels : 1u,
	                              IsBlockCompressedFormat(format),
	                              asyncDxtFmt, autoMipDirect);
	return S_OK;
}

HRESULT CreateManagedCubeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
                                  UINT edgeLength, UINT levels, DWORD usage,
                                  D3DFORMAT format, IDirect3DCubeTexture9 **outTex)
{
	if (!outTex) return D3DERR_INVALIDCALL;
	IDirect3DCubeTexture9 *sysTex = nullptr, *gpuTex = nullptr;
	DWORD gpuUsage = usage & D3DUSAGE_AUTOGENMIPMAP;
	UINT sysLevels = gpuUsage ? 1u : levels;
	HRESULT hr = devEx->CreateCubeTexture(edgeLength, sysLevels, 0, format, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
	if (FAILED(hr)) return hr;
	hr = devEx->CreateCubeTexture(edgeLength, levels, gpuUsage, format, D3DPOOL_DEFAULT, &gpuTex, nullptr);
	if (FAILED(hr)) { sysTex->Release(); return hr; }
	*outTex = new ManagedCubeTexture9(devEx, proxyDev, sysTex, gpuTex);
	return S_OK;
}

HRESULT CreateManagedVolumeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
                                    UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                    D3DFORMAT format, IDirect3DVolumeTexture9 **outTex)
{
	if (!outTex) return D3DERR_INVALIDCALL;
	IDirect3DVolumeTexture9 *sysTex = nullptr, *gpuTex = nullptr;
	DWORD gpuUsage = usage & D3DUSAGE_AUTOGENMIPMAP;
	UINT sysLevels = gpuUsage ? 1u : levels;
	HRESULT hr = devEx->CreateVolumeTexture(width, height, depth, sysLevels, 0, format, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
	if (FAILED(hr)) return hr;
	hr = devEx->CreateVolumeTexture(width, height, depth, levels, gpuUsage, format, D3DPOOL_DEFAULT, &gpuTex, nullptr);
	if (FAILED(hr)) { sysTex->Release(); return hr; }
	*outTex = new ManagedVolumeTexture9(devEx, proxyDev, sysTex, gpuTex);
	return S_OK;
}

IDirect3DBaseTexture9 *UnwrapBaseTextureForGpu(IDirect3DBaseTexture9 *tex, bool *outIsManaged)
{
	if (outIsManaged) *outIsManaged = false;
	if (!tex) return nullptr;
	const void *vt = *(void *const *)tex;
	if (vt == g_vtblManagedTex)  { if (outIsManaged) *outIsManaged = true; return static_cast<ManagedTexture9 *>(tex)->GpuTex(); }
	if (vt == g_vtblManagedCube) { if (outIsManaged) *outIsManaged = true; return static_cast<ManagedCubeTexture9 *>(tex)->GpuTex(); }
	if (vt == g_vtblManagedVol)  { if (outIsManaged) *outIsManaged = true; return static_cast<ManagedVolumeTexture9 *>(tex)->GpuTex(); }
	return tex;
}

IDirect3DBaseTexture9 *UnwrapBaseTextureForSys(IDirect3DBaseTexture9 *tex)
{
	if (!tex) return nullptr;
	const void *vt = *(void *const *)tex;
	if (vt == g_vtblManagedTex)  return static_cast<ManagedTexture9 *>(tex)->SysTex();
	if (vt == g_vtblManagedCube) return static_cast<ManagedCubeTexture9 *>(tex)->SysTex();
	if (vt == g_vtblManagedVol)  return static_cast<ManagedVolumeTexture9 *>(tex)->SysTex();
	return tex;
}

void SyncManagedBaseTexture(IDirect3DBaseTexture9 *tex)
{
	if (!tex) return;
	const void *vt = *(void *const *)tex;
	if (vt == g_vtblManagedTex)       static_cast<ManagedTexture9 *>(tex)->SyncGpu();
	else if (vt == g_vtblManagedCube) static_cast<ManagedCubeTexture9 *>(tex)->SyncGpu();
	else if (vt == g_vtblManagedVol)  static_cast<ManagedVolumeTexture9 *>(tex)->SyncGpu();
}

void SyncAllDirtyManagedResources()
{
	if (!HasDirtyManagedResources()) return;
	// Snapshot up to 64 dirty nodes per pass so SyncGpu() can safely unlink without holding g_dirtyLock.
	ManagedDirtyNode *batch[64];
	int n = 0;
	AcquireSRWLockShared(&g_dirtyLock);
	for (ManagedDirtyNode *cur = g_dirtyHead; cur && n < 64; cur = cur->next)
		batch[n++] = cur;
	ReleaseSRWLockShared(&g_dirtyLock);
	for (int i = 0; i < n; ++i)
		batch[i]->SyncIfUnlocked();
}

IDirect3DSurface9 *UnwrapSurfaceForGpu(IDirect3DSurface9 *surf)
{
	if (!surf) return nullptr;
	if (*(void *const *)surf == g_vtblManagedSurf)
		return static_cast<ManagedSurface9 *>(surf)->GpuSurf();
	return surf;
}

IDirect3DSurface9 *UnwrapSurfaceForSys(IDirect3DSurface9 *surf)
{
	if (!surf) return nullptr;
	if (*(void *const *)surf == g_vtblManagedSurf)
		return static_cast<ManagedSurface9 *>(surf)->SysSurf();
	return surf;
}

void MarkSurfaceDirtyIfManaged(IDirect3DSurface9 *surf)
{
	if (surf && *(void *const *)surf == g_vtblManagedSurf)
		static_cast<ManagedSurface9 *>(surf)->MarkDirty();
}

static bool IsTextureAliveForMip(const ManagedTexture9 *tex)
{
	return tex && tex->HasExternalRefs();
}

static void ReleaseManagedTexRef(ManagedTexture9 *tex)
{
	if (tex) tex->Release();
}

static DWORD WINAPI MipWorkerThreadProc(LPVOID)
{
	for (;;) {
		PendingDxtJob job;
		AcquireSRWLockExclusive(&g_mipLock);
		while (g_mipPending.empty() && !g_mipWorkersStop)
			SleepConditionVariableSRW(&g_mipCv, &g_mipLock, INFINITE, 0);
		if (g_mipWorkersStop) {
			ReleaseSRWLockExclusive(&g_mipLock);
			return 0;
		}
		job = std::move(g_mipPending.front());
		g_mipPending.pop_front();
		ReleaseSRWLockExclusive(&g_mipLock);

		ReadyMipJob ready;
		ready.tex    = job.tex;
		ready.devEx  = job.devEx;
		ready.width  = job.width;
		ready.height = job.height;
		ready.gen    = job.gen;
		ready.fmt    = job.fmt;

		// Skip CPU decode completely if the game already released the texture (still push empty ready job so main thread calls Release() safely!)
		if (IsTextureAliveForMip(job.tex) && job.width > 0 && job.height > 0 && !job.dxtBytes.empty()) {
			bool cached = false;
			if (job.compressedMipCache)
				cached = LoadDxtMipChain(job.fmt, job.width, job.height, job.srcPitch, job.dxtBytes, ready.compressed);
			if (cached) {
				static volatile LONG firstHit = 0;
				if (InterlockedExchange(&firstHit, 1) == 0) Log("Mipmaps: loaded cached DXT mip chain");
			} else {
				ready.bgra8.resize((size_t)job.width * (size_t)job.height * 4u);
				DecodeDxtToBgra8(job.fmt, job.dxtBytes.data(), job.srcPitch,
				                 ready.bgra8.data(), (int)(job.width * 4u), job.width, job.height);
				if (job.compressedMipCache)
					BuildDxtMipChain(job.fmt, job.width, job.height, job.srcPitch, job.dxtBytes,
					                 std::move(ready.bgra8), ready.compressed);
			}
		}

		AcquireSRWLockExclusive(&g_mipLock);
		g_mipReady.push_back(std::move(ready));
		ReleaseSRWLockExclusive(&g_mipLock);
	}
}

static void EnqueueAsyncDxtMip(PendingDxtJob &&job)
{
	AcquireSRWLockExclusive(&g_mipLock);
	if (!g_mipWorkersStarted) {
		g_mipWorkersStarted = true;
		g_mipWorkersStop = false;
		for (int i = 0; i < 2; ++i) {
			g_mipThreads[i] = CreateThread(nullptr, 0, MipWorkerThreadProc, nullptr, 0, nullptr);
			if (g_mipThreads[i])
				SetThreadPriority(g_mipThreads[i], THREAD_PRIORITY_BELOW_NORMAL);
		}
	}
	g_mipPending.push_back(std::move(job));
	WakeConditionVariable(&g_mipCv);
	ReleaseSRWLockExclusive(&g_mipLock);
}

static void ApplyDecodedMipToTexture(ManagedTexture9 *tex, IDirect3DDevice9Ex *devEx,
                                     UINT w, UINT h, uint32_t gen, const std::vector<uint8_t> &bgra8)
{
	if (!tex || !devEx || bgra8.empty() || !tex->HasExternalRefs() || !tex->CanApplyMip(gen))
		return;

	// Ensure reusable SYSTEMMEM staging surface is at least w x h (zero per-texture allocation churn!)
	if (!g_mipScratchSurf || g_mipScratchDev != devEx || g_mipScratchW < w || g_mipScratchH < h) {
		if (g_mipScratchSurf) { g_mipScratchSurf->Release(); g_mipScratchSurf = nullptr; }
		UINT sw = (w > g_mipScratchW && g_mipScratchDev == devEx) ? w : ((g_mipScratchDev == devEx && g_mipScratchW > w) ? g_mipScratchW : w);
		UINT sh = (h > g_mipScratchH && g_mipScratchDev == devEx) ? h : ((g_mipScratchDev == devEx && g_mipScratchH > h) ? g_mipScratchH : h);
		if (sw < 512u) sw = 512u;
		if (sh < 512u) sh = 512u;
		if (FAILED(devEx->CreateOffscreenPlainSurface(sw, sh, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_mipScratchSurf, nullptr)) || !g_mipScratchSurf) {
			g_mipScratchDev = nullptr; g_mipScratchW = g_mipScratchH = 0;
			return;
		}
		g_mipScratchDev = devEx;
		g_mipScratchW = sw;
		g_mipScratchH = sh;
	}

	IDirect3DTexture9 *mipTex = nullptr;
	if (FAILED(devEx->CreateTexture(w, h, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &mipTex, nullptr)) || !mipTex)
		return;
	mipTex->SetAutoGenFilterType(D3DTEXF_LINEAR);

	RECT rc = { 0, 0, (LONG)w, (LONG)h };
	D3DLOCKED_RECT lr = {};
	if (SUCCEEDED(g_mipScratchSurf->LockRect(&lr, &rc, 0)) && lr.pBits) {
		const size_t rowBytes = (size_t)w * 4u;
		for (UINT y = 0; y < h; ++y)
			memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch, bgra8.data() + (size_t)y * rowBytes, rowBytes);
		g_mipScratchSurf->UnlockRect();

		IDirect3DSurface9 *gpuSurf0 = nullptr;
		if (SUCCEEDED(mipTex->GetSurfaceLevel(0, &gpuSurf0)) && gpuSurf0) {
			POINT pt0 = { 0, 0 };
			if (SUCCEEDED(devEx->UpdateSurface(g_mipScratchSurf, &rc, gpuSurf0, &pt0))) {
				mipTex->GenerateMipSubLevels();
				gpuSurf0->Release();
				tex->SwapToMipmappedGpuTex(mipTex, gen);
				return;
			}
			gpuSurf0->Release();
		}
	}
	mipTex->Release();
}

static bool ApplyCompressedMipToTexture(const ReadyMipJob &job)
{
	IDirect3DTexture9 *source = nullptr;
	if (FAILED(job.devEx->CreateTexture(job.width, job.height, 0, 0, job.fmt, D3DPOOL_SYSTEMMEM, &source, nullptr)))
		return false;

	const UINT levels = source->GetLevelCount();
	size_t offset = 0;
	UINT w = job.width, h = job.height;
	bool uploaded = true;
	for (UINT level = 0; level < levels; ++level) {
		const UINT pitch = ((w + 3) / 4) * DxtBlockBytes(job.fmt);
		const UINT rows = (h + 3) / 4;
		const size_t size = (size_t)pitch * rows;
		D3DLOCKED_RECT rect = {};
		if (offset + size > job.compressed.size() || FAILED(source->LockRect(level, &rect, nullptr, 0))) {
			uploaded = false;
			break;
		}
		if (!rect.pBits || rect.Pitch < (INT)pitch) {
			source->UnlockRect(level);
			uploaded = false;
			break;
		}
		for (UINT row = 0; row < rows; ++row)
			memcpy((uint8_t *)rect.pBits + (size_t)row * rect.Pitch, job.compressed.data() + offset + (size_t)row * pitch, pitch);
		source->UnlockRect(level);
		offset += size;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	IDirect3DTexture9 *dest = nullptr;
	uploaded = uploaded && offset == job.compressed.size() && SUCCEEDED(job.devEx->CreateTexture(job.width, job.height, levels,
	           0, job.fmt, D3DPOOL_DEFAULT, &dest, nullptr)) && SUCCEEDED(job.devEx->UpdateTexture(source, dest));
	source->Release();
	if (uploaded) {
		job.tex->SwapToMipmappedGpuTex(dest, job.gen);
		static bool logged = false;
		if (!logged) { logged = true; Log("Mipmaps: compressed DXT chain uploaded to GPU"); }
	} else if (dest) {
		dest->Release();
	}
	return uploaded;
}

void DrainAsyncMipUploads(int maxPerFrame)
{
	if (maxPerFrame <= 0) maxPerFrame = 4;
	for (int n = 0; n < maxPerFrame; ++n) {
		ReadyMipJob job;
		AcquireSRWLockExclusive(&g_mipLock);
		if (g_mipReady.empty()) {
			ReleaseSRWLockExclusive(&g_mipLock);
			break;
		}
		job = std::move(g_mipReady.front());
		g_mipReady.pop_front();
		ReleaseSRWLockExclusive(&g_mipLock);

		if (!job.compressed.empty() && job.tex->HasExternalRefs() && job.tex->CanApplyMip(job.gen)) {
			if (!ApplyCompressedMipToTexture(job)) {
				PendingDxtJob fallback;
				fallback.tex = job.tex;
				fallback.devEx = job.devEx;
				fallback.width = job.width;
				fallback.height = job.height;
				fallback.fmt = job.fmt;
				fallback.srcPitch = (int)(((job.width + 3) / 4) * DxtBlockBytes(job.fmt));
				fallback.gen = job.gen;
				job.compressed.resize(DxtLevelBytes(job.fmt, job.width, job.height));
				fallback.dxtBytes = std::move(job.compressed);
				EnqueueAsyncDxtMip(std::move(fallback));
				continue;
			}
		}
		ApplyDecodedMipToTexture(job.tex, job.devEx, job.width, job.height, job.gen, job.bgra8);
		ReleaseManagedTexRef(job.tex);
	}
}

void ReleaseManagedMipScratch()
{
	if (g_mipScratchSurf) {
		g_mipScratchSurf->Release();
		g_mipScratchSurf = nullptr;
	}
	g_mipScratchDev = nullptr;
	g_mipScratchW = g_mipScratchH = 0;
}

void ShutdownManagedMipWorker()
{
	HANDLE toJoin[2] = {};
	AcquireSRWLockExclusive(&g_mipLock);
	g_mipWorkersStop = true;
	for (int i = 0; i < 2; ++i) {
		toJoin[i] = g_mipThreads[i];
		g_mipThreads[i] = nullptr;
	}
	g_mipWorkersStarted = false;
	WakeAllConditionVariable(&g_mipCv);
	ReleaseSRWLockExclusive(&g_mipLock);

	for (int i = 0; i < 2; ++i) {
		if (toJoin[i]) {
			WaitForSingleObject(toJoin[i], 2000);
			CloseHandle(toJoin[i]);
		}
	}

	// Safely release all remaining ManagedTexture9 references on main thread
	std::deque<PendingDxtJob> remPend;
	std::deque<ReadyMipJob>   remReady;
	AcquireSRWLockExclusive(&g_mipLock);
	remPend.swap(g_mipPending);
	remReady.swap(g_mipReady);
	ReleaseSRWLockExclusive(&g_mipLock);

	for (auto &j : remPend)  ReleaseManagedTexRef(j.tex);
	for (auto &j : remReady) ReleaseManagedTexRef(j.tex);
	ReleaseManagedMipScratch();
}
