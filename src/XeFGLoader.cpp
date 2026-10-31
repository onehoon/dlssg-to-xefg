#include "XeFG.h"
#include "Log.h"
#include "Util.h"

#include <Psapi.h>

#include <vector>

namespace
{
	constexpr wchar_t XeFGDllName[] = L"libxess_fg.dll";

	std::filesystem::path ModulePath(HMODULE module)
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(module, path, MAX_PATH);
		return path;
	}

	// Every libxess_fg.dll in the process, the game and Streamline plugins can bring their own copies
	std::vector<HMODULE> LoadedXeFGModules()
	{
		std::vector<HMODULE> modules(1024);
		DWORD needed = 0;

		if (!EnumProcessModules(GetCurrentProcess(), modules.data(), (DWORD)(modules.size() * sizeof(HMODULE)), &needed))
			return {};

		modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
		std::erase_if(modules,
			[](HMODULE m) { return _wcsicmp(ModulePath(m).filename().c_str(), XeFGDllName) != 0; });

		return modules;
	}

	bool TryModule(HMODULE module)
	{
		const auto path = ModulePath(module);

		if (XeFG::Resolve(module))
		{
			Log::Info("XeFG library loaded: {}", path.string());
			return true;
		}

		Log::Warn("Incompatible XeFG library: {}", path.string());
		return false;
	}

	bool Search()
	{
		for (HMODULE module : LoadedXeFGModules())
		{
			if (!TryModule(module))
				continue;

			// Keeps it loaded even if its owner unloads it
			HMODULE pinned = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &pinned);

			return true;
		}

		auto path = ThisModulePath().replace_filename(XeFGDllName);
		HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

		if (!module)
		{
			Log::Error("No compatible libxess_fg.dll is loaded and {} can't be loaded", path.string());
			return false;
		}

		if (TryModule(module))
			return true;

		FreeLibrary(module);
		return false;
	}
} // namespace

bool XeFG::Load()
{
	static const bool loaded = Search();
	return loaded;
}
