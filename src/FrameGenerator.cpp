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
		break;
	}
}

static ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, D3D12_RESOURCE_DESC desc,
	D3D12_RESOURCE_STATES state, const wchar_t* name, HRESULT& result)
{
	ComPtr<ID3D12Resource> texture;
	D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };

	result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
	if (FAILED(result))
		return nullptr;

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

static const char* UiModeName(uint32_t mode)
{
	switch (mode)
	{
	case XEFGD_UI_MODE_AUTO: return "auto";
	case XEFGD_UI_MODE_BACKBUFFER_HUDLESS: return "backbuffer-hudless";
	default: return "unknown";
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

// Left handed perspective matrix (row vectors) matching the depth buffer: inverted depth swaps near and far,
// cameraFar == 0 means an infinite far plane
static bool GetProjectionMatrix(const NGXParameter& params, bool depthInverted, float aspectRatio, float* out)
{
	float cameraNear = 0.0f;
	float cameraFar = 0.0f;
	float fov = 0.0f;

	// Unreal doesn't pass a correct CameraViewToClip, its separate camera values are fine
	if (!IsUnrealEngine() && CameraFromViewToClip(params, cameraNear, cameraFar, fov))
	{
		if (depthInverted)
			std::swap(cameraNear, cameraFar);
	}
	else
	{
		cameraNear = params.GetOr("DLSSG.CameraNear", 0.0f);
		cameraFar = params.GetOr("DLSSG.CameraFar", 0.0f);
		fov = params.GetOr("DLSSG.CameraFOV", 0.0f);

		// Some games pass degrees, some radians
		if (fov > 10.0f)
			fov *= std::numbers::pi_v<float> / 180.0f;
	}

	float paramAspectRatio = 0.0f;
	params.Get("DLSSG.CameraAspectRatio", &paramAspectRatio);
	if (paramAspectRatio > 0.0f)
		aspectRatio = paramAspectRatio;

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

	XeFG::SetLoggingCallback(newContext->handle, XEFGD_LOGGING_LEVEL_WARNING, XeFGLogCallback);

	xefgd_init_params_t initParams{};
	initParams.outputResolution = { settings.width, settings.height };
	initParams.backBufferFormat = settings.format;
	initParams.initFlags = settings.initFlags;
	initParams.maxInterpolatedFrames = MaxFrames;
	initParams.uiMode = settings.uiMode;

	if (auto result = XeFG::Init(newContext->handle, initParams); result != XEFGD_RESULT_SUCCESS)
	{
		Log::Error("XeFG Init error: {}", result);
		return false;
	}

	Log::Info("XeFG context created: {}x{}, format={}, flags=0x{:X}, uiMode={}", settings.width, settings.height,
		FormatName(settings.format), settings.initFlags, UiModeName(settings.uiMode));
	context = std::move(newContext);
	return true;
}

FrameGenerator::SubrectInfo FrameGenerator::ReadSubrect(const NGXParameter& params, const SubrectParameterKeys& keys,
	ID3D12Resource* resource)
{
	SubrectInfo info{};
	if (!resource)
		return info;

	info.hasResource = true;
	info.providedMask = (params.Get(keys.baseX, &info.rawBaseX) == NGX_SUCCESS ? 1u : 0u) |
		(params.Get(keys.baseY, &info.rawBaseY) == NGX_SUCCESS ? 2u : 0u) |
		(params.Get(keys.width, &info.rawWidth) == NGX_SUCCESS ? 4u : 0u) |
		(params.Get(keys.height, &info.rawHeight) == NGX_SUCCESS ? 8u : 0u);

	const auto desc = resource->GetDesc();
	const auto resolved = ResolveSubrect(desc.Width, desc.Height, info.rawBaseX, info.rawBaseY,
		info.rawWidth, info.rawHeight);
	info.region.base = { resolved.baseX, resolved.baseY };
	info.region.size = { resolved.width, resolved.height };
	info.valid = resolved.valid;
	return info;
}

void FrameGenerator::ResolveFrameSubrects(const NGXParameter& params)
{
	static constexpr SubrectParameterKeys keys[] = {
		{ "Backbuffer", "DLSSG.BackbufferSubrectBaseX", "DLSSG.BackbufferSubrectBaseY",
			"DLSSG.BackbufferSubrectWidth", "DLSSG.BackbufferSubrectHeight" },
		{ "HUDLess", "DLSSG.HUDLessSubrectBaseX", "DLSSG.HUDLessSubrectBaseY",
			"DLSSG.HUDLessSubrectWidth", "DLSSG.HUDLessSubrectHeight" },
		{ "Depth", "DLSSG.DepthSubrectBaseX", "DLSSG.DepthSubrectBaseY",
			"DLSSG.DepthSubrectWidth", "DLSSG.DepthSubrectHeight" },
		{ "MVecs", "DLSSG.MVecsSubrectBaseX", "DLSSG.MVecsSubrectBaseY",
			"DLSSG.MVecsSubrectWidth", "DLSSG.MVecsSubrectHeight" },
	};
	ID3D12Resource* resources[] = {
		params.GetOr<ID3D12Resource*>("DLSSG.Backbuffer", nullptr),
		params.GetOr<ID3D12Resource*>("DLSSG.HUDLess", nullptr),
		params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr),
		params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr),
	};

	for (size_t i = 0; i < frameSubrects.size(); i++)
	{
		auto& info = frameSubrects[i];
		info = ReadSubrect(params, keys[i], resources[i]);
		if (info.hasResource && !info.valid)
		{
			const auto& region = info.region;
			ReportIssue("warn", std::format("subrect:{}:invalid", keys[i].name),
				std::format("Invalid {} Subrect: provided=0x{:X}, raw=({},{},{},{}), resolved=({},{},{},{}), resource={}",
					keys[i].name, info.providedMask, info.rawBaseX, info.rawBaseY, info.rawWidth, info.rawHeight,
					region.base.x, region.base.y, region.size.x, region.size.y,
					DescribeResource(keys[i].name, resources[i])));
		}
	}
}

void FrameGenerator::ReportIssue(std::string_view level, std::string key, std::string message)
{
	auto issue = issueCounts.find(key);
	if (issue == issueCounts.end())
	{
		constexpr size_t MaxTrackedIssues = 128;
		if (issueCounts.size() >= MaxTrackedIssues)
			issueCounts.clear();
		issue = issueCounts.emplace(std::move(key), 0).first;
	}

	const uint64_t count = ++issue->second;
	if (count == 1 || (count & (count - 1)) == 0)
		Log::Write(level, count == 1 ? message : std::format("{} (same issue observed {} times)", message, count));
}

void FrameGenerator::SetOutputBase(uint32_t x, uint32_t y)
{
	if (context)
		std::fill(std::begin(context->outputBases), std::end(context->outputBases), xefgd_2d_t{ x, y });
}

void FrameGenerator::LogOutputLayout(const SubrectInfo& subrect, D3D12_RESOURCE_DESC outputDesc,
	uint32_t outputBaseX, uint32_t outputBaseY, bool usesSubrect)
{
	if (!subrect.valid)
		return;

	const auto& region = subrect.region;
	const OutputLayout layout{ outputDesc.Width, outputDesc.Height, region.base.x, region.base.y, region.size.x,
		region.size.y, outputBaseX, outputBaseY, usesSubrect };
	if (hasOutputLayout && layout == lastOutputLayout)
		return;

	if (usesSubrect)
	{
		Log::Info("Letterbox region: ({},{},{},{}); output base=({},{})", region.base.x, region.base.y,
			region.size.x, region.size.y, outputBaseX, outputBaseY);
	}
	else if (hasOutputLayout)
	{
		Log::Info("Output layout changed: BackbufferSubrect=({},{},{},{}); output base=({},{})", region.base.x,
			region.base.y, region.size.x, region.size.y, outputBaseX, outputBaseY);
	}

	lastOutputLayout = layout;
	hasOutputLayout = true;
}

bool FrameGenerator::CopyChecked(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* destination,
	D3D12_RESOURCE_STATES destinationState, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
	std::string_view operation)
{
	if (!destination || !source)
	{
		const std::string message = std::format("Blocked CopyResource [{}]: destination/source is null", operation);
		ReportIssue("error", std::format("copy:{}:null", operation), message);
		return false;
	}
	if (!AreDistinctCopyResources(destination, source))
	{
		const std::string message = std::format("Blocked CopyResource [{}]: source and destination are the same resource: {}",
			operation, DescribeResource("resource", destination));
		ReportIssue("error", std::format("copy:{}:same-resource", operation), message);
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
		ReportIssue("error", std::format("copy:{}:{}", operation, ResourceCopyMismatchName(check.mismatch)), message);
		return false;
	}

	if (check.compatibility == ResourceCopyCompatibility::Unknown)
	{
		const std::string message = std::format("CopyResource [{}] compatibility is unknown ({}): destination {}; source {}; preserving existing copy",
			operation, ResourceCopyMismatchName(check.mismatch), DescribeResource("destination", destination),
			DescribeResource("source", source));
		ReportIssue("warn", std::format("copy-unknown:{}:{}", operation, ResourceCopyMismatchName(check.mismatch)), message);
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
	{
		HRESULT allocationResult = S_OK;
		copy = CreateTexture(device.Get(), desc, XeFGInputState, L"dlssg-to-xefg input copy", allocationResult);
		if (!copy)
			ReportIssue("error", std::format("texture-allocation:{}", operation),
				std::format("Failed to allocate {} copy {}x{} format={} (HRESULT 0x{:08X})", operation, desc.Width,
					desc.Height, FormatName(desc.Format), static_cast<uint32_t>(allocationResult)));
	}

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
		return true;

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
		SetOutputBase(0, 0);
		return false;
	}
	const bool captured = backbufferCapture == CaptureResult::Captured && depthCapture == CaptureResult::Captured &&
		mvecsCapture == CaptureResult::Captured;
	frame.backbufferSubrectValid = frameSubrects[0].valid;

	auto& data = frame.data;
	data = {};

	if (captured)
	{
		data.backBuffer = frameSubrects[0].region;
		data.depth = frameSubrects[2].region;
		data.motionVectors = frameSubrects[3].region;

		if (hudless)
			data.hudlessColor = frameSubrects[1].region;
	}

	GetViewMatrix(params, data.viewMatrix);
	GetProjectionMatrix(params, depthInverted, (float)settings.width / (float)settings.height,
		data.projectionMatrix);

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
	const auto toResolvedSubrect = [](const xefgd_resource_t& region, bool valid)
		{
			return ResolvedSubrect{ region.base.x, region.base.y, region.size.x, region.size.y, false, valid };
		};
	const ResolvedOutputBase selectedBase = context->pairValid
		? ResolveOutputBase(toResolvedSubrect(frame.data.backBuffer, frame.backbufferSubrectValid),
			toResolvedSubrect(previous.data.backBuffer, previous.backbufferSubrectValid), outputDesc.Width, outputDesc.Height)
		: ResolvedOutputBase{ 0, 0, false };
	SetOutputBase(selectedBase.x, selectedBase.y);
	const auto& currentRegion = frame.data.backBuffer;
	const auto& previousRegion = previous.data.backBuffer;
	const bool sameBackbufferRegion = frame.backbufferSubrectValid && previous.backbufferSubrectValid &&
		currentRegion.base.x == previousRegion.base.x && currentRegion.base.y == previousRegion.base.y &&
		currentRegion.size.x == previousRegion.size.x && currentRegion.size.y == previousRegion.size.y;
	if (context->pairValid && sameBackbufferRegion)
		LogOutputLayout(frameSubrects[0], outputDesc, selectedBase.x, selectedBase.y, selectedBase.usesSubrect);
	frame.valid = captured;
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
		{
			HRESULT allocationResult = S_OK;
			context->outputs[i] = CreateTexture(device.Get(), outputDesc, OutputState, L"dlssg-to-xefg output",
				allocationResult);
			if (!context->outputs[i])
				ReportIssue("error", "texture-allocation:xefg-output",
					std::format("Failed to allocate XeFG output {}x{} format={} (HRESULT 0x{:08X})", outputDesc.Width,
						outputDesc.Height, FormatName(outputDesc.Format), static_cast<uint32_t>(allocationResult)));
		}

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

	if (result == XEFGD_RESULT_SUCCESS)
		return InterpolateResult::Interpolated;

	if (result > 0)
	{
		// Warnings, like prev and next areas differing after a dynamic resolution change
		return InterpolateResult::Skipped;
	}

	ReportIssue("error", std::format("xefg-execute:{}", result),
		std::format("XeFG Execute failed with result {} for {}x{} output", result, settings.width, settings.height));
	context->pairValid = false;
	SetOutputBase(0, 0);
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
		ResolveFrameSubrects(params);

	if (!backbuffer || !output)
	{
		const char* missing = !backbuffer && !output ? "Backbuffer, OutputInterpolated" :
			(!backbuffer ? "Backbuffer" : "OutputInterpolated");
		ReportIssue("error", "resource-missing:backbuffer-output",
			std::format("Missing required resource(s): {}", missing));
		return NGX_FAIL_INVALID_PARAMETER;
	}

	if (outputReal)
	{
		if (!CopyChecked(cmdList, outputReal, OutputState, backbuffer, BackbufferState, "OutputReal <- Backbuffer"))
			return NGX_FAIL;
	}

	if (!enabled)
	{
		if (context)
			context->pairValid = false;
		SetOutputBase(0, 0);
		return NGX_SUCCESS;
	}

	auto depth = params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr);
	auto mvecs = params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr);
	if (!depth || !mvecs)
	{
		const char* missing = !depth && !mvecs ? "Depth, MVecs" : (!depth ? "Depth" : "MVecs");
		ReportIssue("error", "resource-missing:depth-mvecs",
			std::format("Missing required resource(s): {}", missing));
		return NGX_FAIL_INVALID_PARAMETER;
	}

	if (frameIndex == 1)
	{
		if (!CaptureRealFrame(cmdList, params))
			return NGX_FAIL;
	}

	const auto result = Interpolate(cmdList, output->GetDesc(), frameIndex, frameCount);

	// Without a usable pair (first frame, reset) the real frame is shown again, that's not an error
	if (result == InterpolateResult::Interpolated)
	{
		if (!CopyChecked(cmdList, output, OutputState, context->outputs[frameIndex - 1].Get(), OutputState,
			"OutputInterpolated <- XeFGOutput"))
			return NGX_FAIL;
	}
	else
	{
		if (!CopyChecked(cmdList, output, OutputState, backbuffer, BackbufferState,
			"OutputInterpolated <- Backbuffer fallback"))
			return NGX_FAIL;
	}

	return result == InterpolateResult::Failed ? NGX_FAIL : NGX_SUCCESS;
}
