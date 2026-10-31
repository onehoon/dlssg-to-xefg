// The nvngx_dlssg exports. Only DX12, XeFG has no Vulkan API
#include "FrameGenerator.h"
#include "Log.h"
#include "Util.h"

#include <dxgi.h>

#include <atomic>

#define NGX_EXPORT extern "C" __declspec(dllexport)

// The handle given out is ours, the caller hands it back untouched
struct FeatureHandle : NGXHandle
{
	std::shared_ptr<FrameGenerator> generator;
};

static NGXResult Init()
{
	static const bool logged = []
		{
			Log::Info("dlssg-to-xefg, XeFG replacing DLSS Frame Generation");
			return true;
		}();

	return XeFG::Load() ? NGX_SUCCESS : NGX_FAIL_FEATURE_NOT_SUPPORTED;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_Init(unsigned long long, const wchar_t*, ID3D12Device*, const void*, uint32_t)
{
	return Init();
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t,
	const void*)
{
	return Init();
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_Shutdown()
{
	DeferredDestroy::Collect(true);
	return NGX_SUCCESS;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*) { return NVSDK_NGX_D3D12_Shutdown(); }

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_GetScratchBufferSize(uint32_t, const NGXParameter*, size_t* size)
{
	if (!size)
		return NGX_FAIL_INVALID_PARAMETER;

	*size = 0;
	return NGX_SUCCESS;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter*, const void*,
	NGXFeatureRequirement* requirement)
{
	if (!requirement)
		return NGX_FAIL_INVALID_PARAMETER;

	requirement->FeatureSupported = 0;
	requirement->MinHWArchitecture = 0;
	strcpy_s(requirement->MinOSVersion, "10.0.0");

	return NGX_SUCCESS;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* cmdList, uint32_t feature,
	NGXParameter* params, NGXHandle** handle)
{
	if (!cmdList || !params || !handle)
		return NGX_FAIL_INVALID_PARAMETER;

	if (feature != NGX_FEATURE_FRAME_GENERATION || Init() != NGX_SUCCESS)
		return NGX_FAIL_FEATURE_NOT_SUPPORTED;

	ComPtr<ID3D12Device> device;
	if (FAILED(cmdList->GetDevice(IID_PPV_ARGS(&device))))
		return NGX_FAIL_PLATFORM_ERROR;

	static std::atomic_uint32_t nextId = 1;

	// The XeFG context is created on the first evaluation, once the resources are known
	*handle = new FeatureHandle{ { nextId++ }, std::make_shared<FrameGenerator>(device.Get()) };

	return NGX_SUCCESS;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_ReleaseFeature(NGXHandle* handle)
{
	if (!handle)
		return NGX_FAIL_INVALID_PARAMETER;

	auto feature = static_cast<FeatureHandle*>(handle);
	DeferredDestroy::Add(std::move(feature->generator));
	delete feature;

	return NGX_SUCCESS;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* cmdList, const NGXHandle* handle,
	NGXParameter* params, void*)
{
	if (!cmdList || !handle || !params)
		return NGX_FAIL_INVALID_PARAMETER;

	DeferredDestroy::Collect();

	return static_cast<const FeatureHandle*>(handle)->generator->Evaluate(cmdList, *params);
}

static NGXResult GetCurrentSettingsCallback(NGXHandle* handle, NGXParameter* params)
{
	if (!handle || !params)
		return NGX_FAIL_INVALID_PARAMETER;

	params->Set("DLSSG.MustCallEval", 1);
	params->Set("DLSSG.BurstCaptureRunning", 0);

	return NGX_SUCCESS;
}

static NGXResult EstimateVRAMCallback(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
	uint32_t, size_t* estimatedSize)
{
	// A guess, same as dlssg-to-fsr3
	if (estimatedSize)
		*estimatedSize = 300 * 1024 * 1024;

	return NGX_SUCCESS;
}

NGX_EXPORT NGXResult NVSDK_NGX_D3D12_PopulateParameters_Impl(NGXParameter* params)
{
	if (!params)
		return NGX_FAIL_INVALID_PARAMETER;

	params->Set("DLSSG.GetCurrentSettingsCallback", reinterpret_cast<void*>(&GetCurrentSettingsCallback));
	params->Set("DLSSG.EstimateVRAMCallback", reinterpret_cast<void*>(&EstimateVRAMCallback));
	params->Set("DLSSG.MultiFrameCountMax", (int)FrameGenerator::MaxFrames);

	return NGX_SUCCESS;
}
