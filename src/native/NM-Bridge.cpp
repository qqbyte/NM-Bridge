// NM-Bridge.cpp

#include "NM-Bridge.h"

#ifdef _WIN32

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
        static const char* table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string out;
        size_t i = 0;
        size_t len = data.size();

        while (i < len)
        {
            int a = data[i++];
            int b = (i < len) ? data[i++] : 0;
            int c = (i < len) ? data[i++] : 0;

            int triple = (a << 16) + (b << 8) + c;

            out.push_back(table[(triple >> 18) & 0x3F]);
            out.push_back(table[(triple >> 12) & 0x3F]);
            out.push_back((i - 1 <= len) ? table[(triple >> 6) & 0x3F] : '=');
            out.push_back((i <= len) ? table[triple & 0x3F] : '=');
        }

        int mod = static_cast<int>(len % 3);
        if (mod == 1)
        {
            out[out.size() - 1] = '=';
            out[out.size() - 2] = '=';
        }
        else if (mod == 2)
        {
            out[out.size() - 1] = '=';
        }

        return out;
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
    if (ClrRuntimeHost)
        return true;

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
        error = L"ExecuteInDefaultAppDomain(StartServer) failed";
        return false;
    }

    int tries = timeoutMs / 200;
    bool ok = false;

    for (int i = 0; i < tries; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string pipePath = "\\\\.\\pipe\\" + pipename;
        HANDLE h = CreateFileA(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            ok = true;
            break;
        }
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
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

bool NM_Bridge::UnloadDomain(const std::string& domainId, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "unloadDomain";
    rq["domainId"] = domainId;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

// ---------------- Load ----------------

bool NM_Bridge::LoadFromFile(const std::string& domainId, const std::wstring& assemblyPath, const std::string& assemblyAlias, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "loadFromFile";
    rq["domainId"] = domainId;
    rq["path"] = utf16_to_utf8(assemblyPath);
    if (!assemblyAlias.empty()) rq["assemblyAlias"] = assemblyAlias;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

bool NM_Bridge::LoadFromBytes(const std::string& domainId, const std::string& bytesBase64, const std::string& simpleName, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "loadFromBytes";
    rq["domainId"] = domainId;
    rq["bytesBase64"] = bytesBase64;
    if (!simpleName.empty()) rq["assemblySimpleName"] = simpleName;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

// ---------------- Invoke ----------------

bool NM_Bridge::InvokeStatic( const std::string& domainId, const std::string& assemblyAlias, const std::string& typeName, const std::string& methodName, const std::string& argsJson, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "invoke";
    rq["domainId"] = domainId;
    rq["assemblyName"] = assemblyAlias;
    rq["typeName"] = typeName;
    rq["methodName"] = methodName;
    rq["isStatic"] = true;
    rq["instanceId"] = nullptr;
    rq["argsJson"] = argsJson;
    return SendCommand(rq.dump(), response, error, timeoutMs);
}

bool NM_Bridge::InvokeInstance(const std::string& domainId, const std::string& assemblyAlias, const std::string& instanceId, const std::string& typeName, const std::string& methodName, const std::string& argsJson, std::string& response, std::wstring& error, int timeoutMs)
{
    json rq;
    rq["cmd"] = "invoke";
    rq["domainId"] = domainId;
    rq["assemblyName"] = assemblyAlias;
    rq["typeName"] = typeName;
    rq["methodName"] = methodName;
    rq["isStatic"] = false;
    rq["instanceId"] = instanceId;
    rq["argsJson"] = argsJson;
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

    json j;
    try
    {
        j = json::parse(requestJson);
    }
    catch (...)
    {
        error = L"Invalid JSON request";
        return false;
    }

    j["authToken"] = authToken;
    std::string finalRequest = j.dump();

    std::string pipePath = "\\\\.\\pipe\\" + pipename;

    HANDLE hPipe = INVALID_HANDLE_VALUE;

    while (true)
    {
        hPipe = CreateFileA(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (hPipe != INVALID_HANDLE_VALUE)
            break;

        DWORD errc = GetLastError();

        if (errc != ERROR_PIPE_BUSY && errc != ERROR_FILE_NOT_FOUND)
        {
            error = L"CreateFile pipe failed";
            return false;
        }

        if (!WaitNamedPipeA(pipePath.c_str(), timeoutMs))
        {
            error = L"Timeout connecting to pipe";
            return false;
        }
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD written = 0;
    BOOL ok = WriteFile(hPipe, finalRequest.c_str(), (DWORD)finalRequest.size(), &written, nullptr);

    if (!ok)
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

        if (bytesRead > 0)
            buffer.append(temp, bytesRead);

        if (r) 
            break;

        if (!r && err != ERROR_MORE_DATA)
            break;
    }

    CloseHandle(hPipe);

    if (buffer.empty())
    {
        error = L"Empty response";
        return false;
    }

    output = buffer;

    try
    {
        auto resp = json::parse(output);

        if (!resp.value("success", false))
        {
            std::string errMsg = resp.value("error", "Unknown error");
            error = utf8_to_utf16(errMsg);
            return false;
        }
    }
    catch (...)
    {
        error = L"Invalid JSON response";
        return false;
    }

    return true;
}