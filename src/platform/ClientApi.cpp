#include <medicon/platform/ClientApi.h>
#include <medicon/client/Client.hpp>
#include <medicon/gui/EdgeLit.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
std::mutex runtime_mutex;
std::unique_ptr<medicon::Client> client;
std::string error;
std::thread::id frame_thread;
bool allocators_set{};
std::unique_ptr<medicon::gui::EdgeLitD3D11> edge_lit;
ImGuiContext* edge_context{};

template<typename F> int boundary(F&& operation) noexcept {
    std::lock_guard lock(runtime_mutex);
    try { error.clear(); return operation(); }
    catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "Unknown error at the Medicon DLL boundary."; }
    return 0;
}

class ContextScope final {
public:
    explicit ContextScope(ImGuiContext* next) : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(next); }
    ~ContextScope() { ImGui::SetCurrentContext(previous_); }
private:
    ImGuiContext* previous_;
};
}

uint32_t MediconApiVersion() { return 1; }
uint32_t MediconImGuiVersion() { return IMGUI_VERSION_NUM; }

int MediconSetImGuiAllocators(MediconAllocFn allocate, MediconFreeFn release, void* user_data) {
    return boundary([&] {
        if (!allocate || !release || frame_thread != std::thread::id{}) {
            error = "Supply host ImGui allocators before the first frame.";
            return 0;
        }
        ImGui::SetAllocatorFunctions(allocate, release, user_data);
        allocators_set = true;
        return 1;
    });
}

int MediconInitialize(const wchar_t* path) {
    return boundary([&] {
        if (client) return 1;
        auto next = std::make_unique<medicon::Client>(path && *path ? std::filesystem::path(path) : medicon::Client::default_config_path());
        if (!next->start()) { error = next->last_error(); return 0; }
        error = next->last_error();
        client = std::move(next);
        frame_thread = {};
        return 1;
    });
}

int MediconFrame(void* context, float seconds) {
    return boundary([&] {
        if (!client || !context || !allocators_set) {
            error = "Initialize the client, set host allocators and supply an active ImGui frame first.";
            return 0;
        }
        const auto current = std::this_thread::get_id();
        if (frame_thread != std::thread::id{} && current != frame_thread) {
            error = "MediconFrame must run on the same UI thread for every frame.";
            return 0;
        }
        frame_thread = current;
        ContextScope scope(static_cast<ImGuiContext*>(context));
        if (edge_context && edge_context != context) {
            medicon::gui::forget_edge_lit_frame(edge_context);
            edge_lit.reset();
        }
        edge_context = static_cast<ImGuiContext*>(context);
        client->tick(seconds);
        client->render();
        return 1;
    });
}

int MediconKeyEvent(int key, int pressed, int repeat) {
    return boundary([&] {
        if (!client) { error = "Client is not initialized."; return 0; }
        if (frame_thread != std::thread::id{} && frame_thread != std::this_thread::get_id()) {
            error = "MediconKeyEvent must run on the frame's UI thread.";
            return 0;
        }
        const bool owned = client->key_event(key, pressed != 0, repeat != 0);
        return owned ? 1 : 0;
    });
}

int MediconCommand(const char* input, char* reply, uint32_t capacity) {
    return boundary([&] {
        if (!client || !input) { error = "Initialize the client and supply a command."; return 0; }
        if (frame_thread != std::thread::id{} && frame_thread != std::this_thread::get_id()) {
            error = "MediconCommand must run on the frame's UI thread.";
            return 0;
        }
        const auto result = client->execute_command(input);
        if (reply && capacity) {
            const auto length = std::min<std::size_t>(result.message.size(), capacity - 1);
            std::memcpy(reply, result.message.data(), length);
            reply[length] = 0;
        }
        if (!result.success) error = result.message;
        return result.handled && result.success ? 1 : 0;
    });
}

int MediconPrepareFrame(void* context) {
    return boundary([&] {
        if (!client || !context || !allocators_set) {
            error = "Initialize the client and set host allocators before preparing a frame.";
            return 0;
        }
        const auto current = std::this_thread::get_id();
        if (frame_thread != std::thread::id{} && current != frame_thread) {
            error = "MediconPrepareFrame must run on the frame's UI thread.";
            return 0;
        }
        frame_thread = current;
        ContextScope scope(static_cast<ImGuiContext*>(context));
        const bool changed = client->prepare_frame();
        if (changed && edge_lit) edge_lit->invalidate_fonts();
        return changed ? 2 : 1;
    });
}

int MediconRenderEdgeLitD3D11(void* context, void* device, void* immediate,
                            void* back_buffer, void* render_target) {
    return boundary([&] {
        if (!client || !context || !device || !immediate || !back_buffer || !render_target ||
            frame_thread != std::this_thread::get_id() || context != edge_context) {
            error = "Render Edge-lit on the frame's UI thread with its ImGui context and valid D3D11 scene targets.";
            return 0;
        }
        ContextScope scope(static_cast<ImGuiContext*>(context));
        if (!edge_lit) edge_lit = std::make_unique<medicon::gui::EdgeLitD3D11>();
        return edge_lit->render(static_cast<ID3D11Device*>(device), static_cast<ID3D11DeviceContext*>(immediate),
            static_cast<ID3D11Texture2D*>(back_buffer), static_cast<ID3D11RenderTargetView*>(render_target), error) ? 1 : 0;
    });
}

int MediconResetKeys() {
    return boundary([] {
        if (!client) { error = "Client is not initialized."; return 0; }
        if (frame_thread != std::thread::id{} && frame_thread != std::this_thread::get_id()) {
            error = "MediconResetKeys must run on the frame's UI thread.";
            return 0;
        }
        client->modules().reset_keys();
        return 1;
    });
}

int MediconShutdown() {
    return boundary([] {
        if (!client) return 1;
        if (frame_thread != std::thread::id{} && frame_thread != std::this_thread::get_id()) {
            error = "MediconShutdown must run on the frame's UI thread.";
            return 0;
        }
        const bool stopped = client->stop();
        if (!stopped) {
            error = client->last_error();
            if (client->running()) return 0;
        }
        edge_lit.reset();
        if (edge_context) medicon::gui::forget_edge_lit_frame(edge_context);
        edge_context = nullptr;
        client.reset();
        frame_thread = {};
        allocators_set = false;
        return stopped ? 1 : 0;
    });
}

uint32_t MediconLastError(char* buffer, uint32_t capacity) {
    std::lock_guard lock(runtime_mutex);
    if (buffer && capacity) {
        const auto count = std::min<std::size_t>(error.size(), capacity - 1);
        std::memcpy(buffer, error.data(), count);
        buffer[count] = '\0';
    }
    return static_cast<uint32_t>(error.size() + 1);
}
