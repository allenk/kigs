#include "RendererDX11.h"

#include "Core.h"
#include "ModuleRenderer.h"
#include "FreeType_TextDrawer.h"
#include "TravState.h"

#include "UIVerticesInfo.h"

#include "DX11RenderingScreen.h"
#include "DX11Texture.h"
#include "DX11Camera.h"
#include "DX11Material.h"
#include "DX11CameraOrtho.h"

#include "HLSLLight.h"
#include "HLSLShader.h"
#include "HLSLUIShader.h"
#include "HLSLCutShader.h"
#include "HLSLGenericMeshShader.h"
#include "HLSLUniform.h"

#include "ModernMesh.h"

#include "GLSLDebugDraw.h"

#include <dxgi.h>
#include <d3dcommon.h>
#include <d3d11.h>
#include "Platform/Renderer/PlatformRendererDX11.inl.h"
#include "Crc32.h"
#include "RendererDefines.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#ifdef _KIGS_ONLY_STATIC_LIB_
#define MODULEINITFUNC			PlatformRendererModuleInit
extern ModuleBase*				MODULEINITFUNC(KigsCore* core, const kstl::vector<CoreModifiableAttribute*>* params);
#else
#define MODULEINITFUNC			ModuleInit
#endif

// ## Static object initialization
ModuleSpecificRenderer *	RendererDX11::theGlobalRenderer = NULL;
FreeType_TextDrawer*		RendererDX11::myDrawer = NULL;

unsigned int				RendererDX11::myDirtyShaderMatrix = 0;

IMPLEMENT_CLASS_INFO(RendererDX11)

IMPLEMENT_CONSTRUCTOR(RendererDX11)
{
}

ModuleSpecificRenderer::LightCount RendererDX11::SetLightsInfo(kstl::set<CoreModifiable*>*lights)
{
	int newNumberOfDirectLights = 0;
	int newNumberOfPointLights = 0;
	int newNumberOfSpotLights = 0;

	for (auto it : *lights)
	{
		API3DLight* light = static_cast<API3DLight*>(it);
		if (light->getIsDeffered() || !light->getIsOn())
			continue;

		switch (light->GetTypeOfLight())
		{
		case DIRECTIONAL_LIGHT:
			++newNumberOfDirectLights;
			break;
		case POINT_LIGHT:
			++newNumberOfPointLights;
			break;
		case SPOT_LIGHT:
			++newNumberOfSpotLights;
			break;
		default:
			break;
		}
	}

	LightCount  count;
	count.dir = newNumberOfDirectLights;
	count.point = newNumberOfPointLights;
	count.spot = newNumberOfSpotLights;
	return count;
}

void RendererDX11::SendLightsInfo(TravState* travstate)
{
	if (travstate->myLights == nullptr)
		return;

	Camera*	cam = travstate->GetCurrentCamera();
	v3f cam_pos(0,0,0);
	if (cam)
	{
		cam_pos = cam->GetGlobalPosition();
	}
	LightStruct lights;
	for (auto it : *travstate->myLights)
	{
		API3DLight* light = static_cast<API3DLight*>(it);
		light->PrepareLightInfo(lights, cam);
	}
	
	size_t needed_size = sizeof(v4f) + lights.pointlights.size() * sizeof(PointLight) + lights.dirlights.size() * sizeof(DirLight) + lights.spotlights.size() * sizeof(SpotLight);
	
	if (needed_size > myDXInstance.m_currentLightBufferSize)
	{
		myDXInstance.m_lightBuffer->Release();
		//myDXInstance.m_lightBuffer = nullptr;
		D3D11_BUFFER_DESC lightBufferDesc;
		lightBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
		lightBufferDesc.ByteWidth = myDXInstance.m_currentLightBufferSize = needed_size;
		if (lightBufferDesc.ByteWidth % 16 != 0) lightBufferDesc.ByteWidth = (lightBufferDesc.ByteWidth / 16 + 1) * 16;
		lightBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		lightBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		lightBufferDesc.MiscFlags = 0;
		lightBufferDesc.StructureByteStride = 0;
		DX::ThrowIfFailed(myDXInstance.m_device->CreateBuffer(&lightBufferDesc, NULL, &myDXInstance.m_lightBuffer));
	}


	size_t hash = 0;
	hash_combine(hash, cam_pos, lights);

	/*u32 crc = 0;
	crc = crc32_bitwise(&cam_pos, sizeof(v3f), crc);
	crc = crc32_bitwise(lights.pointlights.data(), lights.pointlights.size() * sizeof(PointLight), crc);
	crc = crc32_bitwise(lights.dirlights.data(), lights.dirlights.size() * sizeof(DirLight), crc);
	crc = crc32_bitwise(lights.spotlights.data(), lights.spotlights.size() * sizeof(SpotLight), crc);*/
	
	if (myDXInstance.m_currentLightBufferHash != hash)
	{
		myDXInstance.m_currentLightBufferHash = hash;
		D3D11_MAPPED_SUBRESOURCE mappedResource;
		DX::ThrowIfFailed(myDXInstance.m_deviceContext->Map(myDXInstance.m_lightBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource));
		u8* data_ptr = (u8*)mappedResource.pData;
		*(v3f*)data_ptr = cam_pos;
		data_ptr += sizeof(v4f);

		memcpy(data_ptr, lights.pointlights.data(), lights.pointlights.size() * sizeof(PointLight));
		data_ptr += lights.pointlights.size() * sizeof(PointLight);
		memcpy(data_ptr, lights.dirlights.data(), lights.dirlights.size() * sizeof(DirLight));
		data_ptr += lights.dirlights.size() * sizeof(DirLight);
		memcpy(data_ptr, lights.spotlights.data(), lights.spotlights.size() * sizeof(SpotLight));

		myDXInstance.m_deviceContext->Unmap(myDXInstance.m_lightBuffer, 0);
	}
	myDXInstance.m_deviceContext->PSSetConstantBuffers(DX11_LIGHT_SLOT, 1, &myDXInstance.m_lightBuffer);


	for(auto it : *travstate->myLights)
	{
		API3DLight* light = static_cast<API3DLight*>(it);
		light->PreRendering(this, cam, cam_pos);
		light->DrawLight(travstate);
	}
}

void RendererDX11::ClearLightsInfo(TravState* travstate)
{
	if (travstate->myLights == nullptr)
		return;

	auto itr = travstate->myLights->begin();
	auto end = travstate->myLights->end();
	for (; itr != end; ++itr)
	{
		API3DLight* myLight = static_cast<API3DLight*>(*itr);
		myLight->PostDrawLight(travstate);
	}
}

void RendererDX11::FlushState(bool force)
{
	
	ModuleSpecificRenderer::FlushState(force);
}

void RendererDX11::ProtectedFlushMatrix(TravState* state)
{
	D3D11_MAPPED_SUBRESOURCE mappedResource;

	if (HasShader()) // load uniform
	{
		auto locations = GetActiveShader()->GetLocation();
		auto cam = state->GetCurrentCamera();
		if (cam)
		{
			//auto D = myMatrixStack[1].back()[14];
			//auto C = myMatrixStack[1].back()[10];
			struct FogBuffer
			{
				v4f color;
				float far_plane = 100;
				float scale = 1;
			} buffer;

			auto nearPlane = 0.1f; // D / (C - 1.0f);
			auto farPlane = 100.0f; // D / (C + 1.0f);
			cam->getValue("NearPlane", nearPlane);
			cam->getValue("FarPlane", farPlane);
			buffer.far_plane = farPlane;

			
			v4f fog_color(0, 0, 0, 1);
			float fog_scale = (farPlane - nearPlane) / 10;
			cam->getValue("FogColor", fog_color);
			cam->getValue("FogScale", fog_scale);
			
			buffer.color = fog_color;
			buffer.scale = fog_scale;

			DX::ThrowIfFailed(myDXInstance.m_deviceContext->Map(myDXInstance.m_fogBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource));
			*(FogBuffer*)mappedResource.pData = buffer;
			myDXInstance.m_deviceContext->Unmap(myDXInstance.m_fogBuffer, 0);
			myDXInstance.m_deviceContext->VSSetConstantBuffers(DX11_FOG_SLOT, 1, &myDXInstance.m_fogBuffer);
			myDXInstance.m_deviceContext->PSSetConstantBuffers(DX11_FOG_SLOT, 1, &myDXInstance.m_fogBuffer);
		}

		
		MatrixBufferType* dataPtr;

		if ((myDirtyShaderMatrix == 0) && (myDirtyMatrix == 0))
		{
			return;
		}

		int previousMatrixMode = myCurrentMatrixMode;

		// Lock the constant buffer so it can be written to.
		DX::ThrowIfFailed(myDXInstance.m_deviceContext->Map(myDXInstance.m_matrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource));

		// Get a pointer to the data in the constant buffer.
		dataPtr = (MatrixBufferType*)mappedResource.pData;
		dataPtr->model = myMatrixStack[MATRIX_MODE_MODEL].back();
		
		if (state->GetHolographicMode())
		{
			// In holographic mode, instead of view and proj we have two viewprojs, one for each render target
			if (state->HolographicUseStackMatrix)
			{
				auto proj = myMatrixStack[MATRIX_MODE_PROJECTION].back();
				auto view = myMatrixStack[MATRIX_MODE_VIEW].back();
				dataPtr->stereo_viewproj[0] = dataPtr->stereo_viewproj[1] = proj * view;
			}
			else
			{
				auto stereo_view_proj = state->GetCurrentCamera()->GetStereoViewProjections();
				dataPtr->stereo_viewproj[0] = stereo_view_proj[0];
				dataPtr->stereo_viewproj[1] = stereo_view_proj[1];
			}
			
		}
		else
		{
			auto proj = myMatrixStack[MATRIX_MODE_PROJECTION].back();
			if (myDXInstance.m_isFBORenderTarget)
			{
				proj[5] = -proj[5];
				proj[13] = -proj[13];
			}
			auto view = myMatrixStack[MATRIX_MODE_VIEW].back();
			dataPtr->viewproj = proj * view; // .Mult(view, proj);
			//dataPtr->proj.SetIdentity();
		}

		myDirtyShaderMatrix = 0;
		myDirtyMatrix = 0;
		myDXInstance.m_deviceContext->Unmap(myDXInstance.m_matrixBuffer, 0);
		myDXInstance.m_deviceContext->VSSetConstantBuffers(DX11_MATRIX_SLOT, 1, &myDXInstance.m_matrixBuffer);
	}
}

void DX11RenderingState::ProtectedInitHardwareState()
{
}

DX11RenderingState::~DX11RenderingState()
{

}

void RendererDX11::Init(KigsCore* core, const kstl::vector<CoreModifiableAttribute*>* params)
{
	BaseInit(core, "RendererDX11", params);
	DECLARE_FULL_CLASS_INFO(core, DX11RenderingScreen, RenderingScreen, Renderer)

	DECLARE_CLASS_INFO_WITHOUT_FACTORY(DX11Texture, "Texture");
	RegisterClassToInstanceFactory(core, "Renderer", "Texture",
								   [](const kstl::string& instancename, kstl::vector<CoreModifiableAttribute*>* args) -> CoreModifiable *
	{
		if (args && args->size())
		{
			return DX11Texture::CreateInstance(instancename, args);
		}
		SP<TextureFileManager> fileManager = KigsCore::GetSingleton("TextureFileManager");
		SP<Texture> texture = fileManager->GetTexture(instancename, false);
		// texture will be delete when lambda exit ( as only the pointer is returned )
		if (texture)
		{
			texture->GetRef(); // so get a ref before exiting
		}
		return texture.get();
	});

	DECLARE_FULL_CLASS_INFO(core, DX11Camera, Camera, Renderer);
	DECLARE_FULL_CLASS_INFO(core, DX11CameraOrtho, CameraOrtho, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DLight, Light, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DShader, API3DShader, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUIShader, API3DUIShader, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DCutShader, API3DCutShader, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DGenericMeshShader, API3DGenericMeshShader, Renderer);

	DECLARE_FULL_CLASS_INFO(core, API3DUniformInt, API3DUniformInt, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformFloat, API3DUniformFloat, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformFloat2, API3DUniformFloat2, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformFloat3, API3DUniformFloat3, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformFloat4, API3DUniformFloat4, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformTexture, API3DUniformTexture, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformMatrixArray, API3DUniformMatrixArray, Renderer);
	DECLARE_FULL_CLASS_INFO(core, API3DUniformBuffer, API3DUniformBuffer, Renderer);

	DECLARE_FULL_CLASS_INFO(core, DX11Material, Material, Renderer)

	DECLARE_FULL_CLASS_INFO(core, DebugDraw, DebugDraw, Renderer)

	if (!theGlobalRenderer)
		theGlobalRenderer = this;

	//Drawer freetype
	if (!myDrawer)
	{
		myDrawer = new FreeType_TextDrawer();
		myDrawer->startBuildFonts();
	}

	
	ModuleSpecificRenderer::Init(core, params);
	
	CreateDevice();

	PlatformInit(core, params);

	myDefaultUIShader = KigsCore::GetInstanceOf("UIShader", "API3DUIShader");
	myVertexBufferManager = std::make_unique<VertexBufferManager>();
}

bool RendererDX11::CreateDevice()
{
	DXInstance* dxinstance = getDXInstance();
	
	// Set the feature level to DirectX 11.
	static const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };;
	u32 device_creation_flags = 0;
#ifdef KIGS_TOOLS
	device_creation_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
#ifdef WUP
	//if (gIsHolographic)
	{
		device_creation_flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	}
#endif

	dxinstance->m_device = nullptr;
	dxinstance->m_deviceContext = nullptr;

	winrt::com_ptr<ID3D11Device> device;
	winrt::com_ptr<ID3D11DeviceContext> context;
	DX::ThrowIfFailed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_creation_flags, featureLevels, _countof(featureLevels), D3D11_SDK_VERSION, device.put(), nullptr, context.put()));

#ifdef WUP
	auto space = App::GetApp()->GetHolographicSpace();
	if (space)
	{
		winrt::com_ptr<IDXGIDevice1> dxgiDevice = device.as<IDXGIDevice1>();

		// Wrap the native device using a WinRT interop object.
		winrt::com_ptr<::IInspectable> object;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), object.put()));

		auto d3dinteropdevice = object.as<IDirect3DDevice>();
		space.SetDirect3D11Device(d3dinteropdevice);
	}
#endif

	dxinstance->m_device = device.as<ID3D11Device1>();
	dxinstance->m_deviceContext = context.as<ID3D11DeviceContext1>();

#ifdef WUP
	D3D11_FEATURE_DATA_D3D11_OPTIONS3 options;
	dxinstance->m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options, sizeof(options));
	if (!options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer && gIsHolographic)
	{
		KIGS_ASSERT(!"Device doesn't support stereo rendering (VPAndRTArrayIndexFromAnyShaderFeedingRasterizer is FALSE)");
	}
#endif

#ifdef KIGS_TOOLS
	ID3D11Debug* d3dDebug = nullptr;
	if (SUCCEEDED(dxinstance->m_device->QueryInterface(__uuidof(ID3D11Debug), (void**)& d3dDebug)))
	{
		ID3D11InfoQueue* d3dInfoQueue = nullptr;
		if (SUCCEEDED(d3dDebug->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)& d3dInfoQueue)))
		{

#ifndef WUP
			d3dInfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
			d3dInfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);
#endif
			D3D11_MESSAGE_ID hide[] =
			{
			D3D11_MESSAGE_ID_SETPRIVATEDATA_CHANGINGPARAMS,
			// Add more message IDs here as needed
			};

			D3D11_INFO_QUEUE_FILTER filter;
			memset(&filter, 0, sizeof(filter));
			filter.DenyList.NumIDs = _countof(hide);
			filter.DenyList.pIDList = hide;
			d3dInfoQueue->AddStorageFilterEntries(&filter);
			d3dInfoQueue->Release();
		}
		d3dDebug->Release();
	}
#endif

	D3D11_BUFFER_DESC matrixBufferDesc;
	matrixBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	matrixBufferDesc.ByteWidth = sizeof(MatrixBufferType);
	matrixBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	matrixBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	matrixBufferDesc.MiscFlags = 0;
	matrixBufferDesc.StructureByteStride = 0;
	DX::ThrowIfFailed(dxinstance->m_device->CreateBuffer(&matrixBufferDesc, NULL, &dxinstance->m_matrixBuffer));

	D3D11_BUFFER_DESC lightBufferDesc;
	lightBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	lightBufferDesc.ByteWidth = dxinstance->m_currentLightBufferSize = sizeof(v3f) + 2 * sizeof(PointLight);
	if (lightBufferDesc.ByteWidth % 16 != 0) lightBufferDesc.ByteWidth = (lightBufferDesc.ByteWidth / 16 + 1) * 16;
	lightBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	lightBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	lightBufferDesc.MiscFlags = 0;
	lightBufferDesc.StructureByteStride = 0;
	DX::ThrowIfFailed(dxinstance->m_device->CreateBuffer(&lightBufferDesc, NULL, &dxinstance->m_lightBuffer));

	D3D11_BUFFER_DESC fogBufferDesc;
	fogBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	fogBufferDesc.ByteWidth = 32;
	fogBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	fogBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	fogBufferDesc.MiscFlags = 0;
	fogBufferDesc.StructureByteStride = 0;
	DX::ThrowIfFailed(dxinstance->m_device->CreateBuffer(&fogBufferDesc, NULL, &dxinstance->m_fogBuffer));

	return true;
}

void RendererDX11::Close()
{
	if (myDrawer)
	{
		delete myDrawer;
		myDrawer = NULL;
	}

	// Manage queries
	for (int type = 0; type < RENDERER_QUERY_TYPE_COUNT; ++type)
	{
		for (u32 i = 0; i < mOcclusionQueries[type].size(); ++i)
		{
			auto& query = mOcclusionQueries[type][i];
			query.query->Release();
		}
		mOcclusionQueries[type].clear();
	}

	// clear d3d object
	for (auto obj : myBlendStateList)
		obj.second->Release();
	for (auto obj : myDepthStateList)
		obj.second->Release();
	for (auto obj : myRasterizerStateList)
		obj.second->Release();
	for (auto obj : mySamplerStateList)
		obj.second->Release();

	myDefaultUIShader=nullptr;

	ID3D11Debug *d3dDebug = nullptr;
	if (S_OK == myDXInstance.m_device->QueryInterface(__uuidof(ID3D11Debug), (void**)&d3dDebug))
		d3dDebug->Release();

	myDXInstance.m_currentRenderTarget = nullptr;
	myDXInstance.m_currentDepthStencilTarget = nullptr;
	myDXInstance.m_swapChain = nullptr;
	myDXInstance.m_deviceContext = nullptr;
	myDXInstance.m_device = nullptr;
	
	myDXInstance.m_matrixBuffer->Release();
	myDXInstance.m_lightBuffer->Release();
	
	if (myDXInstance.m_materialBuffer) 
		myDXInstance.m_materialBuffer->Release();
	
#ifdef WIN32
	//wglMakeCurrent(NULL, NULL);
#endif
	ModuleSpecificRenderer::Close();
	BaseClose();
}

void RendererDX11::Update(const Timer& timer, void* addParam)
{
	ModuleSpecificRenderer::Update(timer, addParam);

#if 0
	// print allocated buffers size
	kigsprintf("Start Buffer Desc    ***************************************\n");
	// print vbo size
	for (auto vbo : myVBO)
	{
		auto currentB=((VertexBufferManager*)myVertexBufferManager.get())->mBufferList[vbo];
		if (currentB.mDesc)
		{
			kigsprintf("Buffer %d size = %d\n",vbo, currentB.mDesc->ByteWidth);
		}
	}
#endif

}

ModuleBase* MODULEINITFUNC(KigsCore* core, const kstl::vector<CoreModifiableAttribute*>* params)
{
	KigsCore::ModuleStaticInit(core);

	DECLARE_CLASS_INFO_WITHOUT_FACTORY(RendererDX11, "RendererDX11");
	ModuleBase*    gInstanceRendererDX11 = new RendererDX11("RendererDX11");
	gInstanceRendererDX11->Init(core, params);

	return gInstanceRendererDX11;

}

void DX11RenderingState::ClearView(RendererClearMode clearMode)
{
	RendererDX11* renderer = static_cast<RendererDX11*>(ModuleRenderer::theGlobalRenderer);
	DXInstance * dxinstance = renderer->getDXInstance();
	if (clearMode & RENDERER_CLEAR_COLOR)
	{
		dxinstance->m_deviceContext->ClearRenderTargetView(dxinstance->m_currentRenderTarget.get(), myGlobalClearValueFlag);
	}
	if ((clearMode & RENDERER_CLEAR_DEPTH) || (clearMode & RENDERER_CLEAR_STENCIL))
	{
		u32 flag = (clearMode & RENDERER_CLEAR_DEPTH ? D3D11_CLEAR_DEPTH : 0) | (clearMode & RENDERER_CLEAR_STENCIL ? D3D11_CLEAR_STENCIL : 0);
		dxinstance->m_deviceContext->ClearDepthStencilView(dxinstance->m_currentDepthStencilTarget.get(), flag, 1.0f, 0);
	}
}

void DX11RenderingState::Viewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height)
{
	RendererDX11* renderer = static_cast<RendererDX11*>(ModuleRenderer::theGlobalRenderer);
	DXInstance * dxinstance = renderer->getDXInstance();
	D3D11_VIEWPORT viewport = { (float)x, (float)y, (float)width, (float)height, 0.0f, 1.0f };
	dxinstance->m_deviceContext->RSSetViewports(1, &viewport);
	renderer->SetCurrentViewportSize(v2u(width, height));
}

static D3D11_BLEND ConvertBlend(RendererBlendFuncMode blend)
{
	switch (blend)
	{
	case RendererBlendFuncMode::RENDERER_BLEND_ZERO:
		return D3D11_BLEND_ZERO;
	case RendererBlendFuncMode::RENDERER_BLEND_ONE:
		return D3D11_BLEND_ONE;
	case RendererBlendFuncMode::RENDERER_BLEND_SRC_COLOR:
		return D3D11_BLEND_SRC_COLOR;
	case RendererBlendFuncMode::RENDERER_BLEND_ONE_MINUS_SRC_COLOR:
		return D3D11_BLEND_INV_SRC_COLOR;
	case RendererBlendFuncMode::RENDERER_BLEND_DST_COLOR:
		return D3D11_BLEND_DEST_COLOR;
	case RendererBlendFuncMode::RENDERER_BLEND_ONE_MINUS_DST_COLOR:
		return D3D11_BLEND_INV_DEST_COLOR;
	case RendererBlendFuncMode::RENDERER_BLEND_SRC_ALPHA:
		return D3D11_BLEND_SRC_ALPHA;
	case RendererBlendFuncMode::RENDERER_BLEND_ONE_MINUS_SRC_ALPHA:
		return D3D11_BLEND_INV_SRC_ALPHA;
	case RendererBlendFuncMode::RENDERER_BLEND_DST_ALPHA:
		return D3D11_BLEND_DEST_ALPHA;
	case RendererBlendFuncMode::RENDERER_BLEND_ONE_MINUS_DST_ALPHA:
		return D3D11_BLEND_INV_DEST_ALPHA;
	case RendererBlendFuncMode::RENDERER_BLEND_GL_SRC_ALPHA_SATURATE:
		return D3D11_BLEND_SRC_ALPHA_SAT;
	}
	KIGS_ASSERT(!"unrecognized blend func");
	return D3D11_BLEND_ZERO;
}

void DX11RenderingState::manageBlend(DX11RenderingState* currentState)
{
	size_t hash = 0;
	hash_combine(hash,
				 currentState->myGlobalBlendFlag,
				 currentState->myGlobalBlendValue1Flag,
				 currentState->myGlobalBlendValue2Flag,
				 currentState->myGlobalColorMask[0],
				 currentState->myGlobalColorMask[1],
				 currentState->myGlobalColorMask[2],
				 currentState->myGlobalColorMask[3]
				 );


	RendererDX11* renderer = static_cast<RendererDX11*>(ModuleRenderer::theGlobalRenderer);
	DXInstance * dxinstance = renderer->getDXInstance();

	ID3D11BlendState* alphaBlendingState;
	auto found = renderer->BlendStateList().find(hash);
	if (found != renderer->BlendStateList().end())
	{
		alphaBlendingState = found->second;
	}
	else
	{
		D3D11_BLEND_DESC blendStateDescription;
		ZeroMemory(&blendStateDescription, sizeof(D3D11_BLEND_DESC));
		// Create an alpha enabled blend state description.
		blendStateDescription.RenderTarget[0].BlendEnable = currentState->myGlobalBlendFlag;

		blendStateDescription.RenderTarget[0].SrcBlend = ConvertBlend((RendererBlendFuncMode)currentState->myGlobalBlendValue1Flag);
		blendStateDescription.RenderTarget[0].DestBlend = ConvertBlend((RendererBlendFuncMode)currentState->myGlobalBlendValue2Flag);

		blendStateDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;

		blendStateDescription.RenderTarget[0].SrcBlendAlpha = blendStateDescription.RenderTarget[0].SrcBlend;
		blendStateDescription.RenderTarget[0].DestBlendAlpha = blendStateDescription.RenderTarget[0].DestBlend;
		blendStateDescription.RenderTarget[0].BlendOpAlpha = blendStateDescription.RenderTarget[0].BlendOp;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask = 0;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask |= currentState->myGlobalColorMask[0] ? D3D11_COLOR_WRITE_ENABLE_RED : 0;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask |= currentState->myGlobalColorMask[1] ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask |= currentState->myGlobalColorMask[2] ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0;
		blendStateDescription.RenderTarget[0].RenderTargetWriteMask |= currentState->myGlobalColorMask[3] ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0;

		// Create the blend state using the description.
		HRESULT result = dxinstance->m_device->CreateBlendState(&blendStateDescription, &alphaBlendingState);
		if (FAILED(result))
			return;

		renderer->BlendStateList()[hash] = alphaBlendingState;
	}

/*	float blendFactor[4];

	// Setup the blend factor.
	blendFactor[0] = 0.0f;
	blendFactor[1] = 0.0f;
	blendFactor[2] = 0.0f;
	blendFactor[3] = 0.0f;*/

	dxinstance->m_deviceContext->OMSetBlendState(alphaBlendingState, NULL, 0xffffffff);

}

static D3D11_STENCIL_OP ConvertStencilOp(RendererStencilOp op)
{
	//GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_INCR_WRAP, GL_DECR, GL_DECR_WRAP, and GL_INVERT
	if (op == RENDERER_STENCIL_OP_KEEP)
		return D3D11_STENCIL_OP_KEEP;
	if (op == RENDERER_STENCIL_OP_ZERO)
		return D3D11_STENCIL_OP_ZERO;
	if (op == RENDERER_STENCIL_OP_REPLACE)
		return D3D11_STENCIL_OP_REPLACE;
	if (op == RENDERER_STENCIL_OP_INCR)
		return D3D11_STENCIL_OP_INCR_SAT;
	if (op == RENDERER_STENCIL_OP_INCR_WRAP)
		return D3D11_STENCIL_OP_INCR;
	if (op == RENDERER_STENCIL_OP_DECR)
		return D3D11_STENCIL_OP_DECR_SAT;
	if (op == RENDERER_STENCIL_OP_DECR_WRAP)
		return D3D11_STENCIL_OP_DECR;
	if (op == RENDERER_STENCIL_OP_INVERT)
		return D3D11_STENCIL_OP_INVERT;

	return D3D11_STENCIL_OP_KEEP;
}

static D3D11_COMPARISON_FUNC ConvertStencilFunc(RendererStencilMode func)
{
	if (func == RENDERER_STENCIL_NEVER)
		return D3D11_COMPARISON_NEVER;
	if (func == RENDERER_STENCIL_LESS)
		return D3D11_COMPARISON_LESS;
	if (func == RENDERER_STENCIL_EQUAL)
		return D3D11_COMPARISON_EQUAL;
	if (func == RENDERER_STENCIL_LEQUAL)
		return D3D11_COMPARISON_LESS_EQUAL;
	if (func == RENDERER_STENCIL_GREATER)
		return D3D11_COMPARISON_GREATER;
	if (func == RENDERER_STENCIL_NOTEQUAL)
		return D3D11_COMPARISON_NOT_EQUAL;
	if (func == RENDERER_STENCIL_GEQUAL)
		return D3D11_COMPARISON_GREATER_EQUAL;
	if (func == RENDERER_STENCIL_ALWAYS)
		return D3D11_COMPARISON_ALWAYS;
	
	return D3D11_COMPARISON_ALWAYS;
}

void DX11RenderingState::manageDepthStencilTest(DX11RenderingState* currentState)
{
	//In DX11 we cannot have different masks for front/back faces
	size_t hash = 0; 
	hash_combine(hash
				 , currentState->myGlobalDepthTestFlag, currentState->myGlobalDepthMaskFlag
				 , currentState->myGlobalStencilEnabled, currentState->myGlobalStencilMask[0], currentState->myGlobalStencilFuncMask[0]//, currentState->myGlobalStencilMask[1], currentState->myGlobalStencilFuncMask[1]
				 , currentState->myGlobalStencilOpSFail[0], currentState->myGlobalStencilOpDPFail[0], currentState->myGlobalStencilOpPass[0], currentState->myGlobalStencilMode[0]
				 , currentState->myGlobalStencilOpSFail[1], currentState->myGlobalStencilOpDPFail[1], currentState->myGlobalStencilOpPass[1], currentState->myGlobalStencilMode[1]
	);

	RendererDX11* renderer = static_cast<RendererDX11*>(ModuleRenderer::theGlobalRenderer);
	DXInstance * dxinstance = renderer->getDXInstance();


	ID3D11DepthStencilState* depthStencilState;
	auto found = renderer->DepthStateList().find(hash);
	if (found != renderer->DepthStateList().end())
	{
		depthStencilState = found->second;
	}
	else
	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc;

		// Initialize the description of the stencil state.
		ZeroMemory(&depthStencilDesc, sizeof(depthStencilDesc));

		// Set up the description of the stencil state.
		depthStencilDesc.DepthEnable = currentState->myGlobalDepthTestFlag;
		depthStencilDesc.DepthWriteMask = currentState->myGlobalDepthMaskFlag ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;

		
		depthStencilDesc.StencilEnable = currentState->myGlobalStencilEnabled;
		
		depthStencilDesc.StencilReadMask = currentState->myGlobalStencilMask[0];
		depthStencilDesc.StencilWriteMask = currentState->myGlobalStencilFuncMask[0];

		// Stencil operations if pixel is front-facing.
		depthStencilDesc.FrontFace.StencilFailOp = ConvertStencilOp(currentState->myGlobalStencilOpSFail[0]);
		depthStencilDesc.FrontFace.StencilDepthFailOp = ConvertStencilOp(currentState->myGlobalStencilOpDPFail[0]);
		depthStencilDesc.FrontFace.StencilPassOp = ConvertStencilOp(currentState->myGlobalStencilOpPass[0]);
		depthStencilDesc.FrontFace.StencilFunc = ConvertStencilFunc(currentState->myGlobalStencilMode[0]);

		// Stencil operations if pixel is back-facing.
		depthStencilDesc.BackFace.StencilFailOp = ConvertStencilOp(currentState->myGlobalStencilOpSFail[1]);
		depthStencilDesc.BackFace.StencilDepthFailOp = ConvertStencilOp(currentState->myGlobalStencilOpDPFail[1]);
		depthStencilDesc.BackFace.StencilPassOp = ConvertStencilOp(currentState->myGlobalStencilOpPass[1]);
		depthStencilDesc.BackFace.StencilFunc = ConvertStencilFunc(currentState->myGlobalStencilMode[1]);

		// Create the depth stencil state.
		HRESULT result = dxinstance->m_device->CreateDepthStencilState(&depthStencilDesc, &depthStencilState);
		if (FAILED(result))
			return;

		renderer->DepthStateList()[hash] = depthStencilState;
	}

	// Set the depth stencil state.
	dxinstance->m_deviceContext->OMSetDepthStencilState(depthStencilState, currentState->myGlobalStencilFuncRef[0]);

}

void DX11RenderingState::manageRasterizerState(DX11RenderingState* currentState)
{
	size_t hash = 0;
	hash_combine(hash, currentState->myGlobalCullFlag, currentState->myPolygonMode, currentState->myGlobalScissorTestFlag);

	RendererDX11* renderer = static_cast<RendererDX11*>(ModuleRenderer::theGlobalRenderer);
	DXInstance * dxinstance = renderer->getDXInstance();


	ID3D11RasterizerState* rasterizerState;
	auto found = renderer->RasterizerStateList().find(hash);
	if (found != renderer->RasterizerStateList().end())
	{
		rasterizerState = found->second;
	}
	else
	{
		// Setup the raster description which will determine how and what polygons will be drawn.
		D3D11_RASTERIZER_DESC rasterDesc;
		rasterDesc.AntialiasedLineEnable = false;
		rasterDesc.DepthBias = 0;
		rasterDesc.DepthBiasClamp = 0.0f;
		rasterDesc.DepthClipEnable = true;
		rasterDesc.FrontCounterClockwise = true;
		rasterDesc.MultisampleEnable = false;
		rasterDesc.ScissorEnable = currentState->myGlobalScissorTestFlag;
		rasterDesc.SlopeScaledDepthBias = 0.0f;

		switch (currentState->myGlobalCullFlag)
		{
		case RendererCullMode::RENDERER_CULL_NONE:
			rasterDesc.CullMode = D3D11_CULL_NONE;
			break;
		case RendererCullMode::RENDERER_CULL_FRONT:	// triangle ordering is inverted compared to OpenGL
			rasterDesc.CullMode = D3D11_CULL_FRONT;
			break;
		case RendererCullMode::RENDERER_CULL_BACK: // triangle ordering is inverted compared to OpenGL
			rasterDesc.CullMode = D3D11_CULL_BACK;
			break;
		default:
			KIGS_ERROR("unrecognized cull mode", 2);
			break;
		}

		switch (currentState->myPolygonMode)
		{
		case RendererPolygonMode::RENDERER_LINE:
			rasterDesc.FillMode = D3D11_FILL_WIREFRAME;
			break;
		case RendererPolygonMode::RENDERER_FILL:
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			break;
		default:
			KIGS_ERROR("unrecognized polygon mode", 2);
			break;
		}

		// Create the rasterizer state from the description we just filled out.
		HRESULT result = dxinstance->m_device->CreateRasterizerState(&rasterDesc, &rasterizerState);
		if (FAILED(result))
			return;

		renderer->RasterizerStateList()[hash] = rasterizerState;
	}

	// Now set the rasterizer state.
	dxinstance->m_deviceContext->RSSetState(rasterizerState);
	if (currentState->myGlobalScissorTestFlag)
	{
		D3D11_RECT rect;
		int vp_height = renderer->GetCurrentViewportSize().y;

		rect.bottom = currentState->myGlobalScissorYFlag;
		rect.top = (currentState->myGlobalScissorYFlag + currentState->myGlobalScissorHeightFlag);
		rect.left = currentState->myGlobalScissorXFlag;
		rect.right = currentState->myGlobalScissorXFlag + currentState->myGlobalScissorWidthFlag;

		if (!renderer->getDXInstance()->m_isFBORenderTarget)
		{
			rect.bottom = vp_height - rect.bottom;
			rect.top = vp_height - rect.top;
		}
		else
		{
			std::swap(rect.top, rect.bottom);
		}

		dxinstance->m_deviceContext->RSSetScissorRects(1, &rect);
	}
}

void DX11RenderingState::FlushState(RenderingState* currentState, bool force)
{
	DX11RenderingState* otherOne = static_cast<DX11RenderingState*>(currentState);
	
	// Blend State
	otherOne->myGlobalBlendFlag = myGlobalBlendFlag;
	otherOne->myGlobalBlendValue1Flag = myGlobalBlendValue1Flag;
	otherOne->myGlobalBlendValue2Flag = myGlobalBlendValue2Flag;

	for(int i=0;i<4;++i)
		otherOne->myGlobalColorMask[i] = myGlobalColorMask[i];

	manageBlend(otherOne);

	// Depth Stencil
	otherOne->myGlobalDepthTestFlag = myGlobalDepthTestFlag;
	otherOne->myGlobalDepthMaskFlag = myGlobalDepthMaskFlag;

	otherOne->myGlobalDepthTestFlag = myGlobalDepthTestFlag;
	otherOne->myGlobalDepthMaskFlag = myGlobalDepthMaskFlag;
	otherOne->myGlobalStencilEnabled = myGlobalStencilEnabled;

	for (int i = 0; i < 2; ++i)
	{
		otherOne->myGlobalStencilMode[i] = myGlobalStencilMode[i];
		otherOne->myGlobalStencilFuncMask[i] = myGlobalStencilFuncMask[i];
		otherOne->myGlobalStencilFuncRef[i] = myGlobalStencilFuncRef[i];
		otherOne->myGlobalStencilOpSFail[i] = myGlobalStencilOpSFail[i];
		otherOne->myGlobalStencilOpDPFail[i] = myGlobalStencilOpDPFail[i];
		otherOne->myGlobalStencilOpPass[i] = myGlobalStencilOpPass[i];
		otherOne->myGlobalStencilMode[i] = myGlobalStencilMode[i];
	}
	manageDepthStencilTest(otherOne);

	// manage raster
	otherOne->myGlobalCullFlag = myGlobalCullFlag;
	otherOne->myPolygonMode = myPolygonMode;

	otherOne->myGlobalScissorTestFlag = myGlobalScissorTestFlag;
	otherOne->myGlobalScissorXFlag = myGlobalScissorXFlag;
	otherOne->myGlobalScissorYFlag = myGlobalScissorYFlag;
	otherOne->myGlobalScissorWidthFlag = myGlobalScissorWidthFlag;
	otherOne->myGlobalScissorHeightFlag = myGlobalScissorHeightFlag;

	manageRasterizerState(otherOne);
}

void RendererDX11::CreateTexture(int count, unsigned int * id)
{
	throw std::exception();
}

void RendererDX11::DeleteTexture(int count, unsigned int * id)
{
	throw std::exception();
}

void RendererDX11::EnableTexture()
{
}

void RendererDX11::DisableTexture()
{
}

void RendererDX11::BindTexture(RendererTextureType type, unsigned int ID)
{
	KIGS_ASSERT(false);
}

void RendererDX11::UnbindTexture(RendererTextureType type, unsigned int ID)
{
	KIGS_ASSERT(false);
}

void RendererDX11::TextureParameteri(RendererTextureType type, RendererTexParameter1 name, RendererTexParameter2 param)
{
	KIGS_ASSERT(false);
}

void RendererDX11::SetSampler(bool repeatU, bool repeatV, bool forceNearest)
{
	u32 hash = (repeatU ? 1 : 0) 
		| (repeatV ? 2 : 0) 
		| (forceNearest ? 4 : 0); 

	if (mySamplerStateList.find(hash) == mySamplerStateList.end())
	{
		D3D11_SAMPLER_DESC samplerDesc;
		samplerDesc.Filter = forceNearest ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = repeatU ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = repeatV ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MipLODBias = 0.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
		samplerDesc.BorderColor[0] = 0;
		samplerDesc.BorderColor[1] = 0;
		samplerDesc.BorderColor[2] = 0;
		samplerDesc.BorderColor[3] = 0;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		HRESULT result = myDXInstance.m_device->CreateSamplerState(&samplerDesc, &mySamplerStateList[hash]);
		if (FAILED(result))
		{
			KIGS_ERROR("Failed to create sampler", 3);
		}
	}
	myDXInstance.m_deviceContext->PSSetSamplers(GetActiveTextureChannel(), 1, &mySamplerStateList[hash]);
}

// ### VertexBufferManager Section
VertexBufferManager::VertexBufferManager()
{
	for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
	{
		//mCurrentAskedVertexBuffer[i] = -1;
		mCurrentBoundVertexBuffer[i] = -1;
	}
	
	//mCurrentAskedElementBuffer = -1;
	mCurrentBoundElementBuffer = -1;

	//mEnableVertexAttrib.clear();
	//mEnableVertexAttrib.reserve(32);
	mBufferList.emplace_back().mI3DBuffer = (ID3D11Buffer*)1;
}

void VertexBufferManager::GenBuffer(int count, unsigned int * id)
{
	int i = 0;
	if (mCountFree)
	{
		int j=0;

		while( (i < count) && (j< mBufferList.size()) && mCountFree)
		{
			if ((mBufferList[j].mI3DBuffer == nullptr) && (mBufferList[j].mDesc == nullptr))
			{
				mBufferList[j].mDesc = (D3D11_BUFFER_DESC*)malloc(sizeof(D3D11_BUFFER_DESC));
				memset(mBufferList[j].mDesc, 0, sizeof(D3D11_BUFFER_DESC));
				id[i] = j;
				i++;
				mCountFree--;
			}
			j++;
		}
	}
	for (; i < count; i++)
	{
		vbufferStruct toAdd;
		toAdd.mI3DBuffer = nullptr;
		toAdd.mDesc = (D3D11_BUFFER_DESC*)malloc(sizeof(D3D11_BUFFER_DESC));
		memset(toAdd.mDesc, 0, sizeof(D3D11_BUFFER_DESC));
		toAdd.mBufferStride = 0;
		toAdd.mLayoutDesc.clear();
		toAdd.mLayoutDescWasChanged = false;

		mBufferList.push_back(toAdd);
		id[i] = mBufferList.size() - 1;
	}
}

void VertexBufferManager::DelBuffer(int count, unsigned int * id)
{
	for (int i = 0; i < count; i++)
	{
		if (id[i] == 0) continue;

		if (mBufferList[id[i]].mDesc != nullptr)
		{
			if (mBufferList[id[i]].mI3DBuffer != nullptr)
			{
				mBufferList[id[i]].mI3DBuffer->Release();
				mBufferList[id[i]].mI3DBuffer = nullptr;
			}
		
			free(mBufferList[id[i]].mDesc);
			mBufferList[id[i]].mDesc = nullptr;
			mBufferList[id[i]].mBufferStride = 0;
			mBufferList[id[i]].mLayoutDesc.clear();
			mCountFree++;
		}
	}
}

void VertexBufferManager::DelBufferLater(int count, unsigned int * id)
{
	for (int i = 0; i < count; i++)
		mToDeleteBuffer.push_back(id[i]);
}

void VertexBufferManager::DoDelayedAction()
{
	if (!mToDeleteBuffer.empty())
	{
		DelBuffer(mToDeleteBuffer.size(), mToDeleteBuffer.data());
		mToDeleteBuffer.clear();
	}
}

void VertexBufferManager::internalBindBuffer(unsigned int bufferName, unsigned int bufftype, int slot)
{
	if (bufftype == KIGS_BUFFER_TARGET_ARRAY)
	{
		SetArrayBuffer(bufferName, slot);
		FlushBindBuffer(KIGS_BUFFER_TARGET_ARRAY);
	}
	else if (bufftype == KIGS_BUFFER_TARGET_ELEMENT)
	{
		SetElementBuffer(bufferName);
		FlushBindBuffer(KIGS_BUFFER_TARGET_ELEMENT);
	}
}

void VertexBufferManager::FlushBindBuffer(int target, bool force)
{
	/*if (force)
	{
		for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
			mCurrentBoundVertexBuffer[i] = -1;
		mCurrentBoundElementBuffer = -1;
	}

	if (target == KIGS_BUFFER_TARGET_ARRAY || target == 0)
	{
		for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
		{
			if ((mCurrentAskedVertexBuffer[i] != mCurrentBoundVertexBuffer[i]) && (mCurrentAskedVertexBuffer[i] != -1))
			{
				mCurrentBoundVertexBuffer[i] = mCurrentAskedVertexBuffer[i];
			}
		}
	}

	if (target == KIGS_BUFFER_TARGET_ELEMENT || target == 0)
	{
		if ((mCurrentAskedElementBuffer != mCurrentBoundElementBuffer) && (mCurrentAskedElementBuffer != -1))
		{
			mCurrentBoundElementBuffer = mCurrentAskedElementBuffer;
		}
	}

	for (int i = 0; i < mCurrentAskedVertexBuffer.size(); ++i)
		mCurrentAskedVertexBuffer[i] = -1;
	mCurrentAskedElementBuffer = -1;*/
}

void VertexBufferManager::UnbindBuffer(unsigned int bufferName, int target)
{
	if (target == KIGS_BUFFER_TARGET_ARRAY || target == 0)
	{
		for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
		{
			if (mCurrentBoundVertexBuffer[i] == bufferName)
				mCurrentBoundVertexBuffer[i] = -1;
		}
	}
		

	if (target == KIGS_BUFFER_TARGET_ELEMENT || target == 0)
		if (mCurrentBoundElementBuffer == bufferName)
			mCurrentBoundElementBuffer = -1;
}

void VertexBufferManager::Clear(bool push)
{
	
	if (push)
	{
		/*std::vector<unsigned int>::iterator itr = mEnableVertexAttrib.begin();
		std::vector<unsigned int>::iterator end = mEnableVertexAttrib.end();
		for (; itr != end; ++itr)
		{
			//glDisableVertexAttribArray(*itr); 
		}
		*/
	}
	/*
	for (int i = 0; i < mCurrentAskedVertexBuffer.size(); ++i)
		mCurrentAskedVertexBuffer[i] = -1;
	mCurrentAskedElementBuffer = -1;*/
	//mEnableVertexAttrib.clear();
	
}

void VertexBufferManager::SetElementBuffer(unsigned int bufferName)
{
	//mCurrentAskedElementBuffer = bufferName;
	mCurrentBoundElementBuffer = bufferName;
}

void VertexBufferManager::SetArrayBuffer(unsigned int bufferName, int slot)
{
	//mCurrentAskedVertexBuffer[slot] = bufferName;
	mCurrentBoundVertexBuffer[slot] = bufferName;
}

void VertexBufferManager::SetVertexAttrib(unsigned int bufferName, unsigned int attribID, int size, unsigned int type, bool normalized, unsigned int stride, void * offset,const Locations* locs)
{
	//internalBindBuffer(bufferName, KIGS_BUFFER_TARGET_ARRAY, mLastBoundSlot);

	// set data in temporary structure
	InputElementDesc& toset = mBufferList[bufferName].mLayoutDesc[attribID];
	toset.elemCount = size;
	toset.elemType = type;
	toset.offset = offset;
	
	mBufferList[bufferName].mLayoutDescWasChanged = true;
	mBufferList[bufferName].mBufferStride = stride;

	//mEnableVertexAttrib.push_back(attribID);
}

void VertexBufferManager::SetVertexAttribDivisor(unsigned int bufferName, int attributeID, int divisor)
{
	auto itfind = mBufferList[bufferName].mLayoutDesc.find(attributeID);
	if (itfind == mBufferList[bufferName].mLayoutDesc.end())
	{
		if (divisor == 0) return;
		KIGS_ASSERT(!"trying to set step rate but attributeID has not been setup yet");
	}
	itfind->second.step_rate = divisor;
}

void VertexBufferManager::BufferData(unsigned int bufferName, unsigned int bufferType, int size, void* data, unsigned int usage)
{
	internalBindBuffer(bufferName, bufferType);

	if (mBufferList[bufferName].mDesc == nullptr)
	{
		return;
	}

	if ((mBufferList[bufferName].mDesc->Usage != usage) || (mBufferList[bufferName].mDesc->BindFlags != bufferType) || (mBufferList[bufferName].mDesc->ByteWidth < size) ||  ((mBufferList[bufferName].mDesc->ByteWidth >2048) && (mBufferList[bufferName].mDesc->ByteWidth > (size*4)))) // need to create a new buffer
	{
		if ((usage == D3D11_USAGE_IMMUTABLE) && (data == nullptr)) // STATIC must be init directly
		{
			usage = D3D11_USAGE_DYNAMIC;
		}
		mBufferList[bufferName].mDesc->Usage = (D3D11_USAGE)usage;
		mBufferList[bufferName].mDesc->BindFlags = bufferType;
		mBufferList[bufferName].mDesc->ByteWidth = size;
		if (mBufferList[bufferName].mI3DBuffer)
		{
			mBufferList[bufferName].mI3DBuffer->Release();
		}

		mBufferList[bufferName].mDesc->CPUAccessFlags = (usage == D3D11_USAGE_IMMUTABLE) ? 0 : D3D11_CPU_ACCESS_WRITE;
		mBufferList[bufferName].mDesc->MiscFlags = 0;
		mBufferList[bufferName].mDesc->StructureByteStride = 0;


		D3D11_SUBRESOURCE_DATA InitData;
		InitData.pSysMem = data;
		InitData.SysMemPitch = 0;
		InitData.SysMemSlicePitch = 0;

		D3D11_SUBRESOURCE_DATA* setData =(D3D11_SUBRESOURCE_DATA*)( (data!=nullptr) ? &InitData : data);
		RendererDX11::theGlobalRenderer->as<RendererDX11>()->getDXInstance()->m_device->CreateBuffer(mBufferList[bufferName].mDesc, setData, &mBufferList[bufferName].mI3DBuffer);
	}
	else if (data) // already created buffer
	{
		//fill the vertex buffer
		D3D11_MAPPED_SUBRESOURCE mappedResource;
		ZeroMemory(&mappedResource, sizeof(D3D11_MAPPED_SUBRESOURCE));

		//  Disable GPU access to the vertex buffer data.
		RendererDX11::theGlobalRenderer->as<RendererDX11>()->getDXInstance()->m_deviceContext->Map(mBufferList[bufferName].mI3DBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		//  Update the vertex buffer here.
		memcpy(mappedResource.pData, data, size);
		//  Reenable GPU access to the vertex buffer data.
		RendererDX11::theGlobalRenderer->as<RendererDX11>()->getDXInstance()->m_deviceContext->Unmap(mBufferList[bufferName].mI3DBuffer, 0);
	}
}

size_t VertexBufferManager::GetAllocatedBufferCount()
{
	return mBufferList.size() - mCountFree;
}

void RendererDX11::SetVertexAttribDivisor(TravState* state, unsigned int bufferName, int attribID, int divisor)
{
	myVertexBufferManager->SetVertexAttribDivisor(bufferName, attribID, divisor * (state->GetHolographicMode() ? 2 : 1));
}

size_t VertexBufferManager::GetCurrentLayoutHash()
{
	size_t hash = 0;
	for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
	{
		if (mCurrentBoundVertexBuffer[i] == -1) continue;
		auto& desc = mBufferList[mCurrentBoundVertexBuffer[i]].mLayoutDesc;

		for (auto m : desc)
		{
			hash_combine(hash, m.first, m.second.elemCount, m.second.elemType, (intptr_t)m.second.offset, m.second.step_rate);
		}
	}
	return hash;
}

D3D11_INPUT_ELEMENT_DESC* VertexBufferManager::CreateLayoutDesc(int& descsize)
{
	descsize = 0;
	
	for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
	{
		if (mCurrentBoundVertexBuffer[i] == -1) continue;
		descsize += mBufferList[mCurrentBoundVertexBuffer[i]].mLayoutDesc.size();
	}

	if (descsize == 0) return nullptr;

	D3D11_INPUT_ELEMENT_DESC* result = (D3D11_INPUT_ELEMENT_DESC*)malloc(sizeof(D3D11_INPUT_ELEMENT_DESC) * descsize);
	KIGS_ASSERT(result);

	int index = 0;
	for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
	{
		if (mCurrentBoundVertexBuffer[i] == -1) continue;

		for (auto m : mBufferList[mCurrentBoundVertexBuffer[i]].mLayoutDesc)
		{
			D3D11_INPUT_ELEMENT_DESC& current = result[index];

			current.SemanticName = KIGS_VERTEX_ATTRIB[m.first];
			current.SemanticIndex = KIGS_VERTEX_ATTRIB_INDEX[m.first];

			DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
			if (m.second.elemType == KIGS_FLOAT)
			{
				switch (m.second.elemCount)
				{
				case 1:
					f = DXGI_FORMAT_R32_FLOAT;
					break;
				case 2:
					f = DXGI_FORMAT_R32G32_FLOAT;
					break;
				case 3:
					f = DXGI_FORMAT_R32G32B32_FLOAT;
					break;
				case 4:
					f = DXGI_FORMAT_R32G32B32A32_FLOAT;
					break;
				}
			}
			else if (m.second.elemType == KIGS_UNSIGNED_INT)
			{
				switch (m.second.elemCount)
				{
				case 1:
					f = DXGI_FORMAT_R32_UINT;
					break;
				case 2:
					f = DXGI_FORMAT_R32G32_UINT;
					break;
				case 3:
					f = DXGI_FORMAT_R32G32B32_UINT;
					break;
				case 4:
					f = DXGI_FORMAT_R32G32B32A32_UINT;
					break;
				}
			}
			else if (m.second.elemType == KIGS_UNSIGNED_BYTE)
			{
				switch (m.second.elemCount)
				{
				case 1:
					f = DXGI_FORMAT_R8_UNORM;
					break;
				case 2:
					f = DXGI_FORMAT_R8G8_UNORM;
					break;
				case 3: // 3 bytes is not possible
				case 4:
					f = DXGI_FORMAT_R8G8B8A8_UNORM;
					break;
				default:
					KIGS_ASSERT(!"unsupported element count in input layout");

				}
			}
			else if (m.second.elemType == KIGS_BYTE)
			{
				switch (m.second.elemCount)
				{
				case 1:
					f = DXGI_FORMAT_R8_SNORM;
					break;
				case 2:
					f = DXGI_FORMAT_R8G8_SNORM;
					break;
				case 3: //  3 bytes is not possible
				case 4:
					f = DXGI_FORMAT_R8G8B8A8_SNORM;
					break;
				default:
					KIGS_ASSERT(!"unsupported element count in input layout");
				}
			}
			else
			{
				KIGS_ASSERT(!"unsupported format for input layout");
			}
			current.Format = f;
			current.InputSlot = i;
			current.AlignedByteOffset = (int)m.second.offset; // D3D11_APPEND_ALIGNED_ELEMENT;
			current.InputSlotClass = m.second.step_rate == 0 ? D3D11_INPUT_PER_VERTEX_DATA : D3D11_INPUT_PER_INSTANCE_DATA;
			current.InstanceDataStepRate = m.second.step_rate;
			++index;
		}
	}
	return result;
}

void VertexBufferManager::ClearCurrentLayout()
{
	for (int i = 0; i < mCurrentBoundVertexBuffer.size(); ++i)
	{
		if (mCurrentBoundVertexBuffer[i] == -1) continue;
		mBufferList[mCurrentBoundVertexBuffer[i]].mLayoutDesc.clear();
	}
}

void RendererDX11::DrawUIQuad(TravState * state, const UIVerticesInfo * qi)
{
	unsigned int bufferName = getUIVBO();

	BufferData(bufferName, KIGS_BUFFER_TARGET_ARRAY, qi->Offset * qi->vertexCount, qi->Buffer(), KIGS_BUFFER_USAGE_DYNAMIC);

	SetVertexAttrib(bufferName, KIGS_VERTEX_ATTRIB_VERTEX_ID, qi->vertexComp, KIGS_FLOAT, false, qi->Offset, (void*)qi->vertexStride, nullptr);
	SetVertexAttrib(bufferName, KIGS_VERTEX_ATTRIB_COLOR_ID, qi->colorComp, KIGS_UNSIGNED_BYTE, false, qi->Offset, (void*)qi->colorStride, nullptr);
	SetVertexAttrib(bufferName, KIGS_VERTEX_ATTRIB_TEXCOORD_ID, qi->texComp, KIGS_FLOAT, false, qi->Offset, (void*)qi->texStride, nullptr);

	// Set the index buffer to active in the input assembler so it can be rendered.
	//	myDXInstance.m_deviceContext->IASetIndexBuffer(UIIndexBuffer, DXGI_FORMAT_R32_UINT, 0);

	DrawArrays(state, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 0, qi->vertexCount);
}

void RendererDX11::DrawUITriangles(TravState * state, const UIVerticesInfo * qi)
{
	unsigned int bufferName = getUIVBO();
	
	BufferData(bufferName, KIGS_BUFFER_TARGET_ARRAY, qi->Offset * qi->vertexCount, qi->Buffer(), KIGS_BUFFER_USAGE_DYNAMIC);
	
	SetVertexAttrib(bufferName, KIGS_VERTEX_ATTRIB_VERTEX_ID, qi->vertexComp, KIGS_FLOAT, false, qi->Offset, (void*)qi->vertexStride, nullptr);
	SetVertexAttrib(bufferName, KIGS_VERTEX_ATTRIB_COLOR_ID, qi->colorComp, KIGS_UNSIGNED_BYTE, false, qi->Offset, (void*)qi->colorStride, nullptr);
	SetVertexAttrib(bufferName, KIGS_VERTEX_ATTRIB_TEXCOORD_ID, qi->texComp, KIGS_FLOAT, false, qi->Offset, (void*)qi->texStride, nullptr);

	// Set the index buffer to active in the input assembler so it can be rendered.
	// myDXInstance.m_deviceContext->IASetIndexBuffer(UIIndexBuffer, DXGI_FORMAT_R32_UINT, 0);

	DrawArrays(state, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 0, qi->vertexCount);
}

void RendererDX11::DrawElementsInstanced(TravState* state, unsigned int mode, int count, unsigned int type, void* indices, int primcount, bool clear_manager)
{
	myVertexBufferManager->FlushBindBuffer();
	GetActiveShader()->as<API3DShader>()->setLayout();

	auto buffers = ((VertexBufferManager*)myVertexBufferManager.get())->GetBoundBuffersList();
	auto strides = ((VertexBufferManager*)myVertexBufferManager.get())->GetBoundBuffersStride();
	std::vector<u32> offsets; offsets.resize(buffers.size(), 0);

	myDXInstance.m_deviceContext->IASetVertexBuffers(
		0,					// the first input slot for binding
		buffers.size(),		// the number of buffers in the array
		buffers.data(),		// the array of vertex buffers
		strides.data(),		// array of stride values, one for each buffer
		offsets.data());	// array of offset values, one for each buffer

	FlushState();
	FlushMatrix(state);

	ID3D11Buffer* ibuffer = ((VertexBufferManager*)myVertexBufferManager.get())->GetIBuffer();

	u32 ioffset = 0;
	if (indices != nullptr)
	{
		ioffset = (u32)indices;
	}

	myDXInstance.m_deviceContext->IASetIndexBuffer(ibuffer, (DXGI_FORMAT)type, ioffset);

	KIGS_ASSERT(mode != KIGS_DRAW_MODE_TRIANGLE_FAN);
#ifdef KIGS_TOOLS
	gRendererStats.DrawCalls += 1;
	gRendererStats.DrawCallsTriangleCount += primcount * count / 3;
#endif
	// Set the type of primitive that should be rendered from this vertex buffer, in this case triangles.
	myDXInstance.m_deviceContext->IASetPrimitiveTopology((D3D_PRIMITIVE_TOPOLOGY)mode);

	// Render the triangle.
	myDXInstance.m_deviceContext->DrawIndexedInstanced(count, primcount * (state->GetHolographicMode() ? 2 : 1), 0, 0, 0);
}

void RendererDX11::DrawPendingInstances(TravState* state)
{
	state->mDrawingInstances = true;
#ifdef DEBUG_DRAW_INSTANCES
	static int o = -1;
	static int t = -1;
	if (gKigsToolsAvailable)
	{
		if (!state->myPath)
			ImGui::SliderInt("o", &o, -1, state->mInstancing.size() - 1);
		else
			ImGui::SliderInt("t", &t, -1, state->mInstancing.size() - 1);
	}
#endif
	int k = 0;

	ShaderBase* active_shader = GetActiveShader();
	bool first = true;



	std::vector<std::pair<ModernMeshItemGroup* const, InstancingData>*> sorted_instances; sorted_instances.reserve(state->mInstancing.size());
	for (auto& instance : state->mInstancing)
	{
		sorted_instances.push_back(&instance);
	}

	std::sort(sorted_instances.begin(), sorted_instances.end(), [](const auto& a, const auto& b)
	{
		return std::make_tuple(INT_MAX - a->second.priority, a->second.shader, a->second.shader_variant) < std::make_tuple(INT_MAX - b->second.priority, b->second.shader, b->second.shader_variant);
	});
	
	state->mInstanceBufferIndex = getVBO();

	for (auto& instance_ptr : sorted_instances)
	{
		auto& instance = *instance_ptr;
		++k;
#ifdef DEBUG_DRAW_INSTANCES
		if (!state->myPath && o != -1 && o != k - 1) continue;
		if (state->myPath && t != -1 && t != k - 1) continue;
#endif
		auto mesh = instance.first;
		state->mInstanceCount = instance.second.transforms.size();
		if (active_shader != instance.second.shader)
		{
			if (!first)
				popShader(active_shader, state);

			first = false;
			pushShader(instance.second.shader, state);
			active_shader = instance.second.shader;
		}
		mesh->DoPreDraw(state);
		
		BufferData(state->mInstanceBufferIndex, KIGS_BUFFER_TARGET_ARRAY, sizeof(float) * 12 * instance.second.transforms.size(), instance.second.transforms.data(), KIGS_BUFFER_USAGE_DYNAMIC);

		auto shader = GetActiveShader();

		SetArrayBuffer(state->mInstanceBufferIndex, 1);
		

		mesh->DoDraw(state);
		
		//SetVertexAttribDivisor(state, state->mInstanceBufferIndex, KIGS_VERTEX_ATTRIB_INSTANCE_MATRIX_ID + 0, 0);
		//SetVertexAttribDivisor(state, state->mInstanceBufferIndex, KIGS_VERTEX_ATTRIB_INSTANCE_MATRIX_ID + 1, 0);
		//SetVertexAttribDivisor(state, state->mInstanceBufferIndex, KIGS_VERTEX_ATTRIB_INSTANCE_MATRIX_ID + 2, 0);

		SetArrayBuffer(-1, 1);

		mesh->DoPostDraw(state);

		state->mInstanceCount = 0;
	}

	if (!first)
	{
		popShader(active_shader, state);
	}

	state->mDrawingInstances = false;
	state->mInstancing.clear();
}

// # Draw functions
void RendererDX11::DrawArrays(TravState* state, unsigned int mode, int first, int count)
{
	myVertexBufferManager->FlushBindBuffer();
	GetActiveShader()->as<API3DShader>()->setLayout();

	auto buffers = ((VertexBufferManager*)myVertexBufferManager.get())->GetBoundBuffersList();
	auto strides = ((VertexBufferManager*)myVertexBufferManager.get())->GetBoundBuffersStride();
	std::vector<u32> offsets; offsets.resize(buffers.size(), 0);

	myDXInstance.m_deviceContext->IASetVertexBuffers(
		0,					// the first input slot for binding
		buffers.size(),		// the number of buffers in the array
		buffers.data(),		// the array of vertex buffers
		strides.data(),		// array of stride values, one for each buffer
		offsets.data());	// array of offset values, one for each buffer

	FlushState();
	FlushMatrix(state);

	// Set the type of primitive that should be rendered from this vertex buffer, in this case triangles.

	KIGS_ASSERT(mode != KIGS_DRAW_MODE_TRIANGLE_FAN);

	myDXInstance.m_deviceContext->IASetPrimitiveTopology((D3D_PRIMITIVE_TOPOLOGY)mode);
#ifdef KIGS_TOOLS
	gRendererStats.DrawCalls += 1;
	gRendererStats.DrawCallsTriangleCount += count / 3;
#endif
	// Render the triangle.
	if (state->GetHolographicMode())
	{
		myDXInstance.m_deviceContext->DrawInstanced(count, 2, 0, 0);
	}
	else
	{
		myDXInstance.m_deviceContext->Draw(count, 0);
	}
}

void RendererDX11::DrawElements(TravState* state, unsigned int mode, int count, unsigned int type, void* indices, bool unused)
{
	myVertexBufferManager->FlushBindBuffer();
	GetActiveShader()->as<API3DShader>()->setLayout();

	auto buffers = ((VertexBufferManager*)myVertexBufferManager.get())->GetBoundBuffersList();
	auto strides = ((VertexBufferManager*)myVertexBufferManager.get())->GetBoundBuffersStride();
	std::vector<u32> offsets; offsets.resize(buffers.size(), 0);

	myDXInstance.m_deviceContext->IASetVertexBuffers(
		0,					// the first input slot for binding
		buffers.size(),		// the number of buffers in the array
		buffers.data(),		// the array of vertex buffers
		strides.data(),		// array of stride values, one for each buffer
		offsets.data());	// array of offset values, one for each buffer

	FlushState();
	FlushMatrix(state);

	ID3D11Buffer* ibuffer = ((VertexBufferManager*)myVertexBufferManager.get())->GetIBuffer();

	u32 ioffset = 0;
	if (indices != nullptr)
	{
		ioffset = (u32)indices;
	}

	myDXInstance.m_deviceContext->IASetIndexBuffer(ibuffer, (DXGI_FORMAT)type, ioffset);

	KIGS_ASSERT(mode != KIGS_DRAW_MODE_TRIANGLE_FAN);
	// Set the type of primitive that should be rendered from this vertex buffer, in this case triangles.
	myDXInstance.m_deviceContext->IASetPrimitiveTopology((D3D_PRIMITIVE_TOPOLOGY)mode);

#ifdef KIGS_TOOLS
	gRendererStats.DrawCalls += 1;
	gRendererStats.DrawCallsTriangleCount += count/3;
#endif
	// Render the triangle.
	if (state->GetHolographicMode())
	{
		myDXInstance.m_deviceContext->DrawIndexedInstanced(count, 2, 0, 0, 0);
	}
	else
	{
		myDXInstance.m_deviceContext->DrawIndexed(count, 0, 0);
	}
}


union QueryIDDecoder
{
	u64 id;
	struct
	{
		u16 index;
		u16 type;
		u32 frame_index;
	};
};

const int MAX_OCCLUSION_QUERIES = 128;

bool RendererDX11::BeginOcclusionQuery(TravState* state, u64& query_id, RendererQueryType type, int frames_to_keep)
{
	++mOcclusionQueriesForFrame;
#ifdef KIGS_TOOLS
	gRendererStats.OcclusionQueriesRequested++;
#endif
	KIGS_ASSERT(frames_to_keep >= 1); // need at least one frame alive

	auto frame_index = state->GetFrameNumber();
	auto& query_list = mOcclusionQueries[type];
	auto& free_query_count = mFreeQueryCount[type];

	QueryIDDecoder encoder;

	if (free_query_count)
	{
		for (u16 i = 0; i < query_list.size(); ++i)
		{
			if (frame_index > query_list[i].frame_of_execution + query_list[i].frames_to_keep)
			{
				encoder.index = i;
				free_query_count--;
				break;
			}
		}
	}
	else
	{
		if (query_list.size() >= MAX_OCCLUSION_QUERIES) return false;

		auto& query = query_list.emplace_back();
		D3D11_QUERY_DESC desc = {};
		desc.Query = type == RENDERER_QUERY_SAMPLES_PASSED ? D3D11_QUERY_OCCLUSION : D3D11_QUERY_OCCLUSION_PREDICATE;
		DX::ThrowIfFailed(myDXInstance.m_device->CreateQuery(&desc, &query.query));
		encoder.index = (u16)(query_list.size() - 1);
	}
	auto& query = query_list[encoder.index];

	encoder.type = (u16)type;
	encoder.frame_index = frame_index;

	query.result_ok = false;
	query.frames_to_keep = frames_to_keep;
	query.frame_of_execution = frame_index;
	myDXInstance.m_deviceContext->Begin(query.query);

	query_id = encoder.id;
#ifdef KIGS_TOOLS
	gRendererStats.OcclusionQueriesStarted++;
#endif
	return true;
}

void RendererDX11::EndOcclusionQuery(TravState* state, u64 query_id)
{
	QueryIDDecoder decoder;
	decoder.id = query_id;

	u32 index = decoder.index;
	RendererQueryType type = (RendererQueryType)decoder.type;

	auto& query_list = mOcclusionQueries[type];
	auto& query = query_list[index];
	auto frame_index = state->GetFrameNumber();

	KIGS_ASSERT(frame_index == query.frame_of_execution); // Same frame as begin call

	myDXInstance.m_deviceContext->End(query.query);
}

bool RendererDX11::GetOcclusionQueryResult(TravState* state, u64 query_id, u64& result, int frames_to_extend_if_not_ready)
{
	QueryIDDecoder decoder;
	decoder.id = query_id;
	u32 index = decoder.index;
	RendererQueryType type = (RendererQueryType)decoder.type;

	auto& query_list = mOcclusionQueries[type];
	auto& query = query_list[index];
	auto frame_index = state->GetFrameNumber();
	
	if (query.frame_of_execution != decoder.frame_index // Query has expired and was already reused
		|| frame_index > query.frame_of_execution + query.frames_to_keep) // Query has expired
	{
		result = 1;
		return true;
	}

	if (frame_index == query.frame_of_execution) return false; // Can't get result until next frame
	
	if (!query.result_ok)
	{
		::BOOL bool_result = 0;
		HRESULT ok = S_FALSE;
		if (type == RENDERER_QUERY_ANY_SAMPLES_PASSED)
		{
			ok = myDXInstance.m_deviceContext->GetData(query.query, &bool_result, sizeof(::BOOL), 0);
			query.result = bool_result ? 1 : 0;
		}
		else
			ok = myDXInstance.m_deviceContext->GetData(query.query, &query.result, sizeof(::UINT64), 0);

		query.result_ok = ok == S_OK;
		
	}

	if (query.result_ok)
	{
		result = query.result;
	}
	else
	{
		query.frames_to_keep += frames_to_extend_if_not_ready;
	}

	return query.result_ok;
}

void RendererDX11::startFrame(TravState* state)
{
	ParentClassType::startFrame(state);
	auto frame_index = state->GetFrameNumber();

	state->mFramesNeededForOcclusion = 2*(1 + (mOcclusionQueriesForFrame / MAX_OCCLUSION_QUERIES));
	mOcclusionQueriesForFrame = 0;

#ifdef KIGS_TOOLS
	gRendererStats.AllocatedBuffers = myVertexBufferManager->GetAllocatedBufferCount();
#endif

	// Manage queries
	for (int type = 0; type < RENDERER_QUERY_TYPE_COUNT; ++type)
	{
		for (u32 i = 0; i < mOcclusionQueries[type].size(); ++i)
		{
			auto& query = mOcclusionQueries[type][i];
			if (frame_index == query.frame_of_execution + query.frames_to_keep + 1)
			{
				mFreeQueryCount[type]++;
			}
		}
	}
#ifdef WUP
	static bool s_canUseWaitForNextFrameReadyAPI = true;
	if (myDXInstance.mCurrentFrame)
	{
		if (s_canUseWaitForNextFrameReadyAPI)
		{
			try
			{
				myDXInstance.mHolographicSpace.WaitForNextFrameReady();
			}
			catch (winrt::hresult_not_implemented const& /*ex*/)
			{
				s_canUseWaitForNextFrameReadyAPI = false;
			}
		}
		else
		{
			myDXInstance.mCurrentFrame.WaitForFrameToFinish();
		}
	}
	if (myDXInstance.mHolographicSpace)
	{
		myDXInstance.mCurrentFrame = myDXInstance.mHolographicSpace.CreateNextFrame();
		std::vector<CMSP> cameras = GetInstances("Camera");
		for (auto cam : cameras)
		{
			auto dxcam = cam->as<Camera>();
			auto rs = dxcam->getRenderingScreen();
			if (!rs) continue;
			if (!rs->IsHolographic()) continue;

			auto prediction = myDXInstance.mCurrentFrame.CurrentPrediction();
			auto poses = prediction.CameraPoses();

			for (auto pose : poses)
			{
				rs->as<DX11RenderingScreen>()->SetRenderingParameters(myDXInstance.mCurrentFrame.GetRenderingParameters(pose));
				rs->as<DX11RenderingScreen>()->CreateResources();
			}

			break;
		}
	}
#endif
}

void RendererDX11::endFrame(TravState* state)
{
	ParentClassType::endFrame(state);


}