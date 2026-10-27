#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace medicon::gui {

// A native "Open File" dialog (the Win32 common dialog, which shows the standard
// Explorer file browser) run on a worker thread so the render thread never
// blocks while it is on screen. The render thread owns the object: it calls
// open() once, then poll() each frame until the result is taken. Opening a
// second dialog while one is already up is ignored.
class NativeFileDialog {
public:
    NativeFileDialog() = default;
    NativeFileDialog(const NativeFileDialog&) = delete;
    NativeFileDialog& operator=(const NativeFileDialog&) = delete;
    ~NativeFileDialog();

    // Starts the dialog unless one is already open. `owner_window` is the HWND
    // the dialog is modal to (ImGui's main viewport handle); it may be null.
    // `title` names the window. `filter` is the Win32 double-null filter list,
    // e.g. L"Core DLL\0*.dll\0All files\0*.*\0"; empty shows every file.
    void open(void* owner_window, std::wstring title, std::wstring filter);
    // True while the dialog is on screen (a worker is running).
    bool busy() const noexcept { return running_.load(std::memory_order_acquire); }
    // Once the dialog has closed, moves its result into `path` and returns true
    // exactly once; an empty `path` means it was cancelled. False while no fresh
    // result is waiting.
    bool poll(std::wstring& path);

private:
    void join() noexcept;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> done_{false};
    std::wstring result_;  // published by the worker before done_ turns true
};

} // namespace medicon::gui
