#pragma once
#include <medicon/scripts/ScriptServices.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <thread>
#include <stdexcept>

namespace medicon::scripts::detail {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
};
inline std::wstring quote(std::wstring_view arg) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
        else { result.append(slashes, L'\\'); result += c; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); return result + L'\"';
}
struct ProcessResult { DWORD exit_code = 1; std::string output; bool limited = false; };

inline ProcessResult run(const fs::path& executable, const std::vector<std::wstring>& args,
                         const fs::path& directory, std::atomic_bool& stop, int timeout_seconds = 60,
                         const std::vector<wchar_t>& environment = {}, std::wstring_view raw_tail = {}) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE raw_read{}, raw_write{};
    if (!CreatePipe(&raw_read, &raw_write, &security, 0)) throw std::runtime_error("Cannot create compiler pipe");
    Handle reader(raw_read), writer(raw_write);
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Compiler pipe inheritance failed");
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.value || input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create compiler job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_TIME;
    limits.ProcessMemoryLimit = 1024ull * 1024 * 1024; limits.JobMemoryLimit = 2ull * 1024 * 1024 * 1024;
    limits.BasicLimitInformation.ActiveProcessLimit = 16;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart = 60ll * 10000000;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Cannot apply compiler resource limits");
    SIZE_T bytes{}; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) throw std::runtime_error("Cannot initialize compiler attributes");
    struct DeleteAttributes { PPROC_THREAD_ATTRIBUTE_LIST p; ~DeleteAttributes() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
    HANDLE handles[]{writer.value, input.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr))
        throw std::runtime_error("Cannot restrict compiler handle inheritance");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writer; startup.StartupInfo.hStdError = writer; startup.StartupInfo.hStdInput = input;
    auto command = quote(executable.wstring());
    for (const auto& arg : args) {
        if (!raw_tail.empty() && arg.find_first_of(L" \t\"&|<>\r\n") != std::wstring::npos) throw std::runtime_error("Invalid cmd switch");
        command += L" " + (raw_tail.empty() ? quote(arg) : arg);
    }
    if (!raw_tail.empty()) command += L" " + std::wstring(raw_tail);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
        environment.empty() ? nullptr : const_cast<wchar_t*>(environment.data()), directory.empty() ? nullptr : directory.c_str(),
        &startup.StartupInfo, &process)) throw std::runtime_error("Cannot launch compiler (Win32 " + std::to_string(GetLastError()) + ")");
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job, child)) { TerminateProcess(child, 1); throw std::runtime_error("Cannot isolate compiler in a Job Object"); }
    ResumeThread(thread);
    CloseHandle(writer.value); writer.value = INVALID_HANDLE_VALUE;
    ProcessResult result;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        DWORD available{};
        while (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::array<char, 8192> buffer{}; DWORD count{};
            if (!ReadFile(reader, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr) || !count) break;
            result.output.append(buffer.data(), count);
            if (result.output.size() > 4 * 1024 * 1024) { result.limited = true; break; }
        }
        if (result.limited || stop || std::chrono::steady_clock::now() - start > std::chrono::seconds(timeout_seconds)) {
            result.limited = true; TerminateJobObject(job, 1); WaitForSingleObject(child, 1000); break;
        }
        if (WaitForSingleObject(child, 20) == WAIT_OBJECT_0) {
            if (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) continue;
            GetExitCodeProcess(child, &result.exit_code); break;
        }
    }
    return result;
}
inline fs::path search(std::wstring_view name) {
    std::array<wchar_t, 32768> path{};
    const auto n = SearchPathW(nullptr, std::wstring(name).c_str(), nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
    return n && n < path.size() ? fs::path(path.data()) : fs::path{};
}
inline std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 compiler output");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
} // namespace medicon::scripts::detail
