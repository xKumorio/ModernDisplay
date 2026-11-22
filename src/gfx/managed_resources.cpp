// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "managed_resources.h"
#include "../core/log.h"

static int g_texErrLogs = 0;

static const void *g_vtblManagedTex = nullptr;
static const void *g_vtblManagedCube = nullptr;
static const void *g_vtblManagedVol = nullptr;
static const void *g_vtblManagedSurf = nullptr;

class IManagedParent {
public:
	virtual void MarkDirty() = 0;
	virtual void SyncGpu() = 0;
	virtual void OnUnlock() = 0;
	virtual IUnknown *ParentUnknown() = 0;
};

class ManagedSurface9 final : public IDirect3DSurface9 {
public:
	ManagedSurface9(IDirect3DDevice9 *proxyDev, IManagedParent *parent,
	                IDirect3DSurface9 *sysSurf, IDirect3DSurface9 *gpuSurf)
		: m_proxyDev(proxyDev), m_parent(parent), m_sysSurf(sysSurf), m_gpuSurf(gpuSurf), m_ref(1)
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
	void MarkDirty() { m_parent->MarkDirty(); }

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
		HRESULT hr = m_sysSurf->LockRect(pLockedRect, pRect, Flags);
		if (SUCCEEDED(hr) && !(Flags & D3DLOCK_READONLY)) m_parent->MarkDirty();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect() override
	{
		HRESULT hr = m_sysSurf->UnlockRect();
		m_parent->OnUnlock();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override { m_parent->MarkDirty(); return m_sysSurf->GetDC(phdc); }
	HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override
	{
		HRESULT hr = m_sysSurf->ReleaseDC(hdc);
		m_parent->OnUnlock();
		return hr;
	}

private:
	IDirect3DDevice9  *m_proxyDev;
	IManagedParent    *m_parent;
	IDirect3DSurface9 *m_sysSurf;
	IDirect3DSurface9 *m_gpuSurf;
	volatile LONG      m_ref;
};

class ManagedTexture9 final : public IDirect3DTexture9, public IManagedParent {
public:
	ManagedTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
	                IDirect3DTexture9 *sysTex, IDirect3DTexture9 *gpuTex)
		: m_devEx(devEx), m_proxyDev(proxyDev), m_sysTex(sysTex), m_gpuTex(gpuTex), m_ref(1), m_dirty(true)
	{
		if (!g_vtblManagedTex) g_vtblManagedTex = *(void *const *)(IDirect3DTexture9 *)this;
	}
	~ManagedTexture9()
	{
		if (m_sysTex) m_sysTex->Release();
		if (m_gpuTex) m_gpuTex->Release();
	}

	void MarkDirty() override { m_dirty = true; }
	void SyncGpu() override
	{
		if (m_dirty) {
			m_dirty = false;
			m_syncedOnce = true;
			// Sub-surface LockRect (GetSurfaceLevel -> LockRect, used by D3DX & RenderWare)
			// does not mark the parent texture dirty in D3D9Ex; explicitly dirty it before UpdateTexture.
			m_sysTex->AddDirtyRect(nullptr);
			HRESULT hr = m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
			if (FAILED(hr) && g_texErrLogs < 16) {
				++g_texErrLogs;
				D3DSURFACE_DESC d = {};
				m_sysTex->GetLevelDesc(0, &d);
				Log("D3D9Ex: UpdateTexture failed hr=0x%08lX (%ux%u fmt=%u lvls=%u)", hr, d.Width, d.Height, (unsigned)d.Format, m_sysTex->GetLevelCount());
			}
		}
	}
	void OnUnlock() override
	{
		if (m_syncedOnce && m_dirty) SyncGpu();
	}
	IUnknown *ParentUnknown() override { return static_cast<IDirect3DTexture9 *>(this); }
	IDirect3DTexture9 *GpuTex() { SyncGpu(); return m_gpuTex; }
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
		*ppSurfaceLevel = new ManagedSurface9(m_proxyDev, this, sysS, gpuS);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
	{
		HRESULT hr = m_sysTex->LockRect(Level, pLockedRect, pRect, Flags);
		if (SUCCEEDED(hr) && !(Flags & D3DLOCK_READONLY)) m_dirty = true;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override
	{
		HRESULT hr = m_sysTex->UnlockRect(Level);
		OnUnlock();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT *pDirtyRect) override { m_dirty = true; return m_sysTex->AddDirtyRect(pDirtyRect); }

private:
	IDirect3DDevice9Ex *m_devEx;
	IDirect3DDevice9   *m_proxyDev;
	IDirect3DTexture9  *m_sysTex;
	IDirect3DTexture9  *m_gpuTex;
	volatile LONG       m_ref;
	bool                m_dirty;
	bool                m_syncedOnce = false;
};

class ManagedCubeTexture9 final : public IDirect3DCubeTexture9, public IManagedParent {
public:
	ManagedCubeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
	                    IDirect3DCubeTexture9 *sysTex, IDirect3DCubeTexture9 *gpuTex)
		: m_devEx(devEx), m_proxyDev(proxyDev), m_sysTex(sysTex), m_gpuTex(gpuTex), m_ref(1), m_dirty(true)
	{
		if (!g_vtblManagedCube) g_vtblManagedCube = *(void *const *)(IDirect3DCubeTexture9 *)this;
	}
	~ManagedCubeTexture9()
	{
		if (m_sysTex) m_sysTex->Release();
		if (m_gpuTex) m_gpuTex->Release();
	}

	void MarkDirty() override { m_dirty = true; }
	void SyncGpu() override
	{
		if (m_dirty) {
			m_dirty = false;
			m_syncedOnce = true;
			for (int f = 0; f < 6; ++f)
				m_sysTex->AddDirtyRect((D3DCUBEMAP_FACES)f, nullptr);
			m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
		}
	}
	void OnUnlock() override
	{
		if (m_syncedOnce && m_dirty) SyncGpu();
	}
	IUnknown *ParentUnknown() override { return static_cast<IDirect3DCubeTexture9 *>(this); }
	IDirect3DCubeTexture9 *GpuTex() { SyncGpu(); return m_gpuTex; }
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
		*ppCubeMapSurface = new ManagedSurface9(m_proxyDev, this, sysS, gpuS);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
	{
		HRESULT hr = m_sysTex->LockRect(FaceType, Level, pLockedRect, pRect, Flags);
		if (SUCCEEDED(hr) && !(Flags & D3DLOCK_READONLY)) m_dirty = true;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override
	{
		HRESULT hr = m_sysTex->UnlockRect(FaceType, Level);
		OnUnlock();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, CONST RECT *pDirtyRect) override { m_dirty = true; return m_sysTex->AddDirtyRect(FaceType, pDirtyRect); }

private:
	IDirect3DDevice9Ex    *m_devEx;
	IDirect3DDevice9      *m_proxyDev;
	IDirect3DCubeTexture9 *m_sysTex;
	IDirect3DCubeTexture9 *m_gpuTex;
	volatile LONG          m_ref;
	bool                   m_dirty;
	bool                   m_syncedOnce = false;
};

class ManagedVolumeTexture9 final : public IDirect3DVolumeTexture9 {
public:
	ManagedVolumeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
	                      IDirect3DVolumeTexture9 *sysTex, IDirect3DVolumeTexture9 *gpuTex)
		: m_devEx(devEx), m_proxyDev(proxyDev), m_sysTex(sysTex), m_gpuTex(gpuTex), m_ref(1), m_dirty(true)
	{
		if (!g_vtblManagedVol) g_vtblManagedVol = *(void *const *)this;
	}
	~ManagedVolumeTexture9()
	{
		if (m_sysTex) m_sysTex->Release();
		if (m_gpuTex) m_gpuTex->Release();
	}

	void SyncGpu()
	{
		if (m_dirty) {
			m_dirty = false;
			m_sysTex->AddDirtyBox(nullptr);
			m_devEx->UpdateTexture(m_sysTex, m_gpuTex);
		}
	}
	IDirect3DVolumeTexture9 *GpuTex() { SyncGpu(); return m_gpuTex; }
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
	HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) override { m_dirty = true; return m_sysTex->GetVolumeLevel(Level, ppVolumeLevel); }
	HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags) override
	{
		HRESULT hr = m_sysTex->LockBox(Level, pLockedVolume, pBox, Flags);
		if (SUCCEEDED(hr) && !(Flags & D3DLOCK_READONLY)) m_dirty = true;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) override { return m_sysTex->UnlockBox(Level); }
	HRESULT STDMETHODCALLTYPE AddDirtyBox(CONST D3DBOX *pDirtyBox) override { m_dirty = true; return m_sysTex->AddDirtyBox(pDirtyBox); }

private:
	IDirect3DDevice9Ex      *m_devEx;
	IDirect3DDevice9        *m_proxyDev;
	IDirect3DVolumeTexture9 *m_sysTex;
	IDirect3DVolumeTexture9 *m_gpuTex;
	volatile LONG            m_ref;
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
	hr = devEx->CreateTexture(width, height, levels, gpuUsage, format, D3DPOOL_DEFAULT, &gpuTex, nullptr);
	if (FAILED(hr)) {
		if (g_texErrLogs < 16) {
			++g_texErrLogs;
			Log("D3D9Ex: CreateManagedTexture gpu failed hr=0x%08lX (%ux%u lvls=%u usage=0x%lX fmt=%u)", hr, width, height, levels, usage, (unsigned)format);
		}
		sysTex->Release();
		return hr;
	}
	*outTex = new ManagedTexture9(devEx, proxyDev, sysTex, gpuTex);
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

IDirect3DBaseTexture9 *UnwrapBaseTextureForGpu(IDirect3DBaseTexture9 *tex)
{
	if (!tex) return nullptr;
	const void *vt = *(void *const *)tex;
	if (vt == g_vtblManagedTex)  return static_cast<ManagedTexture9 *>(tex)->GpuTex();
	if (vt == g_vtblManagedCube) return static_cast<ManagedCubeTexture9 *>(tex)->GpuTex();
	if (vt == g_vtblManagedVol)  return static_cast<ManagedVolumeTexture9 *>(tex)->GpuTex();
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
