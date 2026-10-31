#include "../src/Util.h"

#include <cstdio>

namespace
{
	int failures = 0;

	void Check(bool condition, const char* name)
	{
		if (condition)
			return;

		std::printf("FAIL: %s\n", name);
		failures++;
	}

	D3D12_RESOURCE_DESC Texture(uint64_t width, uint32_t height, DXGI_FORMAT format)
	{
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		return desc;
	}
}

int main()
{
	const auto full = ResolveSubrect(1920, 1200, 0, 0, 1920, 1200);
	Check(full.valid && !full.fallback && full.width == 1920 && full.height == 1200,
		"valid full-screen subrect");

	const auto letterbox = ResolveSubrect(1920, 1200, 0, 60, 1920, 1080);
	Check(letterbox.valid && !letterbox.fallback && letterbox.baseY == 60,
		"valid 1920x1080 region inside 1920x1200 resource");

	const auto missing = ResolveSubrect(1920, 1200, 0, 0, 0, 0);
	Check(missing.valid && missing.fallback && missing.width == 1920 && missing.height == 1200,
		"missing subrect uses the existing full-resource fallback");

	const auto zeroWidth = ResolveSubrect(1920, 1200, 0, 60, 0, 1080);
	Check(zeroWidth.valid && zeroWidth.fallback && zeroWidth.baseY == 0,
		"zero width keeps the existing full-resource fallback");
	const auto zeroHeight = ResolveSubrect(1920, 1200, 0, 60, 1920, 0);
	Check(zeroHeight.valid && zeroHeight.fallback && zeroHeight.baseY == 0,
		"zero height keeps the existing full-resource fallback");

	const auto outside = ResolveSubrect(1920, 1200, 0, 60, 1920, 1141);
	Check(!outside.valid, "out-of-bounds subrect is rejected by validation");

	const auto overflow = ResolveSubrect(1920, 1200, UINT32_MAX, 0, 2, 1);
	Check(!overflow.valid, "subrect validation avoids base-plus-size overflow");

	const auto centered169 = ResolveSubrect(1920, 1200, 0, 60, 1920, 1080);
	const auto centeredOutputBase = ResolveOutputBase(centered169, centered169, 1920, 1200);
	Check(centeredOutputBase.x == 0 && centeredOutputBase.y == 60,
		"centered 1920x1080 region in 1920x1200 output uses base (0,60)");
	const auto full1080 = ResolveSubrect(1920, 1080, 0, 0, 1920, 1080);
	const auto full1080OutputBase = ResolveOutputBase(full1080, full1080, 1920, 1080);
	Check(full1080OutputBase.x == 0 && full1080OutputBase.y == 0,
		"full 1920x1080 output keeps origin base");
	const auto missingOutputBase = ResolveOutputBase(missing, missing, 1920, 1200);
	Check(missingOutputBase.x == 0 && missingOutputBase.y == 0,
		"missing subrect fallback keeps origin base");
	const auto native1610 = ResolveSubrect(1920, 1200, 0, 0, 1920, 1200);
	const auto native1610OutputBase = ResolveOutputBase(native1610, native1610, 1920, 1200);
	Check(native1610OutputBase.x == 0 && native1610OutputBase.y == 0,
		"full native 1920x1200 output keeps origin base");
	const auto invalidOutputBase = ResolveOutputBase(outside, centered169, 1920, 1200);
	Check(invalidOutputBase.x == 0 && invalidOutputBase.y == 0,
		"invalid subrect falls back to origin base");
	const auto outOfOutput = ResolveOutputBase(centered169, centered169, 1920, 1139);
	Check(outOfOutput.x == 0 && outOfOutput.y == 0,
		"subrect that exceeds output bounds falls back to origin base");
	const auto shifted169 = ResolveSubrect(1920, 1200, 0, 59, 1920, 1080);
	const auto conflictingOutputBase = ResolveOutputBase(centered169, shifted169, 1920, 1200);
	Check(conflictingOutputBase.x == 0 && conflictingOutputBase.y == 0,
		"different pair regions fall back to origin base");

	auto rgba = Texture(1920, 1200, DXGI_FORMAT_R8G8B8A8_UNORM);
	auto sameRgba = Texture(1920, 1200, DXGI_FORMAT_R8G8B8A8_UNORM);
	auto typelessRgba = Texture(1920, 1200, DXGI_FORMAT_R8G8B8A8_TYPELESS);
	auto bgra = Texture(1920, 1200, DXGI_FORMAT_B8G8R8A8_UNORM);
	auto srgbBgra = Texture(1920, 1200, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
	auto bgra1200 = Texture(1920, 1200, DXGI_FORMAT_B8G8R8A8_UNORM);
	auto rgba1080 = Texture(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
	auto smallRgba = Texture(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
	auto unknownFormat = Texture(1920, 1200, DXGI_FORMAT_NV12);
	auto possibleCompressedReinterpret = Texture(480, 300, DXGI_FORMAT_BC3_UNORM);
	auto rgba128 = Texture(1920, 1200, DXGI_FORMAT_R32G32B32A32_UINT);

	Check(CheckResourceCopyCompatibility(rgba, sameRgba).compatibility == ResourceCopyCompatibility::Compatible,
		"same-size identical formats are compatible");
	int resourceIdentity = 0;
	int otherResourceIdentity = 0;
	Check(!AreDistinctCopyResources(&resourceIdentity, &resourceIdentity),
		"CopyResource rejects identical source and destination objects");
	Check(AreDistinctCopyResources(&resourceIdentity, &otherResourceIdentity),
		"CopyResource accepts distinct source and destination objects");
	Check(CheckResourceCopyCompatibility(rgba, typelessRgba).compatibility == ResourceCopyCompatibility::Compatible,
		"same-family typeless and typed formats are compatible");
	Check(CheckResourceCopyCompatibility(bgra, srgbBgra).compatibility == ResourceCopyCompatibility::Compatible,
		"same-family linear and sRGB formats are compatible");
	Check(!IsBlockCompressedFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "BGRA8 is not a BC format");
	Check(!IsBlockCompressedFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), "BGRA8 sRGB is not a BC format");
	Check(IsBlockCompressedFormat(DXGI_FORMAT_BC3_UNORM), "BC3 is a BC format");
	Check(CheckResourceCopyCompatibility(bgra1200, rgba1080).compatibility == ResourceCopyCompatibility::Incompatible,
		"mismatched BGRA8/RGBA8 dimensions are incompatible");
	Check(CheckResourceCopyCompatibility(rgba, bgra).compatibility == ResourceCopyCompatibility::Incompatible,
		"known different format families are incompatible");
	Check(CheckResourceCopyCompatibility(rgba, smallRgba).compatibility == ResourceCopyCompatibility::Incompatible,
		"different texture dimensions are incompatible");
	Check(CheckResourceCopyCompatibility(rgba, unknownFormat).compatibility == ResourceCopyCompatibility::Unknown,
		"unclassified format pairs remain diagnostic-only");
	Check(CheckResourceCopyCompatibility(possibleCompressedReinterpret, rgba128).compatibility == ResourceCopyCompatibility::Unknown,
		"possible block-compressed reinterpret copies remain diagnostic-only");

	auto multisampled = sameRgba;
	multisampled.SampleDesc.Count = 4;
	Check(CheckResourceCopyCompatibility(rgba, multisampled).compatibility == ResourceCopyCompatibility::Incompatible,
		"different sample counts are incompatible");
	auto mipMismatch = sameRgba;
	mipMismatch.MipLevels = 2;
	Check(CheckResourceCopyCompatibility(rgba, mipMismatch).compatibility == ResourceCopyCompatibility::Incompatible,
		"different mip counts are incompatible");
	auto qualityMismatch = sameRgba;
	qualityMismatch.SampleDesc.Quality = 1;
	Check(CheckResourceCopyCompatibility(rgba, qualityMismatch).compatibility == ResourceCopyCompatibility::Incompatible,
		"different sample qualities are incompatible");
	auto dimensionMismatch = sameRgba;
	dimensionMismatch.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	Check(CheckResourceCopyCompatibility(rgba, dimensionMismatch).compatibility == ResourceCopyCompatibility::Incompatible,
		"different resource dimensions are incompatible");
	auto arrayMismatch = sameRgba;
	arrayMismatch.DepthOrArraySize = 2;
	Check(CheckResourceCopyCompatibility(rgba, arrayMismatch).compatibility == ResourceCopyCompatibility::Incompatible,
		"different depth or array sizes are incompatible");

	if (failures)
		return 1;

	std::puts("All resource diagnostic checks passed.");
	return 0;
}
