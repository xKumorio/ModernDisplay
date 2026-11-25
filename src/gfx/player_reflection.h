#pragma once
#include <d3d9.h>

class PlayerReflection
{
  public:
	void Create(IDirect3DDevice9 *device);
	void Release();
	bool Begin(IDirect3DDevice9 *device, const float ground[4]);
	bool Resume(IDirect3DDevice9 *device);
	bool Bind(IDirect3DDevice9 *device);
	bool CopyToSlot(IDirect3DDevice9 *device, int slot) const;
	void Restore(IDirect3DDevice9 *device, HRESULT drawStatus);
	bool End();
	HANDLE handles[2] = {};
	float crop[4] = {};

  private:
	bool SaveOutputs(IDirect3DDevice9 *device);
	int MatrixRegister(IDirect3DVertexShader9 *shader);
	IDirect3DPixelShader9 *m_materialShader = nullptr, *m_oldPixelShader = nullptr;
	struct ShaderMatrix {
		IDirect3DVertexShader9 *shader = nullptr;
		int firstConstant = -1;
	};
	ShaderMatrix m_matrices[8] = {};
	UINT m_nextMatrix = 0, m_targetCount = 1;
	int m_currentRegister = -1;
	IDirect3DSurface9 *m_oldRT = nullptr, *m_oldDS = nullptr;
	D3DVIEWPORT9 m_oldViewport = {};
	D3DMATRIX m_clipTransform = {}, m_projection = {}, m_view = {}, m_mirrorProjection = {};
	float m_oldConstants[4][4] = {};
	DWORD m_cull = 0, m_scissor = 0, m_colorMask = 0, m_stencil = 0, m_zEnable = 0, m_zFunc = 0;
	bool m_drawn = false, m_failed = false, m_attachment = false;
	static constexpr UINT kSize = 512;
	IDirect3DSurface9 *m_surface[2] = {};
	IDirect3DSurface9 *m_target = nullptr, *m_depth = nullptr;
};
