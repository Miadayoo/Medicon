#include <medicon/sdk/CustomModelProbe.hpp>
#include <medicon/memory/signature_registry.hpp>

#include <Windows.h>
#include <winver.h>

#include <array>
#include <vector>

namespace medicon::sdk::custom_model {
namespace {
namespace native = bedrock_1_26_52_3;

template<class T> bool copy_at(std::uintptr_t address, T& value) noexcept {
    if (!address || sizeof(T) - 1 > UINTPTR_MAX - address) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             &value, sizeof(T), &copied) && copied == sizeof(T);
}

bool supported_file(std::string& diagnostic) {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    const std::wstring_view full(path.data(), length);
    const auto separator = full.find_last_of(L"\\/");
    const auto name = full.substr(separator == std::wstring_view::npos ? 0 : separator + 1);
    if (_wcsicmp(std::wstring(name).c_str(), L"Minecraft.Windows.exe")) {
        diagnostic = "P0 probe requires Minecraft.Windows.exe 1.26.52.3.";
        return false;
    }
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!size || size > 16 * 1024 * 1024) return false;
    std::vector<std::byte> bytes(size);
    if (!GetFileVersionInfoW(path.data(), 0, size, bytes.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        !info || info_size < sizeof(*info) || info->dwSignature != 0xFEEF04BD ||
        HIWORD(info->dwFileVersionMS) != 1 || LOWORD(info->dwFileVersionMS) != 26 ||
        HIWORD(info->dwFileVersionLS) != 52 || LOWORD(info->dwFileVersionLS) != 3) {
        diagnostic = "P0 probe withheld: file version differs from the IDA evidence (1.26.52.3).";
        return false;
    }
    return true;
}
}

ProbeTargets resolve_probe_targets(std::uint32_t timestamp, std::uint32_t image_size,
    std::span<const memory::MemoryRegion> regions, memory::AddressRange image, std::string& diagnostic) {
    // PE identity from the original executable recorded in custom_model_ida_report.md.
    // This is a version/profile gate, not a claim that a loaded image was SHA-256 verified.
    if (!native::known_image(timestamp, image_size)) {
        diagnostic = native::image_mismatch("P0 probe", timestamp, image_size);
        return {};
    }
    constexpr std::array definitions{
        memory::SignatureDefinition{"LevelRendererCamera_nearActorQueue_P0", native::near_actor_queue_pattern,
            memory::SignatureResolution::direct, 0, 0, native::file_version, false},
        memory::SignatureDefinition{"LevelRendererCamera_ecsActorQueue_P0", native::ecs_actor_queue_pattern,
            memory::SignatureResolution::direct, 0, 0, native::file_version, false},
        memory::SignatureDefinition{"DataDrivenRenderer_render_P0_combined", native::data_driven_render_pattern,
            memory::SignatureResolution::direct, 0, 0, native::file_version, false}};
    memory::SignatureRegistry registry;
    registry.scan(definitions, regions, image);
    std::array<std::uintptr_t, 3> addresses{};
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto* result = registry.find(definitions[i].name);
        if (!result || !result->address) {
            diagnostic = std::string(definitions[i].name) + " withheld: ";
            diagnostic += result ? memory::to_string(result->status) : "no result";
            return {};
        }
        addresses[i] = *result->address;
    }
    if (addresses[0] == addresses[1] || addresses[0] == addresses[2] || addresses[1] == addresses[2]) {
        diagnostic = "P0 combined hooks withheld: distinct entries required.";
        return {};
    }
    diagnostic = "P0 combined near/ECS/render entries uniquely resolved (RVAs 0x4716E80 / 0x47172D0 / 0x558D300); combined visual coverage unverified.";
    return {addresses[0], addresses[1], addresses[2]};
}

ProbeTargets Probe::locate(std::string& diagnostic) {
    diagnostic = "P0 probe could not validate the executable version/profile.";
    status_.store(Status::unavailable, std::memory_order_release);
    if (!supported_file(diagnostic)) return {};
    const auto module = memory::current_process_module(&diagnostic);
    if (!module) return {};
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!copy_at(module->image.address, dos) || dos.e_lfanew <= 0 ||
        static_cast<std::uint32_t>(dos.e_lfanew) > UINTPTR_MAX - module->image.address ||
        !module->image.contains(module->image.address + static_cast<std::uint32_t>(dos.e_lfanew), sizeof(nt)) ||
        !copy_at(module->image.address + static_cast<std::uint32_t>(dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return {};
    return resolve_probe_targets(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage,
                                module->executable_regions, module->image, diagnostic);
}

void Probe::set_ready(bool ready) noexcept {
    status_.store(ready ? Status::ready : Status::unavailable, std::memory_order_release);
}

bool Probe::inspect_near(native::NearActorQueueCallback* callback, native::PlayerCandidate* actor,
                         LocalMatcher match, void* owner) noexcept {
    return inspect(QueuePath::near_actor, callback, actor, match, owner);
}

bool Probe::inspect_ecs(native::EcsActorQueueContext* context, native::ActorOwnerRef* component,
                        LocalMatcher match, void* owner) noexcept {
    return inspect(QueuePath::ecs_actor, context, component, match, owner);
}

bool Probe::inspect_render(native::DataDrivenRenderer* renderer, native::BaseActorRenderContext* context,
                           native::ActorRenderData* data, LocalMatcher match, void* owner,bool allow_suppression) noexcept {
    // All three native inputs are borrowed. Display context only; renderer is
    // checked for null but its layout/vtable is never accessed.
    return inspect(QueuePath::data_driven, renderer ? context : nullptr, data, match, owner,allow_suppression);
}

bool Probe::inspect(QueuePath path, void* callback, void* argument,
                    LocalMatcher match, void* owner,bool allow_suppression) noexcept {
    LARGE_INTEGER start{}, end{};
    QueryPerformanceCounter(&start);
    auto& counters = paths_[static_cast<std::size_t>(path)];
    counters.calls.fetch_add(1, std::memory_order_relaxed);
    bool skip = false;
    if (allow_suppression && enabled() && status() == Status::ready && match &&
        (path != QueuePath::data_driven || suppress_data_driven_.load(std::memory_order_acquire))) {
        void* actor = argument;
        // ECS: IDA 0x1447172E9 loads [RDX+0]. DataDriven: 0x14558D354
        // saves R8 and 0x14558D4FF reads its actor at +0. Copy only this
        // proven field inside each callback; no native layout is retained.
        if (callback && argument && path != QueuePath::near_actor &&
            !copy_at(reinterpret_cast<std::uintptr_t>(argument), actor)) actor = nullptr;
        if (!callback || !actor) {
            counters.invalid.fetch_add(1, std::memory_order_relaxed);
        } else if (match(actor, owner)) {
            counters.local.fetch_add(1, std::memory_order_relaxed);
            counters.actor.store(reinterpret_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
            counters.callback.store(reinterpret_cast<std::uintptr_t>(callback), std::memory_order_relaxed);
            // Recheck OFF after the native local-player getter returns.
            skip = enabled() && status() == Status::ready &&
                (path != QueuePath::data_driven || suppress_data_driven_.load(std::memory_order_acquire));
            if (skip) counters.skipped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    QueryPerformanceCounter(&end);
    ticks_.fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
    return skip;
}

ProbeFrame Probe::take_frame() noexcept {
    ProbeFrame frame{};
    frame.overhead_ticks = ticks_.exchange(0);
    for (std::size_t i = 0; i < paths_.size(); ++i) {
        auto& source = paths_[i];
        auto& sample = frame.paths[i];
        sample = {source.calls.exchange(0), source.local.exchange(0), source.skipped.exchange(0),
                  source.invalid.exchange(0), source.actor.exchange(0), source.callback.exchange(0)};
        frame.calls += sample.calls;
        frame.local += sample.local;
        frame.skipped += sample.skipped;
        frame.invalid += sample.invalid;
        if (sample.actor) { frame.actor = sample.actor; frame.callback = sample.callback; }
    }
    return frame;
}

void Probe::reset() noexcept {
    set_enabled(false);
    set_suppress_data_driven(true);
    status_.store(Status::waiting, std::memory_order_release);
    (void)take_frame();
}
} // namespace medicon::sdk::custom_model
