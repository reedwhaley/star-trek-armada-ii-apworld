// Controlled loader for the no-hook Armada II observer. It never selects a
// process by name: callers must name a PID, whose executable is hash-pinned.

#include <windows.h>
#include <bcrypt.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
namespace {
constexpr char kExpectedSha256[] = "c01ff40248bc4c711ea2cde60deda2b9862a8274b18b5537e618fa7b61957ae0";

std::string sha256_file(const std::wstring& path) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{}; DWORD object_size{}, bytes{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0) < 0) return {};
    std::vector<unsigned char> object(object_size), digest(32), buffer(64 * 1024);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) return {};
    std::ifstream input(path, std::ios::binary);
    while (input) { input.read(reinterpret_cast<char*>(buffer.data()), buffer.size()); const auto n = input.gcount(); if (n > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(n), 0) < 0) return {}; }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream text; for (const auto byte : digest) text << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte); return text.str();
}

std::wstring process_path(HANDLE process) {
    std::vector<wchar_t> path(32768); DWORD size = static_cast<DWORD>(path.size());
    return QueryFullProcessImageNameW(process, 0, path.data(), &size) ? std::wstring(path.data(), size) : L"";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--launch") {
        const std::wstring game = argv[2];
        const std::wstring dll = argv[3];
        if (!std::filesystem::is_regular_file(game) || !std::filesystem::is_regular_file(dll)) {
            std::wcerr << L"invalid game or DLL path\n"; return 2;
        }
        if (std::filesystem::path(game).filename().wstring() != L"Armada2.exe" || sha256_file(game) != kExpectedSha256) {
            std::wcerr << L"refused: game is not the pinned Armada2.exe build\n"; return 4;
        }
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION created{};
        std::vector<wchar_t> command(game.begin(), game.end()); command.push_back(L'\0');
        if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                            nullptr, std::filesystem::path(game).parent_path().c_str(), &startup, &created)) {
            std::wcerr << L"cannot start pinned Armada2.exe suspended\n"; return 8;
        }
        const std::wstring pid = std::to_wstring(created.dwProcessId);
        const wchar_t* arguments[] = {argv[0], L"--pid", pid.c_str(), L"--dll", dll.c_str()};
        const int result = wmain(5, const_cast<wchar_t**>(arguments));
        if (result == 0) ResumeThread(created.hThread); else TerminateProcess(created.hProcess, 1);
        CloseHandle(created.hThread); CloseHandle(created.hProcess);
        return result;
    }
    if (argc != 5 || std::wstring(argv[1]) != L"--pid" || std::wstring(argv[3]) != L"--dll") {
        std::wcerr << L"usage: armada2_injector --pid <Armada2 PID> --dll <absolute observer DLL path>\n"
                   << L"   or: armada2_injector --launch <Armada2.exe> <absolute observer DLL path>\n"; return 2;
    }
    const DWORD pid = wcstoul(argv[2], nullptr, 10); const std::wstring dll = argv[4];
    if (!pid || !std::filesystem::is_regular_file(dll)) { std::wcerr << L"invalid PID or DLL path\n"; return 2; }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
    if (!process) { std::wcerr << L"cannot open target PID\n"; return 3; }
    const std::wstring executable = process_path(process);
    if (std::filesystem::path(executable).filename().wstring() != L"Armada2.exe" || sha256_file(executable) != kExpectedSha256) {
        std::wcerr << L"refused: target is not the pinned Armada2.exe build\n"; CloseHandle(process); return 4;
    }
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) { std::wcerr << L"cannot stage DLL path\n"; if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return 5; }
    const auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    const HANDLE thread = CreateRemoteThread(process, nullptr, 0, loader, remote, 0, nullptr);
    if (!thread) { std::wcerr << L"cannot create loader thread\n"; VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return 6; }
    WaitForSingleObject(thread, INFINITE); DWORD module{}; GetExitCodeThread(thread, &module);
    CloseHandle(thread); VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process);
    if (!module) { std::wcerr << L"LoadLibraryW failed in target\n"; return 7; }
    std::wcout << L"observer loaded into pinned Armada2.exe PID " << pid << L"\n"; return 0;
}
