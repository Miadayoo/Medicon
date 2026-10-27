#include <Windows.h>
#include <imgui.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<typename F> F load(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    require(address != nullptr, name);
    return reinterpret_cast<F>(address);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto path = std::filesystem::temp_directory_path() /
        ("medicon-dll-" + std::to_string(GetCurrentProcessId()) + ".json");
    HMODULE module = LoadLibraryW(std::filesystem::path(argv[1]).c_str());
    try {
        require(module != nullptr, "LoadLibraryW Medicon.dll");
        const auto version = load<uint32_t(*)()>(module, "MediconApiVersion");
        const auto imgui_version = load<uint32_t(*)()>(module, "MediconImGuiVersion");
        using Alloc = void*(*)(size_t, void*);
        using Free = void(*)(void*, void*);
        const auto allocators = load<int(*)(Alloc, Free, void*)>(module, "MediconSetImGuiAllocators");
        const auto initialize = load<int(*)(const wchar_t*)>(module, "MediconInitialize");
        const auto frame = load<int(*)(void*, float)>(module, "MediconFrame");
        const auto prepare = load<int(*)(void*)>(module, "MediconPrepareFrame");
        const auto edge_lit = load<int(*)(void*, void*, void*, void*, void*)>(module, "MediconRenderEdgeLitD3D11");
        const auto key = load<int(*)(int, int, int)>(module, "MediconKeyEvent");
        const auto command = load<int(*)(const char*, char*, uint32_t)>(module, "MediconCommand");
        const auto reset_keys = load<int(*)()>(module, "MediconResetKeys");
        const auto shutdown = load<int(*)()>(module, "MediconShutdown");
        const auto error = load<uint32_t(*)(char*, uint32_t)>(module, "MediconLastError");
        require(version() == 1, "Stable API version");
        require(imgui_version() == IMGUI_VERSION_NUM, "Host ImGui version matches DLL");
        require(shutdown() == 1, "Shutdown before initialization");
        require(command(".bind clickgui rshift", nullptr, 0) == 0, "Commands require DLL client initialization");
        require(frame(nullptr, 0.016F) == 0, "Reject missing context/client");
        require(edge_lit(nullptr, nullptr, nullptr, nullptr, nullptr) == 0, "Edge-lit rejects absent host and targets");
        char diagnostic[256]{};
        require(error(diagnostic, sizeof diagnostic) > 1 && diagnostic[0], "Copy boundary diagnostic");
        ImGui::CreateContext();
        struct Context { ~Context() { ImGui::DestroyContext(); } } context;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0F / 60.0F;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ImGuiMemAllocFunc allocate;
        ImGuiMemFreeFunc release;
        void* user_data;
        ImGui::GetAllocatorFunctions(&allocate, &release, &user_data);
        for (int i = 0; i < 2; ++i) {
            require(initialize(path.c_str()) == 1, "Initialize after LoadLibrary");
            require(initialize(path.c_str()) == 1, "Idempotent initialize");
            require(key('A', 1, 0) == 0 && key('A', 0, 0) == 0,
                    "Initialization leaves GUI closed, including a saved open GUI on restart");
            char reply[256]{};
            require(command(nullptr, reply, sizeof reply) == 0, "Command API rejects null input");
            require(command(".bind clickgui rshift", reply, sizeof reply) == 1 &&
                    std::string(reply).find("RShift") != std::string::npos,
                    "Command API remaps the shared GUI keybind");
            std::array<char, 8> bounded;
            bounded.fill('!');
            require(command(".bind clickgui", bounded.data() + 1, 5) == 1,
                    "Command query supports a short reply buffer");
            require(bounded[0] == '!' && bounded[5] == '\0' && bounded[6] == '!' && bounded[7] == '!',
                    "Short command reply is terminated without writing outside capacity");
            char tiny[]{'X', 'Y'};
            require(command(".bind clickgui", tiny, 1) == 1 && tiny[0] == '\0' && tiny[1] == 'Y',
                    "Single-byte command reply contains only the terminator");
            tiny[0] = 'X';
            require(command(".bind clickgui", tiny, 0) == 1 && tiny[0] == 'X',
                    "Zero-capacity command reply does not write memory");
            require(command(".bind clickgui", nullptr, 0) == 1, "Command API permits omitted replies");
            require(command(".bind clickgui F25", reply, sizeof reply) == 0 && reply[0],
                    "Invalid command returns failure and a diagnostic");
            require(command(".bind clickgui", reply, sizeof reply) == 1 &&
                    std::string(reply).find("RShift") != std::string::npos,
                    "Rejected command preserves the shared keybind");
            require(command(".not_a_command", reply, sizeof reply) == 0 && reply[0],
                    "Unknown local command returns failure with a reply");
            require(key(VK_INSERT, 1, 0) == 0 && key(VK_INSERT, 0, 0) == 0,
                    "Previous GUI key cannot open the remapped GUI after initialization");
            require(key(VK_RSHIFT, 1, 0) == 1 && key(VK_RSHIFT, 0, 0) == 1,
                    "Command-remapped key opens the GUI during the splash");
            require(key(VK_RSHIFT, 1, 1) == 1,
                    "Repeated command-remapped key keeps the GUI open");
            require(key(VK_RSHIFT, 1, 0) == 0 && key(VK_RSHIFT, 0, 0) == 0,
                    "Command-remapped key closes the GUI");
            require(key(VK_RSHIFT, 1, 0) == 1 && key(VK_RSHIFT, 0, 0) == 1,
                    "Command-remapped key reopens the GUI");
            require(command(".bind clickgui insert", reply, sizeof reply) == 1,
                    "Restore Insert for existing lifecycle checks");
            require(key(VK_INSERT, 1, 0) == 0, "GUI closes");
            require(key(VK_INSERT, 1, 1) == 0, "Ignore repeated Insert");
            require(reset_keys() == 1, "Reset keys on focus loss");
            require(frame(nullptr, 0.016F) == 0, "Reject null frame context");
            require(allocators(allocate, release, user_data) == 1, "Share host ImGui allocator functions");
            require(key(VK_INSERT, 1, 0) == 1, "Open GUI before frame");
            for (int f = 0; f < 3; ++f) {
                // Host order: platform NewFrame -> MediconPrepareFrame -> (re-upload fonts) -> ImGui::NewFrame.
                const int prepared = prepare(ImGui::GetCurrentContext());
                require(prepared == 1 || prepared == 2, "Prepare frame adds GUI fonts to the host atlas");
                if (prepared == 2) {
                    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
                    require(pixels && io.Fonts->Fonts.Size > 1, "Host atlas rebuilt with GUI fonts, host font kept");
                }
                ImGui::NewFrame();
                require(frame(ImGui::GetCurrentContext(), io.DeltaTime) == 1, "DLL draws into host ImGui frame");
                ImGui::Render();
                require(ImGui::GetDrawData() != nullptr, "Host receives GUI draw data");
            }
            require(shutdown() == 1, "Shutdown before unload");
        }
        require(FreeLibrary(module) != 0, "Unload DLL");
        module = nullptr;
        std::filesystem::remove(path);
        std::cout << "DLL export lifecycle tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        if (module) FreeLibrary(module);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
