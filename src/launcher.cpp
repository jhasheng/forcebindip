#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::wstring ip;
    std::wstring iface;
    std::wstring config_path;
    std::wstring dll_path;
    std::wstring exe_path;
    std::wstring cwd;
    std::vector<std::wstring> args;
    bool list_ifaces = false;
};

struct InterfaceAddress {
    std::wstring name;
    std::wstring description;
    std::wstring friendly_name;
    std::wstring adapter_name;
    std::wstring ip;
};

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring Trim(const std::wstring& value)
{
    const wchar_t* whitespace = L" \t\r\n";
    const size_t first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::wstring StripMatchingQuotes(const std::wstring& value)
{
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::wstring Utf8ToWide(const char* value)
{
    if (! value) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, out.data(), needed);
    return out;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}

std::vector<std::wstring> SplitCommandLine(const std::wstring& value)
{
    std::vector<std::wstring> out;
    int argc = 0;
    wchar_t* mutable_args = _wcsdup(value.c_str());
    if (! mutable_args) {
        return out;
    }
    LPWSTR* argv = CommandLineToArgvW(mutable_args, &argc);
    free(mutable_args);
    if (! argv) {
        return out;
    }
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i]);
    }
    LocalFree(argv);
    return out;
}

std::wstring QuoteArg(const std::wstring& arg)
{
    if (arg.empty()) {
        return L"\"\"";
    }

    const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (! needs_quotes) {
        return arg;
    }

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        }
        else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        }
        else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const Options& options)
{
    std::wstring command = QuoteArg(options.exe_path);
    for (const auto& arg : options.args) {
        command.push_back(L' ');
        command.append(QuoteArg(arg));
    }
    return command;
}

std::filesystem::path CurrentExeDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = 0;
    for (;;) {
        copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

void PrintUsage()
{
    std::wcerr
        << L"usage:\n"
        << L"  forcebindip_cpp.exe [--config <file>] [--ip <ipv4>|--iface <name|description|guid>] [--dll <bindip_hook.dll>] [--cwd <dir>] -- <exe> [args...]\n"
        << L"  forcebindip_cpp.exe --list-ifaces\n";
}

std::optional<std::wstring> FindConfigArg(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--") {
            break;
        }
        if (arg == L"--config" && i + 1 < argc) {
            return std::wstring(argv[i + 1]);
        }
    }
    return std::nullopt;
}

bool ApplyConfigValue(Options& options, const std::wstring& key, const std::wstring& value)
{
    const std::wstring normalized_key = ToLower(Trim(key));
    const std::wstring normalized_value = StripMatchingQuotes(Trim(value));

    if (normalized_key == L"ip") {
        options.ip = normalized_value;
        options.iface.clear();
        return true;
    }
    if (normalized_key == L"iface" || normalized_key == L"interface") {
        options.iface = normalized_value;
        options.ip.clear();
        return true;
    }
    if (normalized_key == L"dll") {
        options.dll_path = normalized_value;
        return true;
    }
    if (normalized_key == L"exe" || normalized_key == L"target") {
        options.exe_path = normalized_value;
        return true;
    }
    if (normalized_key == L"cwd" || normalized_key == L"workdir") {
        options.cwd = normalized_value;
        return true;
    }
    if (normalized_key == L"args") {
        options.args = SplitCommandLine(normalized_value);
        return true;
    }
    if (normalized_key == L"arg") {
        options.args.push_back(normalized_value);
        return true;
    }
    return false;
}

bool LoadConfigFile(const std::wstring& path, Options& options, bool required)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (! file) {
        if (required) {
            std::wcerr << L"config file not found: " << path << L"\n";
        }
        return ! required;
    }

    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    const std::wstring content = Utf8ToWide(bytes);
    std::wistringstream stream(content);
    std::wstring line;
    size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        line = Trim(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') {
            continue;
        }
        if (line.front() == L'[' && line.back() == L']') {
            continue;
        }

        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            std::wcerr << L"invalid config line " << line_number << L": " << line << L"\n";
            return false;
        }

        if (! ApplyConfigValue(options, line.substr(0, equals), line.substr(equals + 1))) {
            std::wcerr << L"unknown config key on line " << line_number << L": " << line.substr(0, equals) << L"\n";
            return false;
        }
    }

    options.config_path = path;
    return true;
}

std::vector<InterfaceAddress> EnumerateInterfaces()
{
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG buffer_size = 15 * 1024;
    std::vector<unsigned char> buffer(buffer_size);

    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buffer_size);
    }
    if (result != NO_ERROR) {
        throw std::runtime_error("GetAdaptersAddresses failed");
    }

    std::vector<InterfaceAddress> out;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (! unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            wchar_t ip[INET_ADDRSTRLEN] {};
            const void* ip_src = &addr->sin_addr;
            if (! InetNtopW(AF_INET, ip_src, ip, static_cast<size_t>(std::size(ip)))) {
                continue;
            }

            InterfaceAddress item;
            item.name = adapter->FriendlyName ? adapter->FriendlyName : L"";
            item.friendly_name = item.name;
            item.description = adapter->Description ? adapter->Description : L"";
            item.adapter_name = Utf8ToWide(adapter->AdapterName);
            item.ip = ip;
            out.push_back(item);
        }
    }
    return out;
}

void PrintInterfaces()
{
    const auto interfaces = EnumerateInterfaces();
    for (const auto& iface : interfaces) {
        std::wcout << iface.ip << L"\n"
                   << L"  name: " << iface.name << L"\n"
                   << L"  description: " << iface.description << L"\n"
                   << L"  guid: " << iface.adapter_name << L"\n";
    }
}

bool MatchesInterface(const InterfaceAddress& iface, const std::wstring& query)
{
    const std::wstring q = ToLower(query);
    return ToLower(iface.name) == q
        || ToLower(iface.description) == q
        || ToLower(iface.adapter_name) == q
        || ToLower(iface.name).find(q) != std::wstring::npos
        || ToLower(iface.description).find(q) != std::wstring::npos
        || ToLower(iface.adapter_name).find(q) != std::wstring::npos;
}

std::optional<std::wstring> ResolveInterfaceIp(const std::wstring& query)
{
    const auto interfaces = EnumerateInterfaces();
    std::vector<InterfaceAddress> matches;
    for (const auto& iface : interfaces) {
        if (MatchesInterface(iface, query)) {
            matches.push_back(iface);
        }
    }

    if (matches.empty()) {
        std::wcerr << L"no interface matched: " << query << L"\n";
        return std::nullopt;
    }
    if (matches.size() > 1) {
        std::wcerr << L"interface name is ambiguous: " << query << L"\n";
        for (const auto& match : matches) {
            std::wcerr << L"  " << match.ip << L"  " << match.name << L"  " << match.description << L"  " << match.adapter_name << L"\n";
        }
        return std::nullopt;
    }
    return matches.front().ip;
}

bool ParseArgs(int argc, wchar_t** argv, Options& options)
{
    int i = 1;
    for (; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--") {
            ++i;
            break;
        }
        if (arg == L"--ip" && i + 1 < argc) {
            options.ip = argv[++i];
            options.iface.clear();
            continue;
        }
        if (arg == L"--iface" && i + 1 < argc) {
            options.iface = argv[++i];
            options.ip.clear();
            continue;
        }
        if (arg == L"--config" && i + 1 < argc) {
            options.config_path = argv[++i];
            continue;
        }
        if (arg == L"--dll" && i + 1 < argc) {
            options.dll_path = argv[++i];
            continue;
        }
        if (arg == L"--cwd" && i + 1 < argc) {
            options.cwd = argv[++i];
            continue;
        }
        if (arg == L"--list-ifaces") {
            options.list_ifaces = true;
            continue;
        }
        if (arg == L"--help" || arg == L"-h") {
            return false;
        }

        if (options.exe_path.empty()) {
            options.exe_path = arg;
        }
        else {
            options.args.push_back(arg);
        }
    }

    if (i < argc) {
        options.exe_path = argv[i++];
        for (; i < argc; ++i) {
            options.args.emplace_back(argv[i]);
        }
    }

    if (options.dll_path.empty()) {
        options.dll_path = (CurrentExeDirectory() / L"bindip_hook.dll").wstring();
    }

    if (options.list_ifaces) {
        return true;
    }

    if (! options.iface.empty()) {
        if (! options.ip.empty()) {
            std::wcerr << L"use only one of --ip or --iface\n";
            return false;
        }
        auto resolved_ip = ResolveInterfaceIp(options.iface);
        if (! resolved_ip) {
            return false;
        }
        options.ip = *resolved_ip;
    }

    return ! options.ip.empty() && ! options.exe_path.empty();
}

std::wstring GetLastErrorMessage(DWORD error)
{
    wchar_t* message = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstring result = L"error " + std::to_wstring(error) + L": ";
    result += size ? std::wstring(message, size) : L"unknown error";
    if (message) {
        LocalFree(message);
    }
    return result;
}

bool IsValidIpv4(const std::wstring& value)
{
    in_addr ignored {};
    return InetPtonW(AF_INET, value.c_str(), &ignored) == 1;
}

bool GetProcessMachine(HANDLE process, USHORT& machine)
{
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto* fn = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (fn) {
        USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (! fn(process, &process_machine, &native_machine)) {
            return false;
        }
        machine = (process_machine == IMAGE_FILE_MACHINE_UNKNOWN) ? native_machine : process_machine;
        return true;
    }

    BOOL is_wow64 = FALSE;
    if (! IsWow64Process(process, &is_wow64)) {
        return false;
    }
#if defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
#elif defined(_M_X64)
    machine = is_wow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
#else
    machine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
    return true;
}

bool CheckSameBitness(HANDLE target_process)
{
    USHORT current_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT target_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (! GetProcessMachine(GetCurrentProcess(), current_machine) || ! GetProcessMachine(target_process, target_machine)) {
        std::wcerr << L"warning: unable to verify process bitness; continuing\n";
        return true;
    }
    return current_machine == target_machine;
}

bool InjectDll(HANDLE process, const std::wstring& dll_path)
{
    const size_t bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (! remote_path) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
        return false;
    }

    SIZE_T written = 0;
    if (! WriteProcessMemory(process, remote_path, dll_path.c_str(), bytes, &written) || written != bytes) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    auto* load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (! load_library) {
        std::wcerr << L"GetProcAddress(LoadLibraryW) failed\n";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote_path, 0, nullptr);
    if (! thread) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(thread, 10000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);

    if (wait_result != WAIT_OBJECT_0 || exit_code == 0) {
        std::wcerr << L"remote LoadLibraryW failed or timed out\n";
        return false;
    }

    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options options;
    const auto explicit_config = FindConfigArg(argc, argv);
    if (explicit_config) {
        if (! LoadConfigFile(*explicit_config, options, true)) {
            return 2;
        }
    }
    else {
        const std::wstring default_config = (CurrentExeDirectory() / L"forcebindip_cpp.ini").wstring();
        if (GetFileAttributesW(default_config.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (! LoadConfigFile(default_config, options, false)) {
                return 2;
            }
        }
    }

    if (! ParseArgs(argc, argv, options)) {
        PrintUsage();
        return 2;
    }

    if (options.list_ifaces) {
        PrintInterfaces();
        return 0;
    }

    if (GetFileAttributesW(options.dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"DLL not found: " << options.dll_path << L"\n";
        return 2;
    }
    if (! IsValidIpv4(options.ip)) {
        std::wcerr << L"invalid bind IPv4 address: " << options.ip << L"\n";
        return 2;
    }
    if (GetFileAttributesW(options.exe_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"target exe not found: " << options.exe_path << L"\n";
        return 2;
    }
    if (! options.cwd.empty()) {
        const DWORD cwd_attrs = GetFileAttributesW(options.cwd.c_str());
        if (cwd_attrs == INVALID_FILE_ATTRIBUTES || (cwd_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wcerr << L"working directory not found: " << options.cwd << L"\n";
            return 2;
        }
    }

    SetEnvironmentVariableW(L"WAR3_FORCE_BIND_IP", options.ip.c_str());

    std::wstring command_line = BuildCommandLine(options);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};

    BOOL ok = CreateProcessW(
        options.exe_path.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        options.cwd.empty() ? nullptr : options.cwd.c_str(),
        &startup,
        &process);

    SetEnvironmentVariableW(L"WAR3_FORCE_BIND_IP", nullptr);

    if (! ok) {
        std::wcerr << L"CreateProcessW failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
        return 1;
    }

    if (! CheckSameBitness(process.hProcess)) {
        std::wcerr << L"target process bitness does not match launcher/DLL bitness\n";
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }

    if (! InjectDll(process.hProcess, options.dll_path)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }

    ResumeThread(process.hThread);
    std::wcout << L"started pid " << process.dwProcessId << L" with bind IP " << options.ip << L"\n";

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
