#include "common/emulatorConfig.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/threads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;
// gta6-prep: config
static std::atomic<Common::QuirkMask> g_title_quirks {0};

// gta6-prep: vram
static VramSettings g_vram_settings;

static VramRequest VramRequestOf(const ConfigOptions& cfg) {
	VramRequest request;
	request.gc_policy       = cfg.vram_gc;
	request.oom_recovery    = cfg.oom_recovery;
	request.no_oom_recovery = cfg.no_oom_recovery;
	request.profile         = cfg.vram_profile;
	request.rt_arena_mb     = cfg.rt_arena_mb;
	return request;
}

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	Common::DebugCounters::SetEnabled(false);
	g_config.reset();
	g_title_quirks = 0;
	g_vram_settings = {}; // gta6-prep: vram
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));
	EXIT_IF(cfg.controller_speaker_volume > 100 || cfg.controller_vibration_intensity > 100);
	EXIT_IF(cfg.keyboard_player > static_cast<uint32_t>(MAX_PLAYERS));

	*g_config = cfg;
	g_vram_settings = ResolveVramSettings(VramRequestOf(cfg)); // gta6-prep: vram
	Common::DebugCounters::SetEnabled(cfg.perf_overlay_enabled);
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

const std::string& GetAudioInputDevice() {
	return g_config->audio_input_device;
}

const std::optional<ControllerColor>& GetControllerColor() {
	return g_config->controller_color;
}

uint32_t GetControllerSpeakerVolume() {
	return g_config->controller_speaker_volume;
}

uint32_t GetControllerVibrationIntensity() {
	return g_config->controller_vibration_intensity;
}

uint32_t GetKeyboardPlayer() {
	return g_config->keyboard_player;
}

int32_t GetPlayerUserId(int player) {
	return g_config->user_id + player * PLAYER_USER_ID_STEP;
}

int GetPlayerFromUserId(int32_t user_id) {
	for (int player = 0; player < MAX_PLAYERS; player++) {
		if (user_id == GetPlayerUserId(player)) {
			return player;
		}
	}
	return -1;
}

bool IsGuestUserId(int32_t user_id) {
	return GetPlayerFromUserId(user_id) > 0;
}

bool PadHapticRumbleEnabled() {
	return g_config->pad_haptic_rumble;
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

bool HdrEnabled() {
	return g_config->hdr_enabled;
}

bool HideCursorEnabled() {
	return g_config->hide_cursor_enabled;
}

bool StretchEnabled() {
	return g_config->stretch_enabled;
}

bool VrEnabled() {
	return g_config->vr_enabled;
}

bool Ps5ProEnabled() {
	return g_config->pro_enabled;
}

const std::filesystem::path& GetDlssFolder() {
	return g_config->dlss_folder;
}

uint32_t GetDlssPreset() {
	return g_config->dlss_preset;
}

bool AmdCpuEnabled() {
	return g_config->amd_cpu_enabled;
}

bool GuestTscEnabled() {
	return g_config->guest_tsc_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool IsNetworkConnected() {
	return g_config->network_connected;
}

bool PsnEmulationEnabled() {
	return g_config->psn_emulation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

bool ShaderCacheEnabled() {
	return g_config->shader_cache_enabled;
}

bool PipelineManifestEnabled() {
	return g_config->pipeline_manifest_enabled;
}

bool PipelineManifestRecord() {
	return g_config->pipeline_manifest_record;
}

bool PipelineManifestStrict() {
	return g_config->pipeline_manifest_strict;
}

PipelinePrecompile GetPipelinePrecompile() {
	return g_config->pipeline_precompile;
}

LogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

LogDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

std::filesystem::path GetSessionLogFile() {
	return g_config->session_log_file;
}

bool ProfilerEnabled() {
	return g_config->profiler_enabled;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool ReadbackTiledImagesEnabled() {
	return g_config->readback_tiled_images;
}

bool ReadbackStorageImagesEnabled() {
	return g_config->readback_storage_images;
}

bool SyncRawImageBuffersEnabled() {
	return g_config->sync_raw_image_buffers;
}

uint32_t GetFlushDraws() {
	return g_config->flush_draws;
}

bool TessellationEnabled() {
	return g_config->tessellation_enabled;
}

bool OcclusionQueriesEnabled() {
	return g_config->occlusion_queries_enabled;
}

bool RelaxedShaderFloatEnabled() {
	return g_config->relaxed_shader_float;
}

bool AsyncPipelinesEnabled() {
	return g_config->async_pipelines_enabled;
}

bool NativeRayTracingEnabled() {
	return g_config->native_ray_tracing_enabled;
}

bool RayTracingStatsEnabled() {
	return g_config->native_ray_tracing_enabled && g_config->rt_stats_enabled;
}

bool TrophyEnabled() {
	return g_config->trophy_enabled;
}

uint32_t GetTrophyDurationSeconds() {
	return std::clamp(g_config->trophy_duration_seconds, 5u, 10u);
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

bool SkipNoticeScreen() {
	return g_config->skip_notice_screen;
}

uint32_t ParseGpuPipelineStages(const char* value) {
	char*      end    = nullptr;
	const auto parsed = std::strtoul(value, &end, 0);
	if (end == value || *end != '\0') {
		EXIT("invalid GPU pipeline stages: %s\n", value);
	}
	return parsed == 1 ? GPU_PIPELINE_ALL : static_cast<uint32_t>(parsed) & GPU_PIPELINE_ALL;
}

void ConfigureGpuStageThread(GpuStageThread stage) {
	// Five physical cores of the first CCD of a Ryzen 9 5900X (SMT siblings are adjacent), away
	// from core 0: the stages share its L3.
	static const std::array<int, 5> cpus = [] {
		std::array<int, 5> result {4, 6, 8, 10, 2};
		// gta6-prep: config: --gpu-thread-cpus, unless the variable is set.
		const char* value = std::getenv("KYTY_GPU_THREAD_CPUS");
		if (value == nullptr && !GetGpuThreadCpus().empty()) {
			value = GetGpuThreadCpus().c_str();
		}
		if (value == nullptr) {
			return result;
		}
		if (!ParseGpuThreadCpus(value, result)) {
			EXIT("invalid KYTY_GPU_THREAD_CPUS: %s\n", value);
		}
		return result;
	}();
	static const bool high_priority = [] {
		const char* value = std::getenv("KYTY_GPU_THREAD_PRIORITY");
		return value == nullptr || std::string_view(value) != "0";
	}();
	if (high_priority) {
		Common::Thread::RaiseCurrentPriority();
	}
	// Guest threads that spin while they wait for work (Demon's Souls keeps ~12 job workers at
	// ~95% of a core each) otherwise run on the SMT siblings of the pinned stages and slow them
	// down: give the stages whole physical cores.
	static std::once_flag reserved;
	std::call_once(reserved, [] { Common::Thread::ReserveCores(cpus); });
	Common::Thread::PinCurrent(cpus[static_cast<uint32_t>(stage)]);
}

uint32_t GpuPipelineStages() {
	if (const char* value = std::getenv("KYTY_GPU_PIPELINE"); value != nullptr) {
		return ParseGpuPipelineStages(value);
	}
	// Tools and tests that do not load a configuration run the GPU inline.
	return g_config != nullptr ? g_config->gpu_pipeline_stages : 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

const std::vector<std::string>& GetGuestArguments() {
	return g_config->guest_arguments;
}

// gta6-prep: config
void SetTitleQuirks(Common::QuirkMask quirks) {
	g_title_quirks = quirks;
}

bool QuirkEnabled(Common::Quirk quirk) {
	return (g_title_quirks.load(std::memory_order_relaxed) & Common::QuirkBit(quirk)) != 0;
}

bool OverlapDispatchesEnabled() {
	return g_config == nullptr || g_config->overlap_dispatches_enabled;
}

bool DeferDrawBarriersEnabled() {
	return g_config == nullptr || g_config->defer_draw_barriers_enabled;
}

bool MaskedSelectEliminationEnabled() {
	return g_config == nullptr || g_config->masked_select_elimination_enabled;
}

bool SpirvForwardingEnabled() {
	return g_config == nullptr || g_config->spirv_forwarding_enabled;
}

bool UniformBuffersEnabled() {
	return g_config == nullptr || g_config->uniform_buffers_enabled;
}

bool LodStatsEnabled() {
	return g_config == nullptr || g_config->lod_stats_enabled;
}

bool KeepSmallLayerTargets() {
	return g_config == nullptr || g_config->keep_small_layer_targets;
}

bool TlasCacheEnabled() {
	return g_config == nullptr || g_config->tlas_cache_enabled;
}

int32_t GetPipelineHelpers() {
	return g_config != nullptr ? g_config->pipeline_helpers : -1;
}

const std::string& GetGpuThreadCpus() {
	static const std::string none;
	return g_config != nullptr ? g_config->gpu_thread_cpus : none;
}

bool ParseGpuThreadCpus(const char* value, std::array<int, 5>& cpus) {
	if (std::string_view(value) == "off") {
		cpus.fill(-1);
		return true;
	}
	// Stages without a listed CPU keep their default.
	for (auto& cpu: cpus) {
		if (*value == '\0') {
			break;
		}
		char* end = nullptr;
		cpu       = static_cast<int>(std::strtol(value, &end, 10));
		if (end == value) {
			return false;
		}
		value = *end == ',' ? end + 1 : end;
	}
	return true;
}

// gta6-prep: hle
const std::filesystem::path& GetNidNamesFile() {
	static const std::filesystem::path none;
	return g_config != nullptr ? g_config->nid_names_file : none;
}

bool StrictImportsEnabled() {
	return g_config != nullptr && g_config->strict_imports;
}

bool UnresolvedOfflineReturnsEnabled() {
	return g_config != nullptr && g_config->unresolved_offline_returns;
}

bool RebindImportsEnabled() {
	return g_config == nullptr || g_config->rebind_imports;
}

const std::filesystem::path& GetDumpImportsFile() {
	static const std::filesystem::path none;
	return g_config != nullptr ? g_config->dump_imports_file : none;
}

bool NoTypeImportFallbackEnabled() {
	return g_config != nullptr && g_config->notype_import_fallback;
}

// gta6-prep: vram
const VramSettings& GetVramSettings() {
	return g_vram_settings;
}

uint32_t GetVramLimitMb() {
	return g_config != nullptr ? g_config->vram_limit_mb : 0;
}

VideoOutResolution GetVideoOutResolution() {
	return g_config != nullptr ? g_config->video_out_resolution : VideoOutResolution::Auto;
}

bool SkipUnsupportedImageAtomics() {
	return g_config != nullptr && g_config->skip_unsupported_image_atomics;
}

} // namespace Config
