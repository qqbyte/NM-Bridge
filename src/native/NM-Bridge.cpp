// NM-Bridge.cpp

#include "NM-Bridge.h"

#include <thread>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <bcrypt.h>
#include <wintrust.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wintrust.lib")

#include "include/json.hpp"
using json = nlohmann::json;

namespace
{
    std::string utf16_to_utf8(const std::wstring& ws)
    {
        if (ws.empty()) return {};
        int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], sz, nullptr, nullptr);
        if (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    std::wstring utf8_to_utf16(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring ws(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], sz);
        if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
        return ws;
    }


    std::string base64_encode(const std::vector<BYTE>& data)
    {
        if (data.empty()) return "";
        DWORD outLen = 0;
        CryptBinaryToStringA((const BYTE*)data.data(), (DWORD)data.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outLen);
        std::string out(outLen + 1, '\0');
        if (CryptBinaryToStringA((const BYTE*)data.data(), (DWORD)data.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &outLen)) {
            out.resize(outLen);
            return out;
        }
        return "";
    }
}


// ---------------- Constructor / Destructor ----------------

NM_Bridge::NM_Bridge() {}

NM_Bridge::~NM_Bridge()
{
    Shutdown();
}

// ---------------- Initialization ---------------- 

bool NM_Bridge::Init(const std::wstring& ManagedDllPath, std::wstring& error)
{
    if (ClrRuntimeHost) return true;

    HRESULT hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(&MetaHost));
    if (FAILED(hr))
    {
        error = L"CLRCreateInstance failed";
        return false;
    }

    hr = MetaHost->GetRuntime(L"v4.0.30319", IID_PPV_ARGS(&RuntimeInfo));
    if (FAILED(hr))
    {
        error = L"GetRuntime failed";
        return false;
    }

    BOOL loadable = FALSE;
    hr = RuntimeInfo->IsLoadable(&loadable);
    if (FAILED(hr) || !loadable)
    {
        error = L"Runtime not loadable";
        return false;
    }

    hr = RuntimeInfo->GetInterface(CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (void**)&ClrRuntimeHost);
    if (FAILED(hr))
    {
        error = L"GetInterface(ICLRRuntimeHost) failed";
        return false;
    }

    hr = ClrRuntimeHost->Start();
    if (FAILED(hr))
    {
        error = L"CLR Start failed";
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    pipename = "managedbridge_server_" + std::to_string(pid) + "_" + std::to_string(rand() % 10000);
    authToken = std::to_string(GetTickCount64()) + "_" + std::to_string(rand());

    json initReq = {
        {"cmd", "_start_server"},
        {"pipeName", pipename},
        {"authToken", authToken}
    };

    std::string dummy;
    return StartManagedServer(ManagedDllPath, initReq.dump(), dummy, error, 15000);
}

void NM_Bridge::Shutdown()
{
    if (!pipename.empty())
    {
        std::string dummy;
        std::wstring err;
        json rq = { {"cmd", "stopServer"} };
        SendCommand(rq.dump(), dummy, err, 2000);
        pipename.clear();
    }

    if (ClrRuntimeHost)
    {
        ClrRuntimeHost->Stop();
        ClrRuntimeHost->Release();
        ClrRuntimeHost = nullptr;
    }

    if (RuntimeInfo)
    {
        RuntimeInfo->Release();
        RuntimeInfo = nullptr;
    }

    if (MetaHost)
    {
        MetaHost->Release();
        MetaHost = nullptr;
    }
}

bool NM_Bridge::StartManagedServer(const std::wstring& ManagedDllPath, const std::string& requestJson, std::string& output, std::wstring& error, int timeoutMs)
{
    if (!ClrRuntimeHost)
    {
        error = L"CLR not initialized";
        return false;
    }

    std::wstring wideReq = utf8_to_utf16(requestJson);

    DWORD ret = 0;
    HRESULT hr = ClrRuntimeHost->ExecuteInDefaultAppDomain(ManagedDllPath.c_str(), L"MANAGED_Bridge.Managed_Bridge", L"StartServer", wideReq.c_str(), &ret);

    if (FAILED(hr))
    {
        error = L"StartServer failed";
        return false;
    }

    int tries = timeoutMs / 200;
    bool ok = false;

    for (int i = 0; i < tries; ++i)
    {
        std::string pipePath = "\\\\.\\pipe\\" + pipename;

        if (WaitNamedPipeA(pipePath.c_str(), 100) || GetLastError() == ERROR_PIPE_BUSY)
        {
            ok = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (!ok)
    {
        error = L"Managed server did not start or pipe not available";
        return false;
    }

    output = "{\"success\":true}";
    return true;
}

// ---------------- Domain ----------------

bool NM_Bridge::CreateDomain(const std::string& domainId, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "createDomain";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

bool NM_Bridge::UnloadDomain(const std::string& domainId, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "unloadDomain";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

// ---------------- Load ----------------

bool NM_Bridge::LoadFromFile(const std::string& domainId, const std::wstring& assemblyPath, const std::string& assemblyAlias, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "loadFromFile";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["path"] = utf16_to_utf8(assemblyPath);
    if (!assemblyAlias.empty()) rq["assemblyAlias"] = assemblyAlias;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

bool NM_Bridge::LoadFromMemory(const std::string& domainId, const std::vector<BYTE>& bytes, const std::string& simpleName, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "loadFromMemory";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["bytesBase64"] = base64_encode(bytes);
    if (!simpleName.empty()) rq["assemblySimpleName"] = simpleName;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

// ---------------- Invoke ----------------

bool NM_Bridge::CreateInstance(const std::string& domainId, const std::string& assemblyAlias, const std::string& typeName, const std::string& constructorArgsJson, std::string& resultJson, std::wstring& err, int timeoutMs)
{
    json rq;
    rq["cmd"] = "createInstance";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["assemblyName"] = assemblyAlias;
    rq["typeName"] = typeName;
    rq["ctorArgsJson"] = FormatArgs(constructorArgsJson);
    return SendCommand(rq.dump(), resultJson, err, timeoutMs);
}

bool NM_Bridge::ReleaseInstance(const std::string& domainId, const std::string& instanceId, std::string& resultJson, std::wstring& err, int timeoutMs)
{
    json rq;
    rq["cmd"] = "releaseInstance";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["instanceId"] = instanceId;
    return SendCommand(rq.dump(), resultJson, err, timeoutMs);
}


bool NM_Bridge::InvokeStatic(const std::string& domainId, const std::string& assemblyAlias, const std::string& typeName, const std::string& methodName, const std::string& argsJson, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "invokeStatic";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["assemblyName"] = assemblyAlias;
    rq["typeName"] = typeName;
    rq["methodName"] = methodName;
    rq["argsJson"] = FormatArgs(argsJson);
    return SendCommand(rq.dump(), response, error, timeoutMs);
}


bool NM_Bridge::InvokeInstance(const std::string& domainId, const std::string& assemblyAlias, const std::string& instanceId, const std::string& typeName, const std::string& methodName, const std::string& argsJson, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "invokeInstance";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["assemblyName"] = assemblyAlias;
    rq["instanceId"] = instanceId;
    rq["methodName"] = methodName;
    rq["argsJson"] = FormatArgs(argsJson);
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

// ---------------- WPF ----------------

bool NM_Bridge::RunWpfApp(const std::string& domainId, const std::string& assemblyName, const std::string& typeName, const std::string& methodName, const std::vector<std::string>& argsJson, std::string& response, std::wstring& error, int timeoutMs) {
    json rq;
    rq["cmd"] = "runWpfApp";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["assemblyName"] = assemblyName;
    rq["typeName"] = typeName;
    rq["methodName"] = methodName;
    if (!argsJson.empty()) rq["argsJson"] = argsJson;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

bool NM_Bridge::StopWpfApp(const std::string& domainId, const std::string& assemblyAlias, std::string& response, std::wstring& error, int timeoutMs) {
    json rq;
    rq["cmd"] = "stopWpfApp";
    rq["domainId"] = domainId;
    rq["authToken"] = authToken;
    rq["assemblyAlias"] = assemblyAlias;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}


// ---------------- SendCommand ----------------

bool NM_Bridge::SendCommand(const std::string& requestJson, std::string& output, std::wstring& error, int timeoutMs)
{
    if (pipename.empty())
    {
        error = L"Server not started";
        return false;
    }

    std::string pipePath = "\\\\.\\pipe\\" + pipename;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    DWORD start = GetTickCount64();

    while (true)
    {
        hPipe = CreateFileA(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) break;

        DWORD errc = GetLastError();
        if (errc != ERROR_PIPE_BUSY && errc != ERROR_FILE_NOT_FOUND)
        {
            error = L"CreateFile pipe failed";
            return false;
        }

        if (!WaitNamedPipeA(pipePath.c_str(), 100))
        {
            if ((GetTickCount64() - start) > (DWORD)timeoutMs)
            {
                error = L"Timeout connecting to pipe";
                return false;
            }
        }
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD written = 0;
    if (!WriteFile(hPipe, requestJson.c_str(), (DWORD)requestJson.size(), &written, nullptr))
    {
        CloseHandle(hPipe);
        error = L"WriteFile failed";
        return false;
    }

    std::string buffer;
    char temp[8192];
    DWORD bytesRead = 0;

    while (true)
    {
        BOOL r = ReadFile(hPipe, temp, sizeof(temp), &bytesRead, nullptr);
        DWORD err = GetLastError();
        if (bytesRead > 0) buffer.append(temp, bytesRead);
        if (r) break;
        if (!r && err != ERROR_MORE_DATA) break;
    }

    CloseHandle(hPipe);

    if (buffer.empty())
    {
        error = L"Empty response";
        return false;
    }

    output = buffer;

    auto resp = json::parse(output, nullptr, false);
    if (resp.is_discarded())
    {
        error = L"Invalid JSON response";
        return false;
    }

    if (resp.is_object())
    {
        if (!resp.value("success", false))
        {
            std::string errMsg = resp.value("error", "Unknown error");
            error = utf8_to_utf16(errMsg);
            return false;
        }
    }
    return true;
}


std::string NM_Bridge::FormatArgs(const std::string& argsJson)
{
    std::string finalArgs = argsJson;
    if (finalArgs.empty() || finalArgs == "null") {
        return "[]";
    }
    if (finalArgs.front() != '[' || finalArgs.back() != ']') {
        json arr = json::array();
        arr.push_back(finalArgs);
        return arr.dump();
    }
    return finalArgs;
}


void NM_Bridge::UnlinkModuleFromPEB(HMODULE hModule)
{
    if (!hModule) return;

#ifdef _M_X64
    PPEB pPEB = (PPEB)__readgsqword(0x60);
#else
    PPEB pPEB = (PPEB)__readfsdword(0x30);
#endif

    PPEB_LDR_DATA_FULL pLdr = (PPEB_LDR_DATA_FULL)pPEB->Ldr;
    PLIST_ENTRY Head = &pLdr->InLoadOrderModuleList;
    PLIST_ENTRY CurrentEntry = Head->Flink;

    while (CurrentEntry != Head && CurrentEntry != NULL)
    {
        PLDR_DATA_TABLE_ENTRY_FULL CurrentData = CONTAINING_RECORD(CurrentEntry, LDR_DATA_TABLE_ENTRY_FULL, InLoadOrderLinks);

        if (CurrentData->DllBase == hModule)
        {
            CurrentData->InLoadOrderLinks.Blink->Flink = CurrentData->InLoadOrderLinks.Flink;
            CurrentData->InLoadOrderLinks.Flink->Blink = CurrentData->InLoadOrderLinks.Blink;

            CurrentData->InMemoryOrderLinks.Blink->Flink = CurrentData->InMemoryOrderLinks.Flink;
            CurrentData->InMemoryOrderLinks.Flink->Blink = CurrentData->InMemoryOrderLinks.Blink;

            if (CurrentData->InInitializationOrderLinks.Flink && CurrentData->InInitializationOrderLinks.Blink) {
                CurrentData->InInitializationOrderLinks.Blink->Flink = CurrentData->InInitializationOrderLinks.Flink;
                CurrentData->InInitializationOrderLinks.Flink->Blink = CurrentData->InInitializationOrderLinks.Blink;
            }

            if (CurrentData->FullDllName.Buffer)
                SecureZeroMemory(CurrentData->FullDllName.Buffer, CurrentData->FullDllName.MaximumLength);

            if (CurrentData->BaseDllName.Buffer)
                SecureZeroMemory(CurrentData->BaseDllName.Buffer, CurrentData->BaseDllName.MaximumLength);

            return;
        }
        CurrentEntry = CurrentEntry->Flink;
    }
}

void NM_Bridge::HideCLR()
{
    const char* clr_modules[] = {
        "clr.dll", "mscoree.dll", "clrjit.dll",
        "coreclr.dll", "hostfxr.dll"
    };

    for (const char* mod : clr_modules) {
        HMODULE hMod = GetModuleHandleA(mod);
        if (hMod) UnlinkModuleFromPEB(hMod);
    }
}