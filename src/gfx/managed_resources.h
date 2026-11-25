// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <d3d9.h>

extern volatile LONG g_dirtyManagedCount;

static inline bool HasDirtyManagedResources()
{
	return g_dirtyManagedCount > 0;
}

typedef void (*ManagedTexDestroyCb_t)(IDirect3DBaseTexture9 *rawTex);
void SetManagedTexDestroyCallback(ManagedTexDestroyCb_t cb);

HRESULT CreateManagedTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
                              UINT width, UINT height, UINT levels, DWORD usage,
                              D3DFORMAT format, IDirect3DTexture9 **outTex);

HRESULT CreateManagedCubeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
                                  UINT edgeLength, UINT levels, DWORD usage,
                                  D3DFORMAT format, IDirect3DCubeTexture9 **outTex);

HRESULT CreateManagedVolumeTexture9(IDirect3DDevice9Ex *devEx, IDirect3DDevice9 *proxyDev,
                                    UINT width, UINT height, UINT depth, UINT levels, DWORD usage,
                                    D3DFORMAT format, IDirect3DVolumeTexture9 **outTex);

IDirect3DBaseTexture9 *UnwrapBaseTextureForGpu(IDirect3DBaseTexture9 *tex, bool *outIsManaged = nullptr);
IDirect3DBaseTexture9 *UnwrapBaseTextureForSys(IDirect3DBaseTexture9 *tex);
void                   SyncManagedBaseTexture(IDirect3DBaseTexture9 *tex);
void                   SyncAllDirtyManagedResources();
void                   DrainAsyncMipUploads(int maxPerFrame = 4);
void                   ReleaseManagedMipScratch();
void                   ShutdownManagedMipWorker();
IDirect3DSurface9     *UnwrapSurfaceForGpu(IDirect3DSurface9 *surf);
IDirect3DSurface9     *UnwrapSurfaceForSys(IDirect3DSurface9 *surf);
void                   MarkSurfaceDirtyIfManaged(IDirect3DSurface9 *surf);
