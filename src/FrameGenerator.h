#pragma once

#include "NGX.h"
#include "XeFG.h"

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

// One DLSSG feature instance, driving one XeFG context
class FrameGenerator
{
public:
	// 6x
	static constexpr uint32_t MaxFrames = 5;

	explicit FrameGenerator(ID3D12Device* device) : device(device)
	{
		executeResults.fill(std::numeric_limits<int32_t>::min());
	}

	// Called once per generated frame, DLSSG.MultiFrameIndex 1 starts a new real frame
	NGXResult Evaluate(ID3D12GraphicsCommandList* cmdList, const NGXParameter& params);

private:
	// What the context was created for, it's recreated when any of it changes
	struct Settings
	{
		uint32_t width = 0;
		uint32_t height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t initFlags = 0;
		uint32_t uiMode = XEFGD_UI_MODE_AUTO;

		bool operator==(const Settings&) const = default;
	};

	// Copies of a real frame's inputs, XeFG reads them after the game moved on
	struct Frame
	{
		ComPtr<ID3D12Resource> backbuffer;
		ComPtr<ID3D12Resource> hudless;
		ComPtr<ID3D12Resource> depth;
		ComPtr<ID3D12Resource> mvecs;
		xefgd_frame_t data{};
		bool valid = false;
	};

	struct SubrectInfo
	{
		xefgd_resource_t region{};
		uint32_t rawBaseX = 0;
		uint32_t rawBaseY = 0;
		uint32_t rawWidth = 0;
		uint32_t rawHeight = 0;
		bool baseXProvided = false;
		bool baseYProvided = false;
		bool widthProvided = false;
		bool heightProvided = false;
		bool fallback = false;
		bool valid = false;
		bool hasResource = false;
	};

	struct Context
	{
		xefgd_handle_t handle = nullptr;
		ComPtr<ID3D12Resource> outputs[MaxFrames];
		xefgd_2d_t outputBases[MaxFrames]{};

		// Ping-ponged between the previous and the current real frame
		Frame frames[2];
		uint32_t current = 0;

		// The previous and the current real frame can be interpolated
		bool pairValid = false;

		~Context();
	};

	enum class InterpolateResult
	{
		Interpolated,
		Skipped, // no usable pair, the real frame is shown instead
		Failed,
	};

	bool CreateContext();
	void BeginFrameDiagnostics(const NGXParameter& params);
	void FinishFrameDiagnostics(uint32_t frameIndex, uint32_t frameCount, NGXResult result);
	void ReportDiagnosticIssue(std::string_view level, std::string key, std::string message);
	static std::string DescribeSubrect(std::string_view name, const SubrectInfo& info);
	bool CopyChecked(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* destination,
		D3D12_RESOURCE_STATES destinationState, ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState,
		std::string_view operation);
	enum class CaptureResult
	{
		Captured,
		Unavailable,
		Failed,
		CopyIncompatible,
	};
	CaptureResult Capture(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
		D3D12_RESOURCE_STATES sourceState, ComPtr<ID3D12Resource>& copy, std::string_view operation,
		bool isDepth = false);
	bool CaptureRealFrame(ID3D12GraphicsCommandList* cmdList, const NGXParameter& params);
	static SubrectInfo ReadSubrect(const NGXParameter& params, const char* name, ID3D12Resource* resource);
	InterpolateResult Interpolate(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_DESC outputDesc,
		uint32_t frameIndex, uint32_t frameCount);

	ComPtr<ID3D12Device> device;
	Settings settings;
	std::shared_ptr<Context> context;
	bool createFailed = false; // retried when the settings change

	uint64_t diagnosticRealFrameCount = 0;
	std::string pendingFrameDiagnostics;
	std::string pendingFrameSignature;
	std::string pendingCopyDiagnostics;
	std::string pendingCallDiagnostics;
	std::array<SubrectInfo, 4> pendingSubrects{};
	std::string lastFrameSignature;
	std::string lastIssueKey;
	uint64_t lastIssueCount = 0;
	std::array<int32_t, MaxFrames> executeResults{};
};
