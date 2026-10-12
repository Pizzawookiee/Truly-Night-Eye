#include "NightEye.h"

namespace
{
	constexpr auto kPluginName = "NightEye"sv;
	constexpr REL::Version kPluginVersion{
		NIGHT_EYE_VERSION_MAJOR, NIGHT_EYE_VERSION_MINOR, NIGHT_EYE_VERSION_PATCH
	};
	constexpr std::array kSupportedHosts{ REL::Version{ 1, 8, 4 },
        REL::Version{ 1, 9, 0 },  // Bottle Shaders
		REL::Version{ 2, 17, 0 } };
	constexpr std::size_t kTrampolineBytes = 512;
	constexpr DWORD kModulePathCharacters = 32768;

	bool hostSupported = false;
	bool initialized = false;

	bool InitializeLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return false;
		}
		*path /= "NightEye.log";
		auto sink =
			std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto logger = std::make_shared<spdlog::logger>(std::string(kPluginName),
			std::move(sink));
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(logger));
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
		return true;
	}

	bool VerifyHost()
	{
		const auto module = GetModuleHandleW(L"CommunityShaders.dll");
		if (!module) {
			SKSE::log::warn("The graphics host is absent. Night Eye remains inactive.");
			return false;
		}
		const auto metadata = reinterpret_cast<const SKSE::PluginVersionData*>(
			GetProcAddress(module, "SKSEPlugin_Version"));
		if (!metadata || metadata->dataVersion != SKSE::PluginVersionData::kVersion) {
			SKSE::log::warn(
				"The graphics host has no supported SKSE version metadata. "
				"Night Eye remains inactive.");
			return false;
		}
		const auto version = metadata->GetPluginVersion();
		std::array<wchar_t, kModulePathCharacters> modulePath{};
		const auto characters = GetModuleFileNameW(
			module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
		if (!characters || characters >= modulePath.size()) {
			SKSE::log::error(
				"Cannot resolve the graphics host module path, Windows error {}.",
				GetLastError());
			return false;
		}
		const auto informationBytes =
			GetFileVersionInfoSizeW(modulePath.data(), nullptr);
		std::vector<std::byte> information(informationBytes);
		VS_FIXEDFILEINFO* fixedInformation = nullptr;
		UINT fixedBytes = 0;
		if (!informationBytes ||
			!GetFileVersionInfoW(modulePath.data(), 0, informationBytes,
				information.data()) ||
			!VerQueryValueW(information.data(), L"\\",
				reinterpret_cast<void**>(&fixedInformation),
				&fixedBytes) ||
			fixedBytes < sizeof(VS_FIXEDFILEINFO) ||
			fixedInformation->dwSignature != VS_FFI_SIGNATURE) {
			SKSE::log::error(
				"Cannot verify the graphics host file/product version. "
				"Night Eye remains inactive.");
			return false;
		}
		const REL::Version fileVersion{ HIWORD(fixedInformation->dwFileVersionMS),
			LOWORD(fixedInformation->dwFileVersionMS),
			HIWORD(fixedInformation->dwFileVersionLS),
			LOWORD(fixedInformation->dwFileVersionLS) };
		const REL::Version productVersion{
			HIWORD(fixedInformation->dwProductVersionMS),
			LOWORD(fixedInformation->dwProductVersionMS),
			HIWORD(fixedInformation->dwProductVersionLS),
			LOWORD(fixedInformation->dwProductVersionLS)
		};
		SKSE::log::info("Graphics host {}: SKSE {}, file {}, product {}.",
			metadata->GetPluginName(), version.string("."),
			fileVersion.string("."), productVersion.string("."));
		if (version != fileVersion || version != productVersion ||
			std::ranges::find(kSupportedHosts, version) == kSupportedHosts.end()) {
			SKSE::log::warn(
				"The graphics host version is unsupported or inconsistent. "
				"Night Eye remains inactive.");
			return false;
		}
		return true;
	}

	void OnMessage(SKSE::MessagingInterface::Message* message)
	{
		switch (message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			hostSupported = VerifyHost();
			if (hostSupported) {
				night_eye::InitializeCompiler();
			}
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (hostSupported && !initialized) {
				SKSE::AllocTrampoline(kTrampolineBytes);
				night_eye::Initialize();
				initialized = true;
				SKSE::log::info(
					"Night Eye initialized. Settings: "
					"Data/SKSE/Plugins/NightEye/Settings.json.");
			}
			break;
		case SKSE::MessagingInterface::kNewGame:
		case SKSE::MessagingInterface::kPostLoadGame:
			if (hostSupported && initialized) {
				night_eye::OnGameLoaded();
			}
			break;
		}
	}
}  // namespace

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version =
	[]() noexcept {
		SKSE::PluginVersionData version;
		version.PluginName(kPluginName);
		version.PluginVersion(kPluginVersion);
		version.UsesAddressLibrary();
		version.UsesNoStructs();
		version.CompatibleVersions({ SKSE::RUNTIME_SSE_1_6_1170 });
		return version;
	}();

extern "C" __declspec(dllexport) bool SKSEAPI
	SKSEPlugin_Query(const SKSE::QueryInterface* skse,
		SKSE::PluginInfo* information)
{
	information->infoVersion = SKSE::PluginInfo::kVersion;
	information->name = SKSEPlugin_Version.pluginName;
	information->version = SKSEPlugin_Version.pluginVersion;
	return !skse->IsEditor();
}

extern "C" __declspec(dllexport) bool SKSEAPI
	SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
	if (!InitializeLog()) {
		return false;
	}
	SKSE::log::info("Truly Night Eye standalone {} loading.",
		kPluginVersion.string("."));
	SKSE::Init(skse);
	if (REL::Module::get().version() != SKSE::RUNTIME_SSE_1_6_1170) {
		SKSE::log::warn("This beta supports only the verified Skyrim AE 1.6.1170 runtime.");
		return false;
	}
	const auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener("SKSE", OnMessage)) {
		SKSE::log::error("Cannot register the SKSE lifecycle listener.");
		return false;
	}
	return true;
}
