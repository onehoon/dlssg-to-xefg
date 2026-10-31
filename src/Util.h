#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// Keeps objects a recorded command list may still use alive for a while, Collect is called once per evaluation.
// ponytail: counts evaluations instead of waiting on a fence, enough for the usual 2-3 frames in flight at 6x
namespace DeferredDestroy
{
	constexpr uint64_t Delay = 32;

	struct Entry
	{
		uint64_t destroyAt;
		std::shared_ptr<void> object;
	};

	inline std::mutex mutex;
	// Leaked on purpose: destroying XeFG contexts from our static destructors at process exit would call into a
	// libxess_fg.dll that may already be torn down
	inline std::vector<Entry>& entries = *new std::vector<Entry>;
	inline uint64_t evaluations = 0;

	inline void Add(std::shared_ptr<void> object)
	{
		std::scoped_lock lock(mutex);
		entries.push_back({ evaluations + Delay, std::move(object) });
	}

	inline void Collect(bool all = false)
	{
		std::vector<Entry> due;
		{
			std::scoped_lock lock(mutex);
			evaluations++;
			auto split = std::partition(entries.begin(), entries.end(),
				[all](const Entry& e) { return !all && e.destroyAt > evaluations; });
			due.assign(std::make_move_iterator(split), std::make_move_iterator(entries.end()));
			entries.erase(split, entries.end());
		}
		// Destroyed here, outside the lock
	}
} // namespace DeferredDestroy

// Full path of this dll
inline std::filesystem::path ThisModulePath()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&ThisModulePath), &module);

	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(module, path, MAX_PATH);
	return path;
}

inline void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after)
{
	if (before == after)
		return;

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmdList->ResourceBarrier(1, &barrier);
}

struct ResolvedSubrect
{
	uint32_t baseX;
	uint32_t baseY;
	uint32_t width;
	uint32_t height;
	bool fallback;
	bool valid;
};

// Keeps the existing NGX behavior: if either size is zero, use the entire resource.
constexpr ResolvedSubrect ResolveSubrect(uint64_t textureWidth, uint32_t textureHeight, uint32_t baseX,
	uint32_t baseY, uint32_t width, uint32_t height)
{
	const bool fallback = width == 0 || height == 0;
	if (fallback)
	{
		baseX = 0;
		baseY = 0;
		width = static_cast<uint32_t>(textureWidth);
		height = textureHeight;
	}

	const bool dimensionsRepresentable = textureWidth <= std::numeric_limits<uint32_t>::max();
	const bool valid = dimensionsRepresentable && textureWidth != 0 && textureHeight != 0 && width != 0 && height != 0 &&
		baseX <= textureWidth && baseY <= textureHeight && width <= textureWidth - baseX &&
		height <= textureHeight - baseY;

	return { baseX, baseY, width, height, fallback, valid };
}

enum class ResourceCopyCompatibility
{
	Compatible,
	Incompatible,
	Unknown,
};

enum class ResourceCopyMismatch
{
	None,
	Dimension,
	Width,
	Height,
	DepthOrArraySize,
	MipLevels,
	SampleCount,
	SampleQuality,
	Format,
};

struct ResourceCopyCheck
{
	ResourceCopyCompatibility compatibility;
	ResourceCopyMismatch mismatch;
};

// Known DXGI typeless families. Unlisted formats remain Unknown so uncertain cases keep the old behavior.
constexpr uint32_t CopyFormatFamily(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 1;
	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
		return 2;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
		return 3;
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
		return 4;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
		return 5;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
		return 6;
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
		return 7;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_D32_FLOAT:
		return 8;
	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
		return 9;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_D16_UNORM:
		return 10;
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
		return 11;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return 12;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return 13;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
		return 14;
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		return 15;
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		return 16;
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
		return 17;
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		return 18;
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
		return 19;
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
		return 20;
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
		return 21;
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return 22;
	default:
		return 0;
	}
}

constexpr bool IsBlockCompressedFormat(DXGI_FORMAT format)
{
	return format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB;
}

constexpr ResourceCopyCheck CheckResourceCopyCompatibility(const D3D12_RESOURCE_DESC& destination,
	const D3D12_RESOURCE_DESC& source)
{
	if (destination.Dimension != source.Dimension)
		return { ResourceCopyCompatibility::Incompatible, ResourceCopyMismatch::Dimension };
	if (destination.DepthOrArraySize != source.DepthOrArraySize)
		return { ResourceCopyCompatibility::Incompatible, ResourceCopyMismatch::DepthOrArraySize };
	if (destination.MipLevels != source.MipLevels)
		return { ResourceCopyCompatibility::Incompatible, ResourceCopyMismatch::MipLevels };
	if (destination.SampleDesc.Count != source.SampleDesc.Count)
		return { ResourceCopyCompatibility::Incompatible, ResourceCopyMismatch::SampleCount };
	if (destination.SampleDesc.Quality != source.SampleDesc.Quality)
		return { ResourceCopyCompatibility::Incompatible, ResourceCopyMismatch::SampleQuality };
	if (destination.Width != source.Width || destination.Height != source.Height)
	{
		// D3D12 has special block-compressed reinterpret copies. Keep these uncertain cases unchanged.
		if (IsBlockCompressedFormat(destination.Format) != IsBlockCompressedFormat(source.Format))
			return { ResourceCopyCompatibility::Unknown, destination.Width != source.Width ? ResourceCopyMismatch::Width : ResourceCopyMismatch::Height };
		return { ResourceCopyCompatibility::Incompatible, destination.Width != source.Width ? ResourceCopyMismatch::Width : ResourceCopyMismatch::Height };
	}
	if (destination.Format == source.Format)
		return { ResourceCopyCompatibility::Compatible, ResourceCopyMismatch::None };

	const auto destinationFamily = CopyFormatFamily(destination.Format);
	const auto sourceFamily = CopyFormatFamily(source.Format);
	if (destinationFamily != 0 && destinationFamily == sourceFamily)
		return { ResourceCopyCompatibility::Compatible, ResourceCopyMismatch::None };
	if (destinationFamily != 0 && sourceFamily != 0)
		return { ResourceCopyCompatibility::Incompatible, ResourceCopyMismatch::Format };
	return { ResourceCopyCompatibility::Unknown, ResourceCopyMismatch::Format };
}

constexpr const char* ResourceCopyMismatchName(ResourceCopyMismatch mismatch)
{
	switch (mismatch)
	{
	case ResourceCopyMismatch::None: return "none";
	case ResourceCopyMismatch::Dimension: return "dimension";
	case ResourceCopyMismatch::Width: return "width";
	case ResourceCopyMismatch::Height: return "height";
	case ResourceCopyMismatch::DepthOrArraySize: return "depth-or-array-size";
	case ResourceCopyMismatch::MipLevels: return "mip-levels";
	case ResourceCopyMismatch::SampleCount: return "sample-count";
	case ResourceCopyMismatch::SampleQuality: return "sample-quality";
	case ResourceCopyMismatch::Format: return "format";
	default: return "unknown";
	}
}

// Both resources are back in their states afterwards
inline void Copy(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* destination, D3D12_RESOURCE_STATES destinationState,
	ID3D12Resource* source, D3D12_RESOURCE_STATES sourceState)
{
	Barrier(cmdList, destination, destinationState, D3D12_RESOURCE_STATE_COPY_DEST);
	Barrier(cmdList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmdList->CopyResource(destination, source);
	Barrier(cmdList, destination, D3D12_RESOURCE_STATE_COPY_DEST, destinationState);
	Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
}

// Typed formats for views on typeless or depth resources. 8-bit sRGB maps to UNORM, typed UAVs can't be sRGB
inline DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R16G16_TYPELESS:
		return DXGI_FORMAT_R16G16_FLOAT;
	case DXGI_FORMAT_R32G32_TYPELESS:
		return DXGI_FORMAT_R32G32_FLOAT;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_UNORM;
	default:
		return format;
	}
}
