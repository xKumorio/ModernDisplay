#include "player_reflection.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "player_material_bc.h"

void PlayerReflection::Release()
{
	if (m_materialShader)
		m_materialShader->Release();
	m_materialShader = nullptr;
	for (auto &entry : m_matrices) {
		if (entry.shader)
			entry.shader->Release();
		entry = {};
	}
	for (int i = 0; i < 2; ++i) {
		if (m_surface[i])
			m_surface[i]->Release();
		m_surface[i] = nullptr;
		handles[i] = nullptr;
	}
	if (m_target)
		m_target->Release();
	m_target = nullptr;
	if (m_depth)
		m_depth->Release();
	m_depth = nullptr;
}

void PlayerReflection::Create(IDirect3DDevice9 *device)
{
	D3DCAPS9 caps = {};
	if (SUCCEEDED(device->GetDeviceCaps(&caps))) m_targetCount = caps.NumSimultaneousRTs;
	if (FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD *>(kPlayerMaterialPS), &m_materialShader)))
		return;
	for (int i = 0; i < 2; ++i) {
		IDirect3DTexture9 *texture = nullptr;
		if (FAILED(device->CreateTexture(kSize, kSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
										 &texture, &handles[i]))) {
			Release();
			return;
		}
		HRESULT hr = texture->GetSurfaceLevel(0, &m_surface[i]);
		texture->Release();
		if (FAILED(hr) || !handles[i]) {
			Release();
			return;
		}
	}
	if (FAILED(device->CreateRenderTarget(kSize, kSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &m_target,
										  nullptr)) ||
		FAILED(device->CreateDepthStencilSurface(kSize, kSize, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &m_depth,
												 nullptr)))
		Release();
}

// Crop the normal game projection around the reflected skeleton. Padding allows
// animation to extend beyond the standing pose without clipping the silhouette.
static bool CropActor(const D3DMATRIX &view, const D3DMATRIX &projection, const float ground[4], float crop[4])
{
	float loX = 1, loY = 1, hiX = 0, hiY = 0;
	for (unsigned corner = 0; corner < 8; ++corner) {
		float x = ground[0] + ((corner & 1) ? 1.12f : -1.12f);
		float y = ground[1] + ((corner & 2) ? 1.12f : -1.12f);
		float z = 2 * ground[3] - (ground[2] + ((corner & 4) ? 1.45f : -1.45f));
		float vx = x * view._11 + y * view._21 + z * view._31 + view._41;
		float vy = x * view._12 + y * view._22 + z * view._32 + view._42;
		float vz = x * view._13 + y * view._23 + z * view._33 + view._43;
		if (vz < 0.35f) {
			loX = loY = 0;
			hiX = hiY = 1;
			break;
		}
		float u = .5f + .5f * (vx * projection._11 / vz + projection._31);
		float v = .5f - .5f * (vy * projection._22 / vz + projection._32);
		loX = std::min(loX, u);
		hiX = std::max(hiX, u);
		loY = std::min(loY, v);
		hiY = std::max(hiY, v);
	}
	crop[0] = std::max(0.f, loX);
	crop[1] = std::max(0.f, loY);
	crop[2] = std::min(1.f, hiX) - crop[0];
	crop[3] = std::min(1.f, hiY) - crop[1];
	return crop[2] > .002f && crop[3] > .002f;
}

bool PlayerReflection::Begin(IDirect3DDevice9 *device, const float ground[4])
{
	D3DMATRIX view;
	if (!m_depth || FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
		FAILED(device->GetTransform(D3DTS_PROJECTION, &m_projection)))
		return false;
	const auto &p = m_projection;
	if (std::fabs(p._34 - 1) > .0001f || std::fabs(p._44) > .0001f || std::fabs(p._43) < .001f ||
		std::fabs(p._11) < .1f || std::fabs(p._22) < .1f || !CropActor(view, p, ground, crop))
		return false;
	// Reflect in view space. The normal is world up expressed in this camera.
	const float n[3] = {view._31, view._32, view._33};
	const float plane = ground[3] + view._41 * n[0] + view._42 * n[1] + view._43 * n[2];
	for (int i = 0; i < 4; ++i) {
		float z = float(i == 3), h = (float(i == 2) - p._33 * z) / p._43;
		float x = (float(i == 0) - p._31 * z) / p._11, y = (float(i == 1) - p._32 * z) / p._22;
		float twice = 2 * (x * n[0] + y * n[1] + z * n[2] - h * plane);
		x -= twice * n[0];
		y -= twice * n[1];
		z -= twice * n[2];
		m_clipTransform.m[i][0] = (x * p._11 + z * (p._31 + 1 - 2 * crop[0] - crop[2])) / crop[2];
		m_clipTransform.m[i][1] = (y * p._22 + z * (p._32 + 2 * crop[1] + crop[3] - 1)) / crop[3];
		m_clipTransform.m[i][2] = z * p._33 + h * p._43;
		m_clipTransform.m[i][3] = z;
	}
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			double sum = 0;
			for (int k = 0; k < 4; ++k)
				sum += double(p.m[i][k]) * m_clipTransform.m[k][j];
			m_mirrorProjection.m[i][j] = (float)sum;
		}
	m_view = view;
	m_attachment = false;
	m_failed = m_drawn = false;
	return SaveOutputs(device);
}

bool PlayerReflection::Resume(IDirect3DDevice9 *device)
{
	D3DMATRIX view, projection;
	if (!m_drawn || m_failed || FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
		FAILED(device->GetTransform(D3DTS_PROJECTION, &projection)) ||
		memcmp(&view, &m_view, sizeof(view)) || memcmp(&projection, &m_projection, sizeof(projection)))
		return false;
	m_attachment = true;
	return SaveOutputs(device);
}

bool PlayerReflection::SaveOutputs(IDirect3DDevice9 *device)
{
	// Leave third-party multi-target passes entirely under their renderer's control.
	for (DWORD slot = 1; slot < m_targetCount; ++slot) {
		IDirect3DSurface9 *other = nullptr;
		if (SUCCEEDED(device->GetRenderTarget(slot, &other)) && other) {
			other->Release();
			return false;
		}
	}
	if (FAILED(device->GetViewport(&m_oldViewport)) || FAILED(device->GetRenderTarget(0, &m_oldRT)))
		return false;
	HRESULT hr = device->GetDepthStencilSurface(&m_oldDS);
	if (FAILED(hr) && hr != D3DERR_NOTFOUND) {
		m_oldRT->Release();
		m_oldRT = nullptr;
		return false;
	}
	return true;
}

int PlayerReflection::MatrixRegister(IDirect3DVertexShader9 *shader)
{
	for (const auto &entry : m_matrices)
		if (entry.shader == shader)
			return entry.firstConstant;
	auto &entry = m_matrices[m_nextMatrix++ % ARRAYSIZE(m_matrices)];
	if (entry.shader)
		entry.shader->Release();
	entry.shader = shader;
	entry.firstConstant = -1;
	shader->AddRef();
	UINT bytes = 0;
	if (FAILED(shader->GetFunction(nullptr, &bytes)) || bytes < 20 || bytes > 65536)
		return -1;
	std::vector<DWORD> code(bytes / sizeof(DWORD));
	if (FAILED(shader->GetFunction(code.data(), &bytes)) || D3DSHADER_VERSION_MAJOR(code[0]) != 2)
		return -1;
	// RenderWare's skinned VS2 transforms the already skinned position with
	// m4x4 oPos, rN, cN. Leave the bone palette and all lighting constants intact.
	for (size_t i = 1; i < code.size();) {
		DWORD op = code[i] & D3DSI_OPCODE_MASK;
		if (op == D3DSIO_END)
			break;
		if (op == D3DSIO_COMMENT) {
			i += 1 + ((code[i] >> 16) & 0x7fff);
			continue;
		}
		UINT len = (code[i] & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
		if (i + len >= code.size()) { entry.firstConstant = -1; break; }
		if (len && (code[i + 1] & (D3DSP_REGTYPE_MASK | D3DSP_REGTYPE_MASK2 | D3DSP_REGNUM_MASK)) == 0x40000000u) {
			if (code[i] != 0x03000014 || code[i + 1] != 0xc00f0000 || (code[i + 3] & ~0x7ffu) != 0xa0e40000u ||
				entry.firstConstant >= 0) {
				entry.firstConstant = -1;
				break;
			}
			entry.firstConstant = code[i + 3] & 0x7ff;
		}
		i += len + 1;
	}
	return entry.firstConstant;
}

bool PlayerReflection::Bind(IDirect3DDevice9 *device)
{
	if (m_failed)
		return false;
	IDirect3DVertexShader9 *shader = nullptr;
	if (FAILED(device->GetVertexShader(&shader))) {
		m_failed = true;
		return false;
	}
	m_currentRegister = -1;
	if (shader) {
		m_currentRegister = MatrixRegister(shader);
		shader->Release();
		if (m_currentRegister < 0 ||
			FAILED(device->GetVertexShaderConstantF(m_currentRegister, &m_oldConstants[0][0], 4))) {
			m_failed = true;
			return false;
		}
	}
	if (FAILED(device->GetRenderState(D3DRS_CULLMODE, &m_cull)) ||
		FAILED(device->GetRenderState(D3DRS_SCISSORTESTENABLE, &m_scissor)) ||
		FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &m_colorMask)) ||
		FAILED(device->GetRenderState(D3DRS_STENCILENABLE, &m_stencil)) ||
		FAILED(device->GetRenderState(D3DRS_ZENABLE, &m_zEnable)) ||
		FAILED(device->GetRenderState(D3DRS_ZFUNC, &m_zFunc)) || FAILED(device->GetPixelShader(&m_oldPixelShader))) {
		m_failed = true;
		return false;
	}
	device->SetDepthStencilSurface(nullptr);
	D3DVIEWPORT9 vp = {0, 0, kSize, kSize, 0, 1};
	bool ok = SUCCEEDED(device->SetRenderTarget(0, m_target)) && SUCCEEDED(device->SetDepthStencilSurface(m_depth)) &&
			  SUCCEEDED(device->SetViewport(&vp));
	if (ok && !m_drawn)
		ok = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1, 0));
	if (ok && m_currentRegister >= 0) {
		float constants[4][4];
		for (int j = 0; j < 4; ++j)
			for (int i = 0; i < 4; ++i) {
				double sum = 0;
				for (int k = 0; k < 4; ++k)
					sum += double(m_oldConstants[k][i]) * m_clipTransform.m[k][j];
				constants[j][i] = (float)sum;
			}
		ok = SUCCEEDED(device->SetVertexShaderConstantF(m_currentRegister, &constants[0][0], 4));
	} else if (ok)
		ok = SUCCEEDED(device->SetTransform(D3DTS_PROJECTION, &m_mirrorProjection));
	if (ok && (m_currentRegister >= 0 || m_attachment) && !m_oldPixelShader) {
		IDirect3DBaseTexture9 *skin = nullptr;
		if (SUCCEEDED(device->GetTexture(0, &skin)) && skin) {
			ok = SUCCEEDED(device->SetPixelShader(m_materialShader));
			skin->Release();
		}
	}
	if (!ok) {
		Restore(device, E_FAIL);
		return false;
	}
	device->SetRenderState(D3DRS_CULLMODE, m_cull == D3DCULL_CW	   ? D3DCULL_CCW
										   : m_cull == D3DCULL_CCW ? D3DCULL_CW
																   : D3DCULL_NONE);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	return true;
}

void PlayerReflection::Restore(IDirect3DDevice9 *device, HRESULT drawStatus)
{
	device->SetPixelShader(m_oldPixelShader);
	if (m_oldPixelShader)
		m_oldPixelShader->Release();
	m_oldPixelShader = nullptr;
	if (m_currentRegister >= 0)
		device->SetVertexShaderConstantF(m_currentRegister, &m_oldConstants[0][0], 4);
	else
		device->SetTransform(D3DTS_PROJECTION, &m_projection);
	device->SetRenderState(D3DRS_CULLMODE, m_cull);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, m_scissor);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, m_colorMask);
	device->SetRenderState(D3DRS_STENCILENABLE, m_stencil);
	device->SetRenderState(D3DRS_ZENABLE, m_zEnable);
	device->SetRenderState(D3DRS_ZFUNC, m_zFunc);
	device->SetDepthStencilSurface(nullptr);
	device->SetRenderTarget(0, m_oldRT);
	device->SetDepthStencilSurface(m_oldDS);
	device->SetViewport(&m_oldViewport);
	m_failed = FAILED(drawStatus);
	m_drawn = SUCCEEDED(drawStatus);
}

bool PlayerReflection::End()
{
	if (m_oldDS)
		m_oldDS->Release();
	if (m_oldRT)
		m_oldRT->Release();
	m_oldRT = m_oldDS = nullptr;
	return m_drawn && !m_failed;
}

bool PlayerReflection::CopyToSlot(IDirect3DDevice9 *device, int slot) const
{
	return SUCCEEDED(device->StretchRect(m_target, nullptr, m_surface[slot], nullptr, D3DTEXF_NONE));
}
