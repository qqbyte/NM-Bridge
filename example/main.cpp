// main.cpp

#include "NM-Bridge.h"
#include "include/json.hpp"
#include <iostream>
#include <fstream>

using json = nlohmann::json;

///////////////////////////////////////////////////////////////////////////////
// Helper function for reading a file into a byte array
// 
// Вспомогательная функция для чтения файла в массив байтов.
///////////////////////////////////////////////////////////////////////////////

std::vector<BYTE> ReadFileBytes(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<BYTE> buffer(size);
    if (file.read((char*)buffer.data(), size)) return buffer;
    return {};
}



int main() {

    srand((unsigned int)time(NULL));

    NM_Bridge bridge;
    std::wstring error;
    std::string response;

    std::cout << "[*] Init bridge..." << std::endl;

    ///////////////////////////////////////////////////////////////////////////////
    // Specify the path to the compiled library of your bridge
    // 
    // Укажите путь к скомпилированной библиотеке вашего моста
    ///////////////////////////////////////////////////////////////////////////////

    if (!bridge.Init(L"..\\..\\managed_bridge.dll", error)) {
        std::wcout << L"[-] Error Init: " << error << std::endl;
        return 1;
    }
    std::cout << "[+] Server started successfully!" << std::endl;



    ///////////////////////////////////////////////////////////////////////////////
    // Optional: Hides CLR modules by unlinking them from the PEB module lists 
    // to avoid detection by standard process monitoring tools.
    // 
    // Необязательно: Скрывает модули CLR, удаляя их из списков модулей PEB.
    // Чтобы избежать обнаружения стандартными инструментами мониторинга процессов.
    ///////////////////////////////////////////////////////////////////////////////

    HideCLR();




    ///////////////////////////////////////////////////////////////////////////////
    // 1. Create domain
    // 
    // 1. Создание домена
    ///////////////////////////////////////////////////////////////////////////////

    std::string domainId = "TestDomain_123";
    if (!bridge.CreateDomain(domainId, response, error)) {
        std::wcout << L"[-] Error CreateDomain: " << error << std::endl;
        return 1;
    }
    std::cout << "[+] Domain create." << std::endl;




    ///////////////////////////////////////////////////////////////////////////////
    // 2. Load from file
    // 
    // 2. Загрузка из файла
    ///////////////////////////////////////////////////////////////////////////////

    std::wstring targetDll = L"TestLib.dll";
    std::string asmAlias = "TestLibAlias";
    if (!bridge.LoadFromFile(domainId, targetDll, asmAlias, response, error)) {
        std::wcout << L"[-] Error LoadFromFile: " << error << std::endl;
    }
    else {
        std::cout << "[+] Assembly loaded from file." << std::endl;
    }




    ///////////////////////////////////////////////////////////////////////////////
    // 3. LoadFromMemory
    // Useful if you downloaded the DLL over the network
    // 
    // 3. Загрузка из памяти
    // Это полезно, если вы загрузили DLL-файл по сети.
    ///////////////////////////////////////////////////////////////////////////////

    std::vector<BYTE> dllBytes = ReadFileBytes(L"TestLib.dll");
    if (!dllBytes.empty()) {
        if (bridge.LoadFromMemory(domainId, dllBytes, "TestLibMemory", response, error)) {
            std::cout << "[+] The assembly was loaded from memory!" << std::endl;
            asmAlias = "TestLibMemory";
        }
    }




    ///////////////////////////////////////////////////////////////////////////////
    // 4. Invoke static
    // Note: arguments are ALWAYS passed as a JSON array!
    // 
    // 4. Вызов статического метода
    // Примечание: аргументы ВСЕГДА передаются в виде массива JSON!
    ///////////////////////////////////////////////////////////////////////////////

    std::string staticArgs = "[15, 27]";
    if (bridge.InvokeStatic(domainId, asmAlias, "TestLib.Calculator", "Add", staticArgs, response, error)) {
        json jRes = json::parse(response);
        std::cout << "[+] InvokeStatic (Add 15+27): " << jRes["result"] << std::endl;
    }
    else {
        std::wcout << L"[-] Error InvokeStatic: " << error << std::endl;
    }




    ///////////////////////////////////////////////////////////////////////////////
    // 5. Create instance
    // We pass one string argument to the constructor
    // 
    // 5. Создание экземпляра
    // Мы передаем один строковый аргумент конструктору
    ///////////////////////////////////////////////////////////////////////////////

    std::string ctorArgs = "[\"SuperCalc_9000\"]";
    std::string instanceId;
    if (bridge.CreateInstance(domainId, asmAlias, "TestLib.Calculator", ctorArgs, response, error)) {
        json jRes = json::parse(response);
        instanceId = jRes["instanceId"];
        std::cout << "[+] Instance created. ID: " << instanceId << std::endl;
    }
    else {
        std::wcout << L"[-] Error CreateInstance: " << error << std::endl;
    }




    ///////////////////////////////////////////////////////////////////////////////
    // 6. Invoke instance
    // 
    // 6. Вызов экземпляра
    ///////////////////////////////////////////////////////////////////////////////

    if (!instanceId.empty()) {

        // The method doesn't take any arguments, so we pass an empty array.
        // Метод не принимает никаких аргументов, поэтому мы передаем пустой массив.

        std::string instArgs = "[]";
        if (bridge.InvokeInstance(domainId, asmAlias, instanceId, "TestLib.Calculator", "GetInfo", instArgs, response, error)) {
            json jRes = json::parse(response);
            std::cout << "[+] InvokeInstance (GetInfo): " << jRes["result"] << std::endl;
        }
        else {
            std::wcout << L"[-] Error InvokeInstance: " << error << std::endl;
        }




        ///////////////////////////////////////////////////////////////////////////////
        // 7. Release instance
        // It is important to ensure that the C# garbage collector can delete the object
        // 
        // 7. Освободить экземпляр
        // Важно убедиться, что сборщик мусора C# может удалить объект
        ///////////////////////////////////////////////////////////////////////////////

        if (bridge.ReleaseInstance(domainId, instanceId, response, error)) {
            std::cout << "[+] Instance " << instanceId << " has been deallocated." << std::endl;
        }
    }




    ///////////////////////////////////////////////////////////////////////////////
    // 8. Unload domain
    // Clears all memory and unloads loaded DLLs in this domain.
    // 
    // 8. Выгрузка домена
    // Очищает всю память и выгружает загруженные DLL - файлы в этом домене.
    ///////////////////////////////////////////////////////////////////////////////

    if (bridge.UnloadDomain(domainId, response, error)) {
        std::cout << "[+] Domain unload." << std::endl;
    }




    ///////////////////////////////////////////////////////////////////////////////
    // 9. Stopping the server
    // 
    // 9. Остановка сервера
    ///////////////////////////////////////////////////////////////////////////////

    bridge.Shutdown();
    std::cout << "[*] Shutdown." << std::endl;


    std::cin.get();
    return 0;
}