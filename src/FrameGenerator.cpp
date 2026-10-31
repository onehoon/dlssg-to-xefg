#include "FrameGenerator.h"
#include "Log.h"
#include "Util.h"

#include <DirectXMath.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

using namespace DirectX;

// States of the DLSSG resources when EvaluateFeature is called (same as dlssg-to-fsr3 assumes)
constexpr auto BackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto InputState = D3D12_RESOURCE_STATE_COPY_DEST; // depth, mvecs, hudless
constexpr auto OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// What XeFG wants for its inputs, our copies always rest in this state
constexpr auto XeFGInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// XeFG's default network. libxess_fg.dll doesn't ship its multi frame variant (16), which Init would switch to
constexpr int32_t XeFGModel = 18;

FrameGenerator::Context::~Context()
{
	if (handle)
	{
		if (auto result = XeFG::Destroy(handle); result != XEFGD_RESULT_SUCCESS)
			Log::Warn("XeFG Destroy error: {}", result);
	}
}

static void XeFGLogCallback(const char* message, xefgd_logging_level_t level, void*)
{
	switch (level)
	{
	case XEFGD_LOGGING_LEVEL_ERROR:
		Log::Error("XeFG: {}", message);
		break;
	case XEFGD_LOGGING_LEVEL_WARNING:
		Log::Warn("XeFG: {}", message);
		break;
	default:
		Log::Info("XeFG: {}", message);
		break;
	}
}

static ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, D3D12_RESOURCE_DESC desc,
	D3D12_RESOURCE_STATES state, const wchar_t* name)
{
	ComPtr<ID3D12Resource> texture;
	D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };

	if (HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
		IID_PPV_ARGS(&texture));
		FAILED(hr))
	{
		Log::Error("CreateCommittedResource error: {:X}", (uint32_t)hr);
		return nullptr;
	}

	texture->SetName(name);
	return texture;
}

// Depth copies become plain color textures where the format allows, anything can read those through SRVs
static void MakeReadableDepth(D3D12_RESOURCE_DESC& desc)
{
	switch (desc.Format)
	{
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		desc.Format = TypedFormat(desc.Format);
		desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		break;
	default:
		break;
	}
}

static const char* ResourceDimensionName(D3D12_RESOURCE_DIMENSION dimension)
{
	switch (dimension)
	{
	case D3D12_RESOURCE_DIMENSION_BUFFER: return "buffer";
	case D3D12_RESOURCE_DIMENSION_TEXTURE1D: return "texture1d";
	case D3D12_RESOURCE_DIMENSION_TEXTURE2D: return "texture2d";
	case D3D12_RESOURCE_DIMENSION_TEXTURE3D: return "texture3d";
	default: return "unknown";
	}
}

static const char* FormatName(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
	case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
	case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
	case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
	case DXGI_FORMAT_R16G16_TYPELESS: return "R16G16_TYPELESS";
	case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
	case DXGI_FORMAT_R32G32_TYPELESS: return "R32G32_TYPELESS";
	case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
	case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
	case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
	case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
	case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
	case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
	case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
	case DXGI_FORMAT_NV12: return "NV12";
	default: return "other";
	}
}

static std::string DescribeResource(std::string_view name, ID3D12Resource* resource)
{
	if (!resource)
		return std::format("{}{{present=no}}", name);

	const auto desc = resource->GetDesc();
	return std::format("{}{{present=yes,dim={}({}),size={}x{}x{},mips={},format={}({}),flags=0x{:X},samples={}/{}}}",
		name, ResourceDimensionName(desc.Dimension), static_cast<uint32_t>(desc.Dimension), desc.Width, desc.Height,
		desc.DepthOrArraySize, desc.MipLevels, FormatName(desc.Format), static_cast<uint32_t>(desc.Format),
		static_cast<uint32_t>(desc.Flags), desc.SampleDesc.Count, desc.SampleDesc.Quality);
}

std::string FrameGenerator::DescribeSubrect(std::string_view name, const SubrectInfo& info)
{
	if (!info.hasResource)
		return std::format("{}Subrect{{resource=absent}}", name);

	const uint32_t provided = (info.baseXProvided ? 1u : 0u) | (info.baseYProvided ? 2u : 0u) |
		(info.widthProvided ? 4u : 0u) | (info.heightProvided ? 8u : 0u);
	const char* fallbackReason = !info.fallback ? "no" :
		(!info.widthProvided || !info.heightProvided ? "full:missing-size" : "full:zero-size");
	return std::format("{}Subrect{{providedMask=0x{:X},raw=({},{},{},{}),fallback={},resolved=({},{},{},{}),valid={}}}",
		name, provided, info.rawBaseX, info.rawBaseY, info.rawWidth, info.rawHeight, fallbackReason,
		info.region.base.x, info.region.base.y, info.region.size.x, info.region.size.y, info.valid ? "yes" : "no");
}

static XMFLOAT3 GetVector(const NGXParameter& params, const char* name)
{
	auto key = [name](const char* axis) { return std::format("DLSSG.{}{}", name, axis); };
	return { params.GetOr(key("X").c_str(), 0.0f), params.GetOr(key("Y").c_str(), 0.0f),
			 params.GetOr(key("Z").c_str(), 0.0f) };
}

// XeFG's scene change detection uses the forward axis
static void GetViewMatrix(const NGXParameter& params, float* out)
{
	const XMFLOAT3 pos = GetVector(params, "CameraPos");
	const XMFLOAT3 right = GetVector(params, "CameraRight");
	const XMFLOAT3 up = GetVector(params, "CameraUp");
	const XMFLOAT3 forward = GetVector(params, "CameraFwd");

	// All zero disables XeFG's scene change detection
	if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f)
		return;

	XMVECTOR p = XMLoadFloat3(&pos);
	XMMATRIX view = { XMVectorSet(right.x, up.x, forward.x, 0.0f), XMVectorSet(right.y, up.y, forward.y, 0.0f),
					  XMVectorSet(right.z, up.z, forward.z, 0.0f),
					  XMVectorSet(-XMVectorGetX(XMVector3Dot(p, XMLoadFloat3(&right))),
								  -XMVectorGetX(XMVector3Dot(p, XMLoadFloat3(&up))),
								  -XMVectorGetX(XMVector3Dot(p, XMLoadFloat3(&forward))), 1.0f) };

	memcpy(out, view.r, sizeof(view));
}

// Near/far/fov from a perspective view to clip matrix, dlssg-to-fsr3's approach. Near and far come out in depth
// buffer order
static bool CameraFromViewToClip(const NGXParameter& params, float& cameraNear, float& cameraFar, float& fov)
{
	float(*viewToClip)[4] = nullptr;
	params.Get("DLSSG.CameraViewToClip", reinterpret_cast<void**>(&viewToClip));

	if (params.GetOr<uint32_t>("DLSSG.OrthoProjection", 0) || !viewToClip)
		return false;

	// Some games (RTX Remix ones) pass zero or identity
	float m[4][4] = {};
	if (memcmp(viewToClip, m, sizeof(m)) == 0)
		return false;

	m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
	if (memcmp(viewToClip, m, sizeof(m)) == 0)
		return false;

	const double b = viewToClip[1][1];
	const double c = viewToClip[2][2];
	const double d = viewToClip[3][2];
	const double e = viewToClip[2][3];

	if (e < 0.0)
	{
		cameraNear = (float)(c == 0.0 ? 0.0 : d / c);
		cameraFar = (float)(d / (c + 1.0));
	}
	else
	{
		cameraNear = (float)(c == 0.0 ? 0.0 : -d / c);
		cameraFar = (float)(-d / (c - 1.0));
	}

	fov = (float)(2.0 * std::atan(1.0 / b));

	return cameraNear >= 0.0f && cameraFar >= 0.0f;
}

// Unreal's executables live in Binaries\<platform> and end with -Shipping
static bool IsUnrealEngine()
{
	static const bool isUnreal = []
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			_wcslwr_s(path);

			std::wstring_view exe = path;
			return exe.find(L"\\binaries\\win64\\") != exe.npos || exe.find(L"\\binaries\\wingdk\\") != exe.npos ||
				exe.ends_with(L"-shipping.exe");
		}();

	return isUnreal;
}

struct ProjectionDiagnostics
{
	float outputAspect = 0.0f;
	float cameraAspect = 0.0f;
	float usedAspect = 0.0f;
	bool cameraAspectProvided = false;
	bool matrixGenerated = false;
	const char* cameraPath = "not-evaluated";
};

// Left handed perspective matrix (row vectors) matching the depth buffer: inverted depth swaps near and far,
// cameraFar == 0 means an infinite far plane
static bool GetProjectionMatrix(const NGXParameter& params, bool depthInverted, float aspectRatio, float* out,
	ProjectionDiagnostics& diagnostics)
{
	const float outputAspect = aspectRatio;
	float cameraNear = 0.0f;
	float cameraFar = 0.0f;
	float fov = 0.0f;

	// Unreal doesn't pass a correct CameraViewToClip, its separate camera values are fine
	if (!IsUnrealEngine() && CameraFromViewToClip(params, cameraNear, cameraFar, fov))
	{
		diagnostics.cameraPath = "CameraViewToClip";
		if (depthInverted)
			std::swap(cameraNear, cameraFar);
	}
	else
	{
		diagnostics.cameraPath = "CameraNearFarFOV";
		cameraNear = params.GetOr("DLSSG.CameraNear", 0.0f);
		cameraFar = params.GetOr("DLSSG.CameraFar", 0.0f);
		fov = params.GetOr("DLSSG.CameraFOV", 0.0f);

		// Some games pass degrees, some radians
		if (fov > 10.0f)
			fov *= std::numbers::pi_v<float> / 180.0f;
	}

	float paramAspectRatio = 0.0f;
	diagnostics.cameraAspectProvided = params.Get("DLSSG.CameraAspectRatio", &paramAspectRatio) == NGX_SUCCESS;
	diagnostics.outputAspect = outputAspect;
	diagnostics.cameraAspect = paramAspectRatio;
	if (paramAspectRatio > 0.0f)
		aspectRatio = paramAspectRatio;
	diagnostics.usedAspect = aspectRatio;

	if (cameraNear <= 0.0f || cameraFar < 0.0f || fov <= 0.0f || aspectRatio <= 0.0f)
		return false;

	const bool infinite = cameraFar == 0.0f;
	const float h = 1.0f / std::tan(fov * 0.5f);
	const float w = h / aspectRatio;

	// z_ndc = (a * z + b) / z
	float a;
	float b;

	if (infinite)
	{
		a = depthInverted ? 0.0f : 1.0f;
		b = depthInverted ? cameraNear : -cameraNear;
	}
	else
	{
		if (XMScalarNearEqual(cameraNear, cameraFar, 0.00001f))
			cameraFar++;

		const float range = cameraFar - cameraNear;
		a = depthInverted ? -cameraNear / range : cameraFar / range;
		b = depthInverted ? cameraNear * cameraFar / range : -cameraNear * cameraFar / range;
	}

	const float projection[16] = { w, 0, 0, 0, 0, h, 0, 0, 0, 0, a, 1, 0, 0, b, 0 };
	memcpy(out, projection, sizeof(projection));
	diagnostics.matrixGenerated = true;
	return true;
}

bool FrameGenerator::CreateContext()
{
	auto newContext = std::make_shared<Context>();

	// Process wide, they also affect other XeFG contexts. Must happen before the context picks its model
	XeFG::SetMaxInterpolatedFrames(MaxFrames);
	XeFG::OverrideModel(XeFGModel);

	if (auto result = XeFG::CreateContext(device.Get(), &newContext->handle); result != XEFGD_RESULT_SUCCESS)
	{
		Log::Error("XeFG CreateContext error: {}", result);
		return false;
	}

	XeFG::SetLoggingCallback(newContext->handle, XEFGD_LOGGING_LEVEL_INFO, XeFGLogCallback);

	xefgd_init_params_t initParams{};
	initParams.outputResolution = { settings.width, settings.height };
	initParams.backBufferFormat = settings.format;
	initParams.initFlags = settings.initFlags;
	initParams.maxInterpolatedFrames = MaxFrames;
	initParams.uiMode = settings.uiMode;

	Log::Info("Creating XeFG context: {}x{}, format: {}, flags: {:X}, ui mode: {}", settings.width, settings.height,
		(uint32_t)settings.format, settings.initFlags, settings.uiMode);

	if (auto result = XeFG::Init(newContext->handle, initParams); result != XEFGD_RESULT_SUCCESS)
	{
		Log::Error("XeFG Init error: {}", result);
		return false;
	}

	context = std::move(newContext);
	return true;
}

FrameGenerator::SubrectInfo FrameGenerator::ReadSubrect(const NGXParameter& params, const char* name,
	ID3D12Resource* resource)
{
	SubrectInfo info{};
	if (!resource)
		return info;

	info.hasResource = true;
	auto key = [name](const char* field) { return std::format("DLSSG.{}Subrect{}", name, field); };
	info.baseXProvided = params.Get(key("BaseX").c_str(), &info.rawBaseX) == NGX_SUCCESS;
	info.baseYProvided = params.Get(key("BaseY").c_str(), &info.rawBaseY) == NGX_SUCCESS;
	info.widthProvided = params.Get(key("Width").c_str(), &info.rawWidth) == NGX_SUCCESS;
	info.heightProvided = params.Get(key("Height").c_str(), &info.rawHeight) == NGX_SUCCESS;

	const auto desc = resource->GetDesc();
	const auto resolved = ResolveSubrect(desc.Width, desc.Height, info.rawBaseX, info.rawBaseY,
		info.rawWidth, info.rawHeight);
	info.region.base = { resolved.baseX, resolved.baseY };
	info.region.size = { resolved.width, resolved.height };
	info.fallback = resolved.fallback;
	info.valid = resolved.valid;
	return info;
}

void FrameGenerator::ReportDiagnosticIssue(std::string_view level, std::string key, std::string message)
{
	if (key != lastIssueKey)
	{
		lastIssueKey = std::move(key);
		lastIssueCount = 1;
		Log::Write(level, message);
		return;
	}

	lastIssueCount++;
	if ((lastIssueCount & (lastIssueCount - 1)) == 0)
		Log::Write(level, std::format("{} (same issue observed {} times)", message, lastIssueCount));
}

void FrameGenerator::BeginFrameDiagnostics(const NGXParameter& params)
{
	diagnosticRealFrameCount++;
	pendingFrameDiagnostics.clear();
	pendingCopyDiagnostics.clear();
	pendingCallDiagnostics.clear();
	std::fill(executeResults.begin(), executeResults.end(), std::numeric_limits<int32_t>::min());

	auto backbuffer = params.GetOr<ID3D12Resource*>("DLSSG.Backbuffer", nullptr);
	auto hudless = params.GetOr<ID3D12Resource*>("DLSSG.HUDLess", nullptr);
	auto depth = params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr);
	auto mvecs = params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr);
	auto outputReal = params.GetOr<ID3D12Resource*>("DLSSG.OutputReal", nullptr);
	auto output = params.GetOr<ID3D12Resource*>("DLSSG.OutputInterpolated", nullptr);

	pendingFrameDiagnostics = std::format("{}; {}; {}; {}; {}; {}",
		DescribeResource("Backbuffer", backbuffer), DescribeResource("HUDLess", hudless),
		DescribeResource("Depth", depth), DescribeResource("MVecs", mvecs),
		DescribeResource("OutputReal", outputReal), DescribeResource("OutputInterpolated", output));

	const char* subrectNames[] = { "Backbuffer", "HUDLess", "Depth", "MVecs" };
	ID3D12Resource* subrectResources[] = { backbuffer, hudless, depth, mvecs };
	for (size_t i = 0; i < std::size(pendingSubrects); i++)
	{
		pendingSubrects[i] = ReadSubrect(params, subrectNames[i], subrectResources[i]);
		pendingFrameDiagnostics += std::format("; {}", DescribeSubrect(subrectNames[i], pendingSubrects[i]));

		if (pendingSubrects[i].hasResource && !pendingSubrects[i].valid)
		{
			const std::string key = std::format("subrect:{}:{}", subrectNames[i], DescribeSubrect(subrectNames[i], pendingSubrects[i]));
			ReportDiagnosticIssue("warn", key,
				std::format("Invalid {} Subrect for {}", subrectNames[i], DescribeResource(subrectNames[i], subrectResources[i])));
		}
	}

	float cameraAspect = 0.0f;
	const bool cameraAspectProvided = params.Get("DLSSG.CameraAspectRatio", &cameraAspect) == NGX_SUCCESS;
	const uint32_t enableInterp = params.GetOr<uint32_t>("DLSSG.EnableInterp", 1);
	const uint32_t reset = params.GetOr<uint32_t>("DLSSG.Reset", 0);
	const uint32_t depthInverted = params.GetOr<uint32_t>("DLSSG.DepthInverted", 0);
	const uint32_t mvecJittered = params.GetOr<uint32_t>("DLSSG.MvecJittered", 0);
	const uint32_t requestedFrameIndex = params.GetOr<uint32_t>("DLSSG.MultiFrameIndex", 1);
	const uint32_t requestedFrameCount = params.GetOr<uint32_t>("DLSSG.MultiFrameCount", 1);
	pendingFrameDiagnostics += std::format(
		"; NGX{{MultiFrameIndex={},MultiFrameCount={},Reset={},EnableInterp={},DepthInverted={},MvecJittered={},CameraAspectRatio={{provided={},value={}}}}}",
		requestedFrameIndex, requestedFrameCount, reset, enableInterp, depthInverted, mvecJittered,
		cameraAspectProvided ? "yes" : "no", cameraAspect);
	pendingFrameSignature = pendingFrameDiagnostics;
}

void FrameGenerator::FinishFrameDiagnostics(uint32_t frameIndex, uint32_t frameCount, NGXResult result)
{
	if (pendingFrameDiagnostics.find("XeFGInit{") == std::string::npos)
		pendingFrameDiagnostics += std::format("; XeFGInit{{resolution=not-created-this-call,contextAvailable={}}}",
			context ? "yes" : "no");
	if (pendingFrameDiagnostics.find("Projection{") == std::string::npos)
		pendingFrameDiagnostics += "; Projection{path=not-evaluated}";
	pendingFrameSignature = pendingFrameDiagnostics;

	std::string outputs;
	std::string bases = "not-created";
	if (context)
	{
		bases.clear();
		for (uint32_t i = 0; i < frameCount; i++)
		{
			if (i)
				bases += ",";
			bases += std::format("{}:{}", context->outputBases[i].x, context->outputBases[i].y);
			outputs += std::format("; XeFGOutput[{}]={}", i, DescribeResource("resource", context->outputs[i].Get()));
		}
	}

	std::string executions;
	for (uint32_t i = 0; i < frameCount; i++)
	{
		if (i)
			executions += ",";
		if (executeResults[i] == std::numeric_limits<int32_t>::min())
			executions += "not-run";
		else
			executions += std::format("{}", executeResults[i]);
	}

	const std::string copyDetails = pendingCopyDiagnostics.empty() ? "none" : pendingCopyDiagnostics;
	const std::string signature = std::format("{}; calls={}; copies={}; outputBases={}; execute={}; ngxResult=0x{:X}{}",
		pendingFrameSignature, pendingCallDiagnostics, copyDetails, bases, executions, result, outputs);
	if (diagnosticRealFrameCount <= 120 || signature != lastFrameSignature)
	{
		Log::Info("Frame diagnostics #{} (call {}/{}): {}", diagnosticRealFrameCount, frameIndex, frameCount, signature);
		lastFrameSignature = signature;
	}
}

bool FrameGenerator::CopyChecked(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* destination,
	D3D12_RESOURCE_STATES destinationState, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
	std::string_view operation)
{
	if (!destination || !source)
	{
		const std::string message = std::format("Blocked CopyResource [{}]: destination/source is null", operation);
		ReportDiagnosticIssue("error", std::format("copy:{}:null", operation), message);
		pendingCopyDiagnostics += std::format("; {}=blocked:null", operation);
		return false;
	}

	const auto destinationDesc = destination->GetDesc();
	const auto sourceDesc = source->GetDesc();
	const auto check = CheckResourceCopyCompatibility(destinationDesc, sourceDesc);
	if (check.compatibility == ResourceCopyCompatibility::Incompatible)
	{
		const std::string message = std::format("Blocked CopyResource [{}], mismatch={}: destination {}; source {}",
			operation, ResourceCopyMismatchName(check.mismatch), DescribeResource("destination", destination),
			DescribeResource("source", source));
		const std::string key = std::format("copy:{}:{}:{}:{}", operation,
			ResourceCopyMismatchName(check.mismatch), DescribeResource("destination", destination),
			DescribeResource("source", source));
		ReportDiagnosticIssue("error", key, message);
		pendingCopyDiagnostics += std::format("; {}=blocked:{}", operation, ResourceCopyMismatchName(check.mismatch));
		return false;
	}

	if (check.compatibility == ResourceCopyCompatibility::Unknown)
	{
		const std::string message = std::format("CopyResource [{}] compatibility is unknown ({}): destination {}; source {}; preserving existing copy",
			operation, ResourceCopyMismatchName(check.mismatch), DescribeResource("destination", destination),
			DescribeResource("source", source));
		const std::string key = std::format("copy-unknown:{}:{}:{}", operation,
			DescribeResource("destination", destination), DescribeResource("source", source));
		ReportDiagnosticIssue("warn", key, message);
		pendingCopyDiagnostics += std::format("; {}=copied:unknown", operation);
	}
	else
	{
		pendingCopyDiagnostics += std::format("; {}=copied:compatible", operation);
	}

	Copy(cmdList, destination, destinationState, source, sourceState);
	return true;
}

FrameGenerator::CaptureResult FrameGenerator::Capture(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
	D3D12_RESOURCE_STATES sourceState, ComPtr<ID3D12Resource>& copy, std::string_view operation, bool isDepth)
{
	if (!source)
	{
		copy.Reset();
		return CaptureResult::Unavailable;
	}

	auto desc = source->GetDesc();
	desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

	if (isDepth)
		MakeReadableDepth(desc);

	if (copy)
	{
		const auto copyDesc = copy->GetDesc();
		if (copyDesc.Dimension != desc.Dimension || copyDesc.Alignment != desc.Alignment || copyDesc.Width != desc.Width ||
			copyDesc.Height != desc.Height || copyDesc.DepthOrArraySize != desc.DepthOrArraySize ||
			copyDesc.MipLevels != desc.MipLevels || copyDesc.Format != desc.Format ||
			copyDesc.SampleDesc.Count != desc.SampleDesc.Count || copyDesc.SampleDesc.Quality != desc.SampleDesc.Quality ||
			copyDesc.Layout != desc.Layout || copyDesc.Flags != desc.Flags)
		{
			// The old one can still be in use by the GPU.
			DeferredDestroy::Add(std::make_shared<ComPtr<ID3D12Resource>>(std::move(copy)));
		}
	}

	if (!copy)
		copy = CreateTexture(device.Get(), desc, XeFGInputState, L"dlssg-to-xefg input copy");

	if (!copy)
		return CaptureResult::Failed;

	if (!CopyChecked(cmdList, copy.Get(), XeFGInputState, source, sourceState, operation))
		return CaptureResult::CopyIncompatible;

	return CaptureResult::Captured;
}

bool FrameGenerator::CaptureRealFrame(ID3D12GraphicsCommandList* cmdList, const NGXParameter& params)
{
	static bool warnedCameraMotion = false;
	if (params.GetOr<uint32_t>("DLSSG.CameraMotionIncluded", 1) == 0 && !std::exchange(warnedCameraMotion, true))
		Log::Warn("DLSSG.CameraMotionIncluded is 0, the camera's motion is missing from the motion vectors");

	auto backbuffer = params.GetOr<ID3D12Resource*>("DLSSG.Backbuffer", nullptr);
	auto hudless = params.GetOr<ID3D12Resource*>("DLSSG.HUDLess", nullptr);
	auto depth = params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr);
	auto mvecs = params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr);
	auto output = params.GetOr<ID3D12Resource*>("DLSSG.OutputInterpolated", nullptr);

	const bool depthInverted = params.GetOr<uint32_t>("DLSSG.DepthInverted", 0) != 0;

	Settings wanted;
	auto outputDesc = output->GetDesc();
	wanted.width = (uint32_t)outputDesc.Width;
	wanted.height = outputDesc.Height;
	wanted.format = TypedFormat(outputDesc.Format);
	wanted.initFlags = (depthInverted ? XEFGD_INIT_FLAG_INVERTED_DEPTH : 0) |
		(params.GetOr<uint32_t>("DLSSG.MvecJittered", 0) ? XEFGD_INIT_FLAG_JITTERED_MV : 0);
	wanted.uiMode = hudless ? XEFGD_UI_MODE_BACKBUFFER_HUDLESS : XEFGD_UI_MODE_AUTO;

	if (wanted != settings)
	{
		settings = wanted;
		createFailed = false;

		if (context)
			DeferredDestroy::Add(std::move(context));
	}

	if (!context && !createFailed)
		createFailed = !CreateContext();

	if (!context)
	{
		pendingFrameDiagnostics += std::format(
			"; XeFGInit{{resolution={}x{},matchesOutputInterpolated={},contextCreated=no}}; Projection{{path=not-evaluated}}",
			settings.width, settings.height,
			(uint64_t)settings.width == outputDesc.Width && settings.height == outputDesc.Height ? "yes" : "no");
		pendingFrameSignature = pendingFrameDiagnostics;
		return true;
	}

	context->current ^= 1;
	auto& frame = context->frames[context->current];
	auto& previous = context->frames[context->current ^ 1];

	const auto backbufferCapture = Capture(cmdList, backbuffer, BackbufferState, frame.backbuffer, "Capture Backbuffer");
	const auto depthCapture = Capture(cmdList, depth, InputState, frame.depth, "Capture Depth", true);
	const auto mvecsCapture = Capture(cmdList, mvecs, InputState, frame.mvecs, "Capture MVecs");
	const auto hudlessCapture = Capture(cmdList, hudless, InputState, frame.hudless, "Capture HUDLess");
	if (backbufferCapture == CaptureResult::CopyIncompatible || depthCapture == CaptureResult::CopyIncompatible ||
		mvecsCapture == CaptureResult::CopyIncompatible || hudlessCapture == CaptureResult::CopyIncompatible)
	{
		context->pairValid = false;
		frame.valid = false;
		return false;
	}
	const bool captured = backbufferCapture == CaptureResult::Captured && depthCapture == CaptureResult::Captured &&
		mvecsCapture == CaptureResult::Captured;

	auto& data = frame.data;
	data = {};

	if (captured)
	{
		data.backBuffer = pendingSubrects[0].region;
		data.depth = pendingSubrects[2].region;
		data.motionVectors = pendingSubrects[3].region;

		if (hudless)
			data.hudlessColor = pendingSubrects[1].region;
	}

	GetViewMatrix(params, data.viewMatrix);
	ProjectionDiagnostics projection{};
	GetProjectionMatrix(params, depthInverted, (float)settings.width / (float)settings.height,
		data.projectionMatrix, projection);
	const auto& backbufferRegion = pendingSubrects[0].region;
	const float subrectAspect = backbufferRegion.size.y == 0 ? 0.0f :
		(float)backbufferRegion.size.x / (float)backbufferRegion.size.y;
	pendingFrameDiagnostics += std::format(
		"; XeFGInit{{resolution={}x{},matchesOutputInterpolated={},contextCreated=yes}}; Projection{{path={},outputAspect={},backbufferSubrectAspect={},cameraAspectProvided={},cameraAspect={},usedAspect={},matrixGenerated={}}}",
		settings.width, settings.height,
		(uint64_t)settings.width == outputDesc.Width && settings.height == outputDesc.Height ? "yes" : "no",
		projection.cameraPath, projection.outputAspect, subrectAspect,
		projection.cameraAspectProvided ? "yes" : "no", projection.cameraAspect, projection.usedAspect,
		projection.matrixGenerated ? "yes" : "no");

	data.outputResolution[0] = (float)settings.width;
	data.outputResolution[1] = (float)settings.height;
	data.jitterOffset[0] = params.GetOr("DLSSG.JitterOffsetX", 0.0f);
	data.jitterOffset[1] = params.GetOr("DLSSG.JitterOffsetY", 0.0f);

	// Unlike Streamline's constant, the NGX parameter already converts to pixels
	data.motionVectorScale[0] = params.GetOr("DLSSG.MvecScaleX", 1.0f);
	data.motionVectorScale[1] = params.GetOr("DLSSG.MvecScaleY", 1.0f);

	if (data.motionVectorScale[0] == 0.0f || data.motionVectorScale[1] == 0.0f)
		data.motionVectorScale[0] = data.motionVectorScale[1] = 1.0f;

	const bool reset = params.GetOr<uint32_t>("DLSSG.Reset", 0) != 0;
	data.resetHistory = reset;

	// XeFG needs both frames to have hudless or neither
	context->pairValid = captured && previous.valid && !reset && (frame.hudless != nullptr) == (previous.hudless != nullptr);
	frame.valid = captured;
	pendingFrameSignature = pendingFrameDiagnostics;
	return true;
}

FrameGenerator::InterpolateResult FrameGenerator::Interpolate(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_DESC outputDesc,
	uint32_t frameIndex, uint32_t frameCount)
{
	if (!context || !context->pairValid)
		return InterpolateResult::Skipped;

	outputDesc.Format = settings.format;
	outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	for (uint32_t i = 0; i < frameCount; i++)
	{
		if (!context->outputs[i])
			context->outputs[i] = CreateTexture(device.Get(), outputDesc, OutputState, L"dlssg-to-xefg output");

		if (!context->outputs[i])
			return InterpolateResult::Failed;
	}

	auto fill = [this](const Frame& frame, xefgd_frame_t& out)
		{
			out = frame.data;
			out.backBuffer.pResource = frame.backbuffer.Get();
			out.depth.pResource = frame.depth.Get();
			out.motionVectors.pResource = frame.mvecs.Get();
			out.hudlessColor.pResource = frame.hudless.Get();
		};

	xefgd_exec_params_t params{};
	fill(context->frames[context->current ^ 1], params.prev);
	fill(context->frames[context->current], params.next);

	// XeFG wants the same array for every frame of a pair
	static_assert(sizeof(ComPtr<ID3D12Resource>) == sizeof(ID3D12Resource*));
	params.ppOutputs = reinterpret_cast<ID3D12Resource* const*>(context->outputs);
	params.pOutputBases = context->outputBases;
	params.numInterpolatedFrames = (uint8_t)frameCount;
	params.uiCompositionEnabled = settings.uiMode != XEFGD_UI_MODE_AUTO;

	// Frame 1 runs the per pair work and generates its frame, the rest only generate theirs
	params.firstFrameIndex = frameIndex - 1;
	params.frameCount = 1;

	auto result = XeFG::Execute(context->handle, cmdList, params);
	executeResults[frameIndex - 1] = result;

	if (result == XEFGD_RESULT_SUCCESS)
		return InterpolateResult::Interpolated;

	if (result > 0)
	{
		// Warnings, like prev and next areas differing after a dynamic resolution change
		return InterpolateResult::Skipped;
	}

	ReportDiagnosticIssue("error", std::format("xefg-execute:{}", result),
		std::format("XeFG Execute error: {}, frame {}/{}", result, frameIndex, frameCount));
	context->pairValid = false;
	return InterpolateResult::Failed;
}

NGXResult FrameGenerator::Evaluate(ID3D12GraphicsCommandList* cmdList, const NGXParameter& params)
{
	auto backbuffer = params.GetOr<ID3D12Resource*>("DLSSG.Backbuffer", nullptr);
	auto output = params.GetOr<ID3D12Resource*>("DLSSG.OutputInterpolated", nullptr);
	auto outputReal = params.GetOr<ID3D12Resource*>("DLSSG.OutputReal", nullptr);
	const bool enabled = params.GetOr<uint32_t>("DLSSG.EnableInterp", 1) != 0;
	const uint32_t frameCount = std::clamp(params.GetOr<uint32_t>("DLSSG.MultiFrameCount", 1), 1u, MaxFrames);
	const uint32_t frameIndex = std::clamp(params.GetOr<uint32_t>("DLSSG.MultiFrameIndex", 1), 1u, frameCount);
	if (frameIndex == 1)
		BeginFrameDiagnostics(params);
	const uint32_t rawFrameIndex = params.GetOr<uint32_t>("DLSSG.MultiFrameIndex", 1);
	const uint32_t rawFrameCount = params.GetOr<uint32_t>("DLSSG.MultiFrameCount", 1);
	pendingCallDiagnostics += std::format("[raw={}/{},used={}/{}]", rawFrameIndex, rawFrameCount,
		frameIndex, frameCount);

	if (!backbuffer || !output)
	{
		Log::Error("Missing backbuffer or output");
		FinishFrameDiagnostics(frameIndex, frameCount, NGX_FAIL_INVALID_PARAMETER);
		return NGX_FAIL_INVALID_PARAMETER;
	}

	if (outputReal)
	{
		if (!CopyChecked(cmdList, outputReal, OutputState, backbuffer, BackbufferState, "OutputReal <- Backbuffer"))
		{
			FinishFrameDiagnostics(frameIndex, frameCount, NGX_FAIL);
			return NGX_FAIL;
		}
	}

	if (!enabled)
	{
		if (context)
			context->pairValid = false;

		if (frameIndex == frameCount || frameIndex == 1)
			FinishFrameDiagnostics(frameIndex, frameCount, NGX_SUCCESS);
		return NGX_SUCCESS;
	}

	if (!params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr) ||
		!params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr))
	{
		Log::Error("Missing depth or motion vectors");
		FinishFrameDiagnostics(frameIndex, frameCount, NGX_FAIL_INVALID_PARAMETER);
		return NGX_FAIL_INVALID_PARAMETER;
	}

	if (frameIndex == 1)
	{
		if (!CaptureRealFrame(cmdList, params))
		{
			FinishFrameDiagnostics(frameIndex, frameCount, NGX_FAIL);
			return NGX_FAIL;
		}
	}

	const auto result = Interpolate(cmdList, output->GetDesc(), frameIndex, frameCount);

	// Without a usable pair (first frame, reset) the real frame is shown again, that's not an error
	if (result == InterpolateResult::Interpolated)
	{
		if (!CopyChecked(cmdList, output, OutputState, context->outputs[frameIndex - 1].Get(), OutputState,
			"OutputInterpolated <- XeFGOutput"))
		{
			FinishFrameDiagnostics(frameIndex, frameCount, NGX_FAIL);
			return NGX_FAIL;
		}
	}
	else
	{
		if (!CopyChecked(cmdList, output, OutputState, backbuffer, BackbufferState,
			"OutputInterpolated <- Backbuffer fallback"))
		{
			FinishFrameDiagnostics(frameIndex, frameCount, NGX_FAIL);
			return NGX_FAIL;
		}
	}

	const NGXResult finalResult = result == InterpolateResult::Failed ? NGX_FAIL : NGX_SUCCESS;
	if (frameIndex == frameCount)
		FinishFrameDiagnostics(frameIndex, frameCount, finalResult);
	return finalResult;
}
