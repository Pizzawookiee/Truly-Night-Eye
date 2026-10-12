#include "ShaderCompiler.h"

#include <bcrypt.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <detours/detours.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <format>
#include <limits>

namespace night_eye
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		constexpr std::size_t kMaximumSourceBytes = 4 * 1024 * 1024;
		constexpr std::size_t kMaximumExpandedBytes = 32 * 1024 * 1024;
		constexpr std::string_view kPixelProfile = "ps_5_0";
		constexpr std::string_view kEntryPoint = "main";
		const std::filesystem::path kAdapterDirectory = L"Data/Shaders/NightEye/Adapter";
		const std::filesystem::path kAddonCacheDirectory = L"Data/SKSE/Plugins/NightEye/ShaderCache";
		const std::filesystem::path kHostSourceDirectory = L"Data/Shaders";
		const std::filesystem::path kHostCacheDirectory = L"Data/ShaderCache";

		decltype(&D3DCompileFromFile) originalCompile = D3DCompileFromFile;
		decltype(&D3DReadFileToBlob) originalRead = D3DReadFileToBlob;
		decltype(&D3DWriteBlobToFile) originalWrite = D3DWriteBlobToFile;

		struct Insertion
		{
			std::string anchor;
			std::string body;
			bool after = false;
		};

		struct Recipe
		{
			std::string version;
			std::string family;
			std::string sourceHash;
			std::vector<Insertion> insertions;
		};

		struct Source
		{
			std::filesystem::file_time_type modified;
			std::uintmax_t bytes = 0;
			std::string adapted;
			std::string version;
			bool recognized = false;
		};

		std::vector<Recipe> recipes;
		std::unordered_map<std::wstring, std::shared_ptr<const Source>> sourceCache;
		std::mutex sourceMutex;
		std::mutex statusMutex;
		std::mutex cacheWriteMutex;
		std::once_flag initialization;
		std::atomic<bool> installed = false;
		std::atomic<std::uint64_t> compiledCount = 0;
		std::atomic<std::uint64_t> cacheHitCount = 0;
		std::atomic<std::uint64_t> fallbackCount = 0;
		// Keep adapted blobs out of the host cache during a concurrent source reload.
		std::atomic<bool> suppressLightingWrites = false;
		std::atomic<bool> suppressEffectWrites = false;
		std::string lastStatus = "Shader adapter not initialized";
		std::atomic<bool> lightingRecognized = false;
		std::atomic<bool> effectRecognized = false;
		std::string lightingVersion;
		std::string effectVersion;

		void SetStatus(std::string message)
		{
			std::scoped_lock lock(statusMutex);
			lastStatus = std::move(message);
		}

		std::wstring NormalizePath(const std::filesystem::path& path)
		{
			auto normalized = std::filesystem::absolute(path).lexically_normal().native();
			std::ranges::transform(normalized, normalized.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
			return normalized;
		}

		std::string Canonical(std::string data)
		{
			if (data.starts_with("\xEF\xBB\xBF"))
				data.erase(0, 3);
			std::string result;
			result.reserve(data.size());
			for (std::size_t index = 0; index < data.size(); ++index) {
				if (data[index] == '\r' && index + 1 < data.size() && data[index + 1] == '\n')
					continue;
				result += data[index];
			}
			return result;
		}

		std::string ReadText(const std::filesystem::path& path)
		{
			const auto bytes = std::filesystem::file_size(path);
			if (bytes == 0 || bytes > kMaximumSourceBytes)
				throw std::runtime_error("Shader asset size is outside the supported bounds");
			std::ifstream stream(path, std::ios::binary);
			std::string data(static_cast<std::size_t>(bytes), '\0');
			if (!stream.read(data.data(), static_cast<std::streamsize>(bytes)))
				throw std::runtime_error("Cannot read shader asset");
			return Canonical(std::move(data));
		}

		std::string SHA256(std::string_view data)
		{
			struct HashHandles
			{
				BCRYPT_ALG_HANDLE algorithm = nullptr;
				BCRYPT_HASH_HANDLE hash = nullptr;
				std::vector<UCHAR> storage;
				~HashHandles()
				{
					if (hash)
						BCryptDestroyHash(hash);
					if (algorithm)
						BCryptCloseAlgorithmProvider(algorithm, 0);
				}
			} handles;
			if (data.size() > std::numeric_limits<ULONG>::max() ||
				BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
				throw std::runtime_error("Cannot open SHA256 provider");
			DWORD objectBytes = 0;
			DWORD returned = 0;
			if (BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0)
				throw std::runtime_error("Cannot query SHA256 storage");
			handles.storage.resize(objectBytes);
			std::array<UCHAR, 32> digest{};
			if (BCryptCreateHash(handles.algorithm, &handles.hash, handles.storage.data(), objectBytes, nullptr, 0, 0) < 0 ||
				BCryptHashData(handles.hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) < 0 ||
				BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
				throw std::runtime_error("Cannot hash shader source");
			std::string result;
			result.reserve(digest.size() * 2);
			constexpr std::string_view digits = "0123456789abcdef";
			for (const auto value : digest) {
				result += digits[value >> 4];
				result += digits[value & 15];
			}
			return result;
		}

		void LoadRecipes()
		{
			const auto document = nlohmann::json::parse(ReadText(kAdapterDirectory / L"Recipes.json"));
			if (document.at("schema").get<unsigned int>() != 1 || !document.at("recipes").is_array() || document.at("recipes").empty())
				throw std::runtime_error("Unsupported shader recipe schema");
			for (const auto& item : document.at("recipes")) {
				Recipe recipe;
				recipe.version = item.at("version").get<std::string>();
				recipe.family = item.at("family").get<std::string>();
				recipe.sourceHash = item.at("sourceSHA256").get<std::string>();
				if ((recipe.family != "Lighting" && recipe.family != "Effect") || recipe.sourceHash.size() != 64 ||
					!std::ranges::all_of(recipe.sourceHash, [](unsigned char value) { return std::isxdigit(value) != 0; }))
					throw std::runtime_error("Invalid shader recipe identity");
				if (!item.at("insertions").is_array() || item.at("insertions").empty() || item.at("insertions").size() > 4)
					throw std::runtime_error("Invalid shader insertion count");
				for (const auto& insertion : item.at("insertions")) {
					const auto assetName = insertion.at("asset").get<std::string>();
					const std::filesystem::path asset(assetName);
					if (asset.has_parent_path() || asset.extension() != ".hlsli")
						throw std::runtime_error("Shader recipe asset must be a local HLSL include");
					Insertion operation;
					operation.anchor = insertion.at("anchor").get<std::string>();
					operation.after = insertion.at("after").get<bool>();
					operation.body = ReadText(kAdapterDirectory / asset);
					if (operation.anchor.empty())
						throw std::runtime_error("Shader recipe anchor is empty");
					if (assetName == "Declarations.hlsli")
						operation.body.insert(0, std::format("#define NIGHT_EYE_ADAPTER_OS217 {}\n", item.at("os217").get<bool>() ? 1 : 0));
					recipe.insertions.push_back(std::move(operation));
				}
				recipes.push_back(std::move(recipe));
			}
		}

		std::shared_ptr<const Source> GetSource(const std::filesystem::path& path)
		{
			const auto normalized = NormalizePath(path);
			const auto parent = std::filesystem::path(normalized).parent_path();
			if (parent.native() != NormalizePath(kHostSourceDirectory))
				return nullptr;
			const auto filename = std::filesystem::path(normalized).filename().native();
			const auto family = filename == L"lighting.hlsl" ? "Lighting" : filename == L"effect.hlsl" ? "Effect" : "";
			if (family[0] == '\0')
				return nullptr;
			std::scoped_lock lock(sourceMutex);
			const auto modified = std::filesystem::last_write_time(path);
			const auto bytes = std::filesystem::file_size(path);
			if (const auto previous = sourceCache.find(normalized); previous != sourceCache.end() && previous->second->modified == modified && previous->second->bytes == bytes)
				return previous->second;
			Source source{ modified, bytes, {}, {}, false };
			auto text = ReadText(path);
			if (text.find("NIGHT_EYE") == std::string::npos && text.find("NightEye/ActorVision") == std::string::npos) {
				const auto digest = SHA256(text);
				const auto recipe = std::ranges::find_if(recipes, [&](const Recipe& entry) { return entry.family == family && entry.sourceHash == digest; });
				if (recipe != recipes.end()) {
					for (const auto& insertion : recipe->insertions) {
						const auto position = text.find(insertion.anchor);
						if (position == std::string::npos || text.find(insertion.anchor, position + insertion.anchor.size()) != std::string::npos)
							throw std::runtime_error("Shader recipe anchor is not unique");
						text.insert(position + (insertion.after ? insertion.anchor.size() : 0), "\n" + insertion.body + "\n");
					}
					source.adapted = std::move(text);
					source.version = recipe->version;
					source.recognized = true;
					spdlog::info("Night Eye shader adapter recognized {} {}", source.version, family);
				} else {
					spdlog::warn("Night Eye shader adapter left unknown {} source unchanged: {}", family, digest);
				}
			} else {
				spdlog::warn("Night Eye shader adapter left legacy Night Eye {} source unchanged", family);
			}
			const auto cached = std::make_shared<const Source>(std::move(source));
			sourceCache.insert_or_assign(normalized, cached);
			{
				std::scoped_lock statusLock(statusMutex);
				if (filename == L"lighting.hlsl") {
					lightingRecognized = cached->recognized;
					lightingVersion = cached->version;
					if (cached->recognized)
						suppressLightingWrites = true;
				} else {
					effectRecognized = cached->recognized;
					effectVersion = cached->version;
					if (cached->recognized)
						suppressEffectWrites = true;
				}
			}
			return cached;
		}

		std::wstring HostCacheFamily(LPCWSTR filename)
		{
			if (!installed || !filename)
				return {};
			const auto path = std::filesystem::path(NormalizePath(filename));
			if (path.extension() != L".pso" || path.parent_path().parent_path().native() != NormalizePath(kHostCacheDirectory))
				return {};
			const auto family = path.parent_path().filename().native();
			if (family != L"lighting" && family != L"effect")
				return {};
			return family;
		}

		bool BypassHostCache(LPCWSTR filename)
		{
			const auto family = HostCacheFamily(filename);
			if (family.empty())
				return false;
			const auto source = GetSource(kHostSourceDirectory / (family + L".hlsl"));
			return source && source->recognized;
		}

		bool IsExcluded(const D3D_SHADER_MACRO* defines)
		{
			if (!defines)
				return false;
			for (const auto* define = defines; define->Name; ++define) {
				const std::string_view name(define->Name);
				if (name == "MULTBLEND" || name == "MULTBLEND_DECAL" || name == "VR")
					return true;
			}
			return false;
		}

		bool IsValidPixelBlob(ID3DBlob* blob)
		{
			if (!blob || blob->GetBufferSize() == 0 || blob->GetBufferSize() > kMaximumExpandedBytes)
				return false;
			ComPtr<ID3D11ShaderReflection> reflection;
			if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(reflection.GetAddressOf()))))
				return false;
			D3D11_SHADER_DESC description{};
			return SUCCEEDED(reflection->GetDesc(&description)) && D3D11_SHVER_GET_TYPE(description.Version) == D3D11_SHVER_PIXEL_SHADER;
		}

		std::string BlobText(ID3DBlob* blob)
		{
			if (!blob)
				return {};
			return std::string(static_cast<const char*>(blob->GetBufferPointer()), std::min<std::size_t>(blob->GetBufferSize(), 2048));
		}

		HRESULT WINAPI CompileFromFile(LPCWSTR filename, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
			LPCSTR entryPoint, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** output, ID3DBlob** errors)
		{
			if (!installed || !filename || !entryPoint || !target || !output ||
				entryPoint != kEntryPoint || target != kPixelProfile || IsExcluded(defines))
				return originalCompile(filename, defines, include, entryPoint, target, flags1, flags2, output, errors);
			try {
				const auto source = GetSource(filename);
				if (!source)
					return originalCompile(filename, defines, include, entryPoint, target, flags1, flags2, output, errors);
				if (!source->recognized) {
					SetStatus("Unsupported host source, original shader retained");
					return originalCompile(filename, defines, include, entryPoint, target, flags1, flags2, output, errors);
				}
				const auto sourceUTF8 = std::filesystem::path(filename).u8string();
				const std::string sourceName(reinterpret_cast<const char*>(sourceUTF8.data()), sourceUTF8.size());
				ComPtr<ID3DBlob> expanded;
				ComPtr<ID3DBlob> preprocessingErrors;
				const auto preprocessResult = D3DPreprocess(source->adapted.data(), source->adapted.size(), sourceName.c_str(), defines, include,
					expanded.GetAddressOf(), preprocessingErrors.GetAddressOf());
				if (FAILED(preprocessResult) || !expanded || expanded->GetBufferSize() > kMaximumExpandedBytes)
					throw std::runtime_error("Adapted preprocessing failed: " + BlobText(preprocessingErrors.Get()));
				std::string cacheInput(static_cast<const char*>(expanded->GetBufferPointer()), expanded->GetBufferSize());
				cacheInput += std::format("\nNightEyeCache:{}:{}:{}:{}", target, entryPoint, flags1, flags2);
				const auto cachePath = kAddonCacheDirectory / (SHA256(cacheInput) + ".pso");
				ComPtr<ID3DBlob> bytecode;
				if (SUCCEEDED(originalRead(cachePath.c_str(), bytecode.GetAddressOf())) && IsValidPixelBlob(bytecode.Get())) {
					*output = bytecode.Detach();
					if (errors)
						*errors = preprocessingErrors.Detach();
					++cacheHitCount;
					SetStatus(source->version + " shader adapter active");
					return S_OK;
				}
				bytecode.Reset();
				ComPtr<ID3DBlob> compilationErrors;
				const auto result = D3DCompile(expanded->GetBufferPointer(), expanded->GetBufferSize(), sourceName.c_str(), nullptr, nullptr,
					entryPoint, target, flags1, flags2, bytecode.GetAddressOf(), compilationErrors.GetAddressOf());
				if (FAILED(result) || !IsValidPixelBlob(bytecode.Get()))
					throw std::runtime_error("Adapted compilation failed: " + BlobText(compilationErrors.Get()));
				{
					std::scoped_lock lock(cacheWriteMutex);
					std::error_code error;
					std::filesystem::create_directories(kAddonCacheDirectory, error);
					if (!error && FAILED(originalWrite(bytecode.Get(), cachePath.c_str(), TRUE)))
						spdlog::warn("Night Eye could not save its shader cache entry {}", cachePath.string());
				}
				*output = bytecode.Detach();
				if (errors)
					*errors = compilationErrors.Detach();
				++compiledCount;
				SetStatus(source->version + " shader adapter active");
				return result;
			} catch (const std::exception& exception) {
				++fallbackCount;
				SetStatus("Original shader fallback: " + std::string(exception.what()));
				spdlog::warn("Night Eye shader adapter kept the original shader: {}", exception.what());
			}
			return originalCompile(filename, defines, include, entryPoint, target, flags1, flags2, output, errors);
		}

		HRESULT WINAPI ReadFileToBlob(LPCWSTR filename, ID3DBlob** output)
		{
			try {
				if (output && BypassHostCache(filename)) {
					*output = nullptr;
					return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
				}
			} catch (const std::exception& exception) {
				spdlog::warn("Night Eye cache read remained unchanged: {}", exception.what());
			}
			return originalRead(filename, output);
		}

		HRESULT WINAPI WriteBlobToFile(ID3DBlob* blob, LPCWSTR filename, BOOL overwrite)
		{
			try {
				const auto family = HostCacheFamily(filename);
				if (blob && ((family == L"lighting" && suppressLightingWrites) || (family == L"effect" && suppressEffectWrites) || BypassHostCache(filename)))
					return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
			} catch (const std::exception& exception) {
				spdlog::warn("Night Eye cache write was blocked: {}", exception.what());
				return E_FAIL;
			}
			return originalWrite(blob, filename, overwrite);
		}
	}

	void InitializeCompiler()
	{
		std::call_once(initialization, [] {
			try {
				if (REL::Module::IsVR())
					throw std::runtime_error("Standalone shader adaptation supports SE and AE");
				LoadRecipes();
				for (const auto* filename : { L"Lighting.hlsl", L"Effect.hlsl" }) {
					const auto path = kHostSourceDirectory / filename;
					if (std::filesystem::exists(path))
						GetSource(path);
				}
				const auto begin = DetourTransactionBegin();
				if (begin != NO_ERROR)
					throw std::runtime_error(std::format("Cannot begin compiler hook transaction: {}", begin));
				auto result = DetourUpdateThread(GetCurrentThread());
				if (result == NO_ERROR)
					result = DetourAttach(reinterpret_cast<PVOID*>(&originalCompile), reinterpret_cast<PVOID>(CompileFromFile));
				if (result == NO_ERROR)
					result = DetourAttach(reinterpret_cast<PVOID*>(&originalRead), reinterpret_cast<PVOID>(ReadFileToBlob));
				if (result == NO_ERROR)
					result = DetourAttach(reinterpret_cast<PVOID*>(&originalWrite), reinterpret_cast<PVOID>(WriteBlobToFile));
				if (result != NO_ERROR) {
					DetourTransactionAbort();
					throw std::runtime_error(std::format("Cannot attach compiler hooks: {}", result));
				}
				result = DetourTransactionCommit();
				if (result != NO_ERROR)
					throw std::runtime_error(std::format("Cannot commit compiler hooks: {}", result));
				installed = true;
				SetStatus("Shader adapter ready for stock Community Shaders 1.8.4 or Open Shaders 2.17.0");
				spdlog::info("Night Eye standalone compiler hooks installed, host PSO cache remains unchanged");
			} catch (const std::exception& exception) {
				SetStatus("Shader adapter unavailable: " + std::string(exception.what()));
				spdlog::error("Night Eye compiler initialization failed: {}", exception.what());
			}
		});
	}

	bool IsGeometryAdapterSupported(RE::BSShader::Type type) noexcept
	{
		if (!installed.load(std::memory_order_relaxed))
			return false;
		if (type == RE::BSShader::Type::Lighting)
			return lightingRecognized.load(std::memory_order_relaxed);
		if (type == RE::BSShader::Type::Effect)
			return effectRecognized.load(std::memory_order_relaxed);
		return false;
	}

	CompilerStatus GetCompilerState()
	{
		std::scoped_lock lock(statusMutex);
		CompilerStatus status;
		status.installed = installed.load();
		status.lightingRecognized = lightingRecognized.load();
		status.effectRecognized = effectRecognized.load();
		status.compiled = compiledCount.load();
		status.cacheHits = cacheHitCount.load();
		status.fallbacks = fallbackCount.load();
		status.version = lightingVersion == effectVersion ? lightingVersion : std::format("Lighting: {}; Effect: {}", lightingVersion, effectVersion);
		status.diagnostic = lastStatus;
		return status;
	}

	std::string GetCompilerStatus()
	{
		const auto status = GetCompilerState();
		return std::format("{} (compiled {}, addon cache {}, fallback {})", status.diagnostic,
			status.compiled, status.cacheHits, status.fallbacks);
	}
}
