# NM-Bridge (Native-Managed Bridge) 🌉

🌍 *Read this in other languages: [English](#english) | [Русский](#русский)*

---

<a id="english"></a>
## 🇬🇧 English

**NM-Bridge** is a lightweight and secure library for interaction between unmanaged code (C++) and managed code (C# / .NET) without the heavy C++/CLI or complex COM interop.

The bridge uses **CLR Hosting** to run the .NET environment directly within the C++ process, and **Named Pipes** with **JSON** serialization for fast and secure method invocation.

### ✨ Key Features

* **Full Isolation:** Supports the creation and unloading of isolated `AppDomain`s. You can load and unload assemblies (DLLs) on the fly without memory leaks in the main process.
* **Flexible Assembly Loading:** Load .NET libraries directly from the hard drive (`LoadFromFile`) or straight from RAM (`LoadFromMemory`), which is excellent for anti-reverse engineering protection.
* **Smart Method Invocation (Reflection):** Automatic resolution of constructor and method overloads in C#. Parameters are passed as JSON arrays and automatically cast to the required .NET types.
* **Security:** Named pipes are protected by system access rights (current Windows user only), and each session is secured with a unique authentication token.
* **Zero-Dependency (almost):** The C++ side uses only the standard Windows API and the header-only `nlohmann/json` library.

### ⚙️ Requirements

* **OS:** Windows (uses specific APIs: `mscoree.dll`, Named Pipes).
* **C++ Compiler:** C++17 support (MSVC).
* **.NET Framework:** 4.0 or higher (4.7.2+ recommended).
* **C++ Dependencies:** [nlohmann/json](https://github.com/nlohmann/json) (`json.hpp` header).
* **C# Dependencies:** `Newtonsoft.Json`.

---

<a id="русский"></a>
## 🇷🇺 Русский

**NM-Bridge** — это легковесная и безопасная библиотека для взаимодействия между неуправляемым кодом (C++) и управляемым кодом (C# / .NET) без использования громоздкого C++/CLI или сложного COM-взаимодействия. 

Мост использует механизм **CLR Hosting** для запуска .NET-окружения прямо внутри процесса C++, и **Именованные каналы (Named Pipes)** с сериализацией в **JSON** для быстрого и безопасного вызова методов.

### ✨ Ключевые возможности

* **Полная изоляция:** Поддержка создания и выгрузки изолированных `AppDomain`. Вы можете загружать и выгружать сборки (DLL) "на лету", не оставляя утечек памяти в основном процессе.
* **Гибкая загрузка сборок:** Загрузка .NET библиотек напрямую с жесткого диска (`LoadFromFile`) или прямо из оперативной памяти (`LoadFromMemory`), что отлично подходит для защиты от реверс-инжиниринга.
* **Умный вызов методов (Reflection):** Автоматическое разрешение перегрузок конструкторов и методов в C#. Параметры передаются в виде JSON-массивов и автоматически приводятся к нужным типам .NET.
* **Безопасность:** Именованные пайпы защищены системными правами доступа (только для текущего пользователя Windows), а каждая сессия защищена уникальным токеном авторизации.
* **Zero-Dependency (почти):** На стороне C++ используется только стандартный Windows API и header-only библиотека `nlohmann/json`.

### ⚙️ Требования

* **ОС:** Windows (используются специфичные API: `mscoree.dll`, Named Pipes).
* **Компилятор C++:** Поддержка C++17 (MSVC).
* **.NET Framework:** 4.0 или выше (рекомендуется 4.7.2+).
* **Зависимости C++:** [nlohmann/json](https://github.com/nlohmann/json) (заголовочный файл `json.hpp`).
* **Зависимости C#:** `Newtonsoft.Json`.