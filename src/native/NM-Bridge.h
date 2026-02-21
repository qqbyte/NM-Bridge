// NM-Bridge.h

#pragma once
#ifdef _WIN32

#include <windows.h>
#include <metahost.h>
#include <string>
#include <vector>

#pragma comment(lib, "mscoree.lib")

class NM_Bridge {
	
public:
    NM_Bridge();
    ~NM_Bridge();

    bool Init(const std::wstring& HelperDllPath, std::wstring& error);
    void Shutdown();
	
    bool CreateDomain(const std::string& domainId, std::string& response, std::wstring& error, int timeoutMs = 15000);
    bool UnloadDomain(const std::string& domainId, std::string& response, std::wstring& error, int timeoutMs = 15000);
	
	bool LoadFromFile(const std::string& domainId, const std::wstring& assemblyPath, const std::string& assemblyAlias, std::string& response, std::wstring& error, int timeoutMs = 15000);
    bool LoadFromBytes(const std::string& domainId, const std::string& bytesBase64, const std::string& simpleName, std::string& response, std::wstring& error, int timeoutMs = 15000);
	
    bool CreateInstance(const std::string& domainId, const std::string& assemblyAlias, const std::string& typeName, const std::string& constructorArgsJson, std::string& response, std::wstring& error, int timeoutMs = 15000);
    bool ReleaseInstance(const std::string& domainId, const std::string& instanceId, std::string& response, std::wstring& error, int timeoutMs = 15000);
	
    bool InvokeStatic(const std::string& domainId, const std::string& assemblyAlias, const std::string& typeName, const std::string& methodName, const std::string& argsJson, std::string& response, std::wstring& error, int timeoutMs = 15000);
    bool InvokeInstance(const std::string& domainId, const std::string& assemblyAlias, const std::string& instanceId, const std::string& typeName, const std::string& methodName, const std::string& argsJson, std::string& response, std::wstring& error, int timeoutMs = 15000);


private:
    ICLRMetaHost* MetaHost = nullptr;
    ICLRRuntimeInfo* RuntimeInfo = nullptr;
    ICLRRuntimeHost* ClrRuntimeHost = nullptr;
    std::string pipename;
	std::string authToken;

    bool StartManagedServer(const std::wstring& HelperDllPath, const std::string& request, std::string& output, std::wstring& error, int timeoutMs = 15000);
    bool SendCommand(const std::string& requestJson, std::string& output, std::wstring& error, int timeoutMs); 
};
#endif
