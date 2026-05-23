#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using bind_fn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using connect_fn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using sendto_fn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using wsaconnect_fn = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
using wsasendto_fn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using getsockname_fn = int(WSAAPI*)(SOCKET, sockaddr*, int*);

HMODULE g_self = nullptr;
in_addr g_bind_addr {};
bool g_enabled = false;

bind_fn g_real_bind = nullptr;
connect_fn g_real_connect = nullptr;
sendto_fn g_real_sendto = nullptr;
wsaconnect_fn g_real_wsaconnect = nullptr;
wsasendto_fn g_real_wsasendto = nullptr;
getsockname_fn g_real_getsockname = nullptr;

void Log(const char* fmt, ...)
{
    char buffer[1024] {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    char line[1200] {};
    std::snprintf(line, sizeof(line), "[bindip_hook] %s\n", buffer);
    OutputDebugStringA(line);
}

bool IsLoopback(in_addr addr)
{
    return (ntohl(addr.s_addr) >> 24) == 127;
}

void BindSocketToForcedIp(SOCKET socket, unsigned short port)
{
    sockaddr_in forced {};
    forced.sin_family = AF_INET;
    forced.sin_addr = g_bind_addr;
    forced.sin_port = port;
    if (g_real_bind(socket, reinterpret_cast<const sockaddr*>(&forced), sizeof(forced)) == 0) {
        char ip[INET_ADDRSTRLEN] {};
        InetNtopA(AF_INET, &g_bind_addr, ip, sizeof(ip));
        Log("bound socket to %s:%u", ip, ntohs(port));
    }
}

bool ShouldRewriteBindAddress(const sockaddr* name, int namelen)
{
    if (! g_enabled || ! name || namelen < static_cast<int>(sizeof(sockaddr_in))) {
        return false;
    }
    if (name->sa_family != AF_INET) {
        return false;
    }

    const auto* in = reinterpret_cast<const sockaddr_in*>(name);
    if (in->sin_addr.s_addr != INADDR_ANY) {
        return false;
    }
    return true;
}

void EnsureSocketBound(SOCKET socket, const sockaddr* remote, int remote_len)
{
    if (! g_enabled || ! g_real_getsockname || ! g_real_bind || socket == INVALID_SOCKET) {
        return;
    }
    if (! remote || remote_len < static_cast<int>(sizeof(sockaddr_in)) || remote->sa_family != AF_INET) {
        return;
    }

    const auto* remote_in = reinterpret_cast<const sockaddr_in*>(remote);
    if (IsLoopback(remote_in->sin_addr)) {
        return;
    }

    sockaddr_in local {};
    int local_len = sizeof(local);
    if (g_real_getsockname(socket, reinterpret_cast<sockaddr*>(&local), &local_len) == SOCKET_ERROR) {
        BindSocketToForcedIp(socket, 0);
        return;
    }
    if (local.sin_family != AF_INET) {
        return;
    }
    if (local.sin_addr.s_addr != INADDR_ANY) {
        return;
    }

    if (local.sin_port != 0) {
        Log("socket is already bound to 0.0.0.0:%u; earlier bind hook did not catch it", ntohs(local.sin_port));
        return;
    }

    BindSocketToForcedIp(socket, 0);
}

int WSAAPI HookedBind(SOCKET socket, const sockaddr* name, int namelen)
{
    if (ShouldRewriteBindAddress(name, namelen)) {
        sockaddr_in copy = *reinterpret_cast<const sockaddr_in*>(name);
        copy.sin_addr = g_bind_addr;

        char ip[INET_ADDRSTRLEN] {};
        InetNtopA(AF_INET, &g_bind_addr, ip, sizeof(ip));
        Log("bind rewrite to %s:%u", ip, ntohs(copy.sin_port));
        return g_real_bind(socket, reinterpret_cast<const sockaddr*>(&copy), sizeof(copy));
    }
    return g_real_bind(socket, name, namelen);
}

int WSAAPI HookedConnect(SOCKET socket, const sockaddr* name, int namelen)
{
    EnsureSocketBound(socket, name, namelen);
    return g_real_connect(socket, name, namelen);
}

int WSAAPI HookedSendTo(SOCKET socket, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
    EnsureSocketBound(socket, to, tolen);
    return g_real_sendto(socket, buf, len, flags, to, tolen);
}

int WSAAPI HookedWSAConnect(SOCKET socket, const sockaddr* name, int namelen, LPWSABUF caller_data, LPWSABUF callee_data, LPQOS sqos, LPQOS gqos)
{
    EnsureSocketBound(socket, name, namelen);
    return g_real_wsaconnect(socket, name, namelen, caller_data, callee_data, sqos, gqos);
}

int WSAAPI HookedWSASendTo(SOCKET socket, LPWSABUF buffers, DWORD buffer_count, LPDWORD bytes_sent, DWORD flags, const sockaddr* to, int tolen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    EnsureSocketBound(socket, to, tolen);
    return g_real_wsasendto(socket, buffers, buffer_count, bytes_sent, flags, to, tolen, overlapped, completion);
}

struct PatchTarget {
    void* original;
    void* replacement;
    const char* name;
};

bool PatchSlot(void** slot, void* original, void* replacement)
{
    if (*slot != original) {
        return false;
    }

    DWORD old_protect = 0;
    if (! VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        return false;
    }
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void PatchModuleUnsafe(HMODULE module, const PatchTarget* targets, size_t target_count)
{
    if (! module || module == g_self) {
        return;
    }

    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    const auto& imports_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports_dir.VirtualAddress == 0 || imports_dir.Size == 0) {
        return;
    }

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports_dir.VirtualAddress);
    for (; imports->Name; ++imports) {
        const char* dll_name = reinterpret_cast<const char*>(base + imports->Name);
        if (_stricmp(dll_name, "ws2_32.dll") != 0) {
            continue;
        }

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            for (size_t i = 0; i < target_count; ++i) {
                if (PatchSlot(slot, targets[i].original, targets[i].replacement)) {
                    Log("patched %s in module %p", targets[i].name, module);
                    break;
                }
            }
        }
    }
}

void PatchModule(HMODULE module, const PatchTarget* targets, size_t target_count)
{
    __try {
        PatchModuleUnsafe(module, targets, target_count);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("skipped module %p after exception while patching imports", module);
    }
}

void PatchAllModules()
{
    PatchTarget targets[] = {
        { reinterpret_cast<void*>(g_real_bind), reinterpret_cast<void*>(&HookedBind), "bind" },
        { reinterpret_cast<void*>(g_real_connect), reinterpret_cast<void*>(&HookedConnect), "connect" },
        { reinterpret_cast<void*>(g_real_sendto), reinterpret_cast<void*>(&HookedSendTo), "sendto" },
        { reinterpret_cast<void*>(g_real_wsaconnect), reinterpret_cast<void*>(&HookedWSAConnect), "WSAConnect" },
        { reinterpret_cast<void*>(g_real_wsasendto), reinterpret_cast<void*>(&HookedWSASendTo), "WSASendTo" },
    };

    HMODULE modules[1024] {};
    DWORD needed = 0;
    if (! EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return;
    }

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        PatchModule(modules[i], targets, sizeof(targets) / sizeof(targets[0]));
    }
}

bool InitializeConfig()
{
    char ip[64] {};
    const DWORD copied = GetEnvironmentVariableA("WAR3_FORCE_BIND_IP", ip, sizeof(ip));
    if (copied == 0 || copied >= sizeof(ip)) {
        Log("WAR3_FORCE_BIND_IP is missing");
        return false;
    }
    if (InetPtonA(AF_INET, ip, &g_bind_addr) != 1) {
        Log("invalid IPv4 address: %s", ip);
        return false;
    }

    char rendered[INET_ADDRSTRLEN] {};
    InetNtopA(AF_INET, &g_bind_addr, rendered, sizeof(rendered));
    Log("configured bind IP: %s", rendered);
    return true;
}

bool InitializeWinsockPointers()
{
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (! ws2) {
        ws2 = LoadLibraryA("ws2_32.dll");
    }
    if (! ws2) {
        Log("unable to load ws2_32.dll");
        return false;
    }

    g_real_bind = reinterpret_cast<bind_fn>(GetProcAddress(ws2, "bind"));
    g_real_connect = reinterpret_cast<connect_fn>(GetProcAddress(ws2, "connect"));
    g_real_sendto = reinterpret_cast<sendto_fn>(GetProcAddress(ws2, "sendto"));
    g_real_wsaconnect = reinterpret_cast<wsaconnect_fn>(GetProcAddress(ws2, "WSAConnect"));
    g_real_wsasendto = reinterpret_cast<wsasendto_fn>(GetProcAddress(ws2, "WSASendTo"));
    g_real_getsockname = reinterpret_cast<getsockname_fn>(GetProcAddress(ws2, "getsockname"));

    return g_real_bind && g_real_connect && g_real_sendto && g_real_wsaconnect && g_real_wsasendto && g_real_getsockname;
}

DWORD WINAPI WorkerThread(void*)
{
    if (! InitializeConfig() || ! InitializeWinsockPointers()) {
        return 1;
    }

    g_enabled = true;
    for (int i = 0; i < 120; ++i) {
        PatchAllModules();
        Sleep(500);
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
