#pragma once

#include "NGX.h"
#include "XeFG.h"

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

// One DLSSG feature instance, driving one XeFG context
class FrameGenerator
{
public:
	// 6x
	static constexpr uint32_t MaxFrames = 5;

	explicit FrameGenerator(ID3D12Device* device) : device(device) {}

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
		bool backbufferSubrectValid = false;
		bool valid = false;
	};

	struct SubrectInfo
	{
		xefgd_resource_t region{};
		uint32_t rawBaseX = 0;
		uint32_t rawBaseY = 0;
		uint32_t rawWidth = 0;
		uint32_t rawHeight = 0;
		uint32_t providedMask = 0;
		bool valid = false;
		bool hasResource = false;
	};

	struct SubrectParameterKeys
	{
		const char* name;
		const char* baseX;
		const char* baseY;
		const char* width;
		const char* height;
	};

	struct OutputLayout
	{
		uint64_t outputWidth = 0;
		uint32_t outputHeight = 0;
		uint32_t regionX = 0;
		uint32_t regionY = 0;
		uint32_t regionWidth = 0;
		uint32_t regionHeight = 0;
		uint32_t outputBaseX = 0;
		uint32_t outputBaseY = 0;
		bool usesSubrect = false;

		bool operator==(const OutputLayout&) const = default;
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
	void ResolveFrameSubrects(const NGXParameter& params);
	void ReportIssue(std::string_view level, std::string key, std::string message);
	void SetOutputBase(uint32_t x, uint32_t y);
	void LogOutputLayout(const SubrectInfo& subrect, D3D12_RESOURCE_DESC outputDesc,
		uint32_t outputBaseX, uint32_t outputBaseY, bool usesSubrect);
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
	static SubrectInfo ReadSubrect(const NGXParameter& params, const SubrectParameterKeys& keys,
		ID3D12Resource* resource);
	InterpolateResult Interpolate(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_DESC outputDesc,
		uint32_t frameIndex, uint32_t frameCount);

	ComPtr<ID3D12Device> device;
	Settings settings;
	std::shared_ptr<Context> context;
	bool createFailed = false; // retried when the settings change

	std::array<SubrectInfo, 4> frameSubrects{};
	std::unordered_map<std::string, uint64_t> issueCounts;
	OutputLayout lastOutputLayout{};
	bool hasOutputLayout = false;
};
