#include <medicon/gui/NativeFileDialog.hpp>

#include <Windows.h>
#include <objbase.h>
#include <commdlg.h>

#include <array>
#include <utility>

namespace medicon::gui {

NativeFileDialog::~NativeFileDialog() { join(); }

void NativeFileDialog::join() noexcept {
    if (worker_.joinable()) worker_.join();
}

void NativeFileDialog::open(void* owner_window, std::wstring title, std::wstring filter) {
    // One dialog at a time; a second open() while one is on screen is ignored.
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    // A finished-but-unpolled worker is joined before the next one starts.
    join();
    done_.store(false, std::memory_order_release);
    result_.clear();
    worker_ = std::thread([this, owner = static_cast<HWND>(owner_window), title = std::move(title),
                           filter = std::move(filter)]() mutable {
        // COM lets the common dialog show the modern Explorer browser; it still
        // opens without it, so a failure here is not fatal.
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        std::array<wchar_t, MAX_PATH> buffer{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter.empty() ? nullptr : filter.c_str();
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
        // No hook or custom template, so Vista and later show the Explorer dialog.
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        std::wstring picked;
        if (GetOpenFileNameW(&ofn)) picked.assign(buffer.data());
        result_ = std::move(picked);  // written before done_ is published
        done_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        if (com) CoUninitialize();
    });
}

bool NativeFileDialog::poll(std::wstring& path) {
    if (!done_.load(std::memory_order_acquire)) return false;
    join();  // the worker has published its result and is finishing
    done_.store(false, std::memory_order_release);
    path = std::move(result_);
    result_.clear();
    return true;
}

} // namespace medicon::gui
