// Class1.cs

using Newtonsoft.Json.Linq;
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Reflection;
using System.Security;
using System.Security.AccessControl;
using System.Security.Permissions;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Xml.Linq;

namespace MANAGED_Bridge
{
    public class Managed_Bridge
    {
        private static string authToken;
        private static Thread serverThread;
        private static volatile bool serverRunning = false;
        private static string serverPipeName;
        private static readonly object sync = new object();

        class DomainRecord { public string Id; public AppDomain Domain; public DomainProxy Proxy; }
        static Dictionary<string, DomainRecord> domains = new Dictionary<string, DomainRecord>(StringComparer.OrdinalIgnoreCase);
        static Dictionary<string, Queue<string>> pools = new Dictionary<string, Queue<string>>(StringComparer.OrdinalIgnoreCase);


        public static int StartServer(string initJson)
        {
            try
            {
                if (string.IsNullOrEmpty(initJson))
                    throw new ArgumentException("initJson is empty");

                var j = JObject.Parse(initJson);

                string cmd = (string)j["cmd"];
                if (cmd != "_start_server")
                    throw new InvalidOperationException("Expected _start_server command");

                string pipeName = (string)j["pipeName"];
                authToken = (string)j["authToken"];

                string ManagedBridgePath = (string)j["ManagedBridgePath"] ?? "";

                if (string.IsNullOrEmpty(pipeName))
                    pipeName = $"managedbridge_server_{System.Diagnostics.Process.GetCurrentProcess().Id}";

                serverPipeName = pipeName;

                lock (sync)
                {
                    if (serverRunning) return 1;
                    serverRunning = true;
                    serverThread = new Thread(ServerLoop) { IsBackground = true };
                    serverThread.Start();
                }

                return 1;
            }
            catch (Exception ex)
            {
                return -1;
            }
        }


        public static int StopServer()
        {
            try
            {
                lock (sync)
                {
                    serverRunning = false;
                }

                if (serverThread != null && serverThread.IsAlive && Thread.CurrentThread.ManagedThreadId != serverThread.ManagedThreadId)
                    serverThread.Join(2000);

                lock (sync)
                {
                    foreach (var kv in domains.ToArray())
                    {
                        try { AppDomain.Unload(kv.Value.Domain); } catch { }
                    }

                    domains.Clear();
                    pools.Clear();
                }
                return 1;
            }
            catch (Exception ex)
            {
                return -1;
            }
        }


        private static void ServerLoop()
        {
            while (serverRunning)
            {
                NamedPipeServerStream pipe = null;
                try
                {
                    var ps = new PipeSecurity();
                    var sid = WindowsIdentity.GetCurrent().User;

                    ps.AddAccessRule(new PipeAccessRule(sid, PipeAccessRights.FullControl, AccessControlType.Allow));
                    pipe = new NamedPipeServerStream(serverPipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Message, PipeOptions.None, 32768, 32768, ps);
                    pipe.WaitForConnection();

                    string requestJson;
                    using (var ms = new MemoryStream())
                    {
                        var buffer = new byte[32768];
                        int bytesRead;
                        do
                        {
                            bytesRead = pipe.Read(buffer, 0, buffer.Length);
                            if (bytesRead > 0)
                                ms.Write(buffer, 0, bytesRead);
                        } while (!pipe.IsMessageComplete && bytesRead > 0);

                        requestJson = Encoding.UTF8.GetString(ms.ToArray());
                    }

                    if (string.IsNullOrWhiteSpace(requestJson)) requestJson = "{}";

                    string responseJson = ProcessRequest(requestJson);

                    if (!string.IsNullOrEmpty(responseJson))
                    {
                        byte[] resp = Encoding.UTF8.GetBytes(responseJson);
                        pipe.Write(resp, 0, resp.Length);
                        pipe.Flush();
                    }
                    pipe?.Dispose();
                }
                catch (OperationCanceledException) { break; }
                catch (ObjectDisposedException) { break; }
                catch (Exception ex)
                {
                    try { pipe?.Dispose(); } catch { }
                    Thread.Sleep(100);
                }
            }
        }


        private static string ProcessRequest(string reqJson)
        {
            try
            {
                var req = JObject.Parse(reqJson);
                string token = (string)req["authToken"];

                if (authToken != null && token != authToken)
                {
                    return new JObject
                    {
                        ["success"] = false,
                        ["error"] = "Unauthorized"
                    }.ToString(Newtonsoft.Json.Formatting.None);
                }
                string cmd = (string)req["cmd"] ?? "";

                JObject resp;
                switch (cmd)
                {
                    case "createDomain":
                        resp = Cmd_CreateDomain(req); break;
                    case "unloadDomain":
                        resp = Cmd_UnloadDomain(req); break;
                    case "loadFromFile":
                        resp = Cmd_LoadFromFile(req); break;
                    case "loadFromMemory":
                        resp = Cmd_LoadFromMemory(req); break;
                    case "createInstance":
                        resp = Cmd_CreateInstance(req); break;
                    case "invoke":
                        resp = Cmd_Invoke(req); break;
                    case "releaseInstance":
                        resp = Cmd_ReleaseInstance(req); break;
                    case "stopServer":
                        resp = new JObject { ["success"] = true }; StopServer(); break;
                    default:
                        resp = new JObject { ["success"] = false, ["error"] = "Unknown cmd: " + cmd }; break;
                }
                return resp.ToString(Newtonsoft.Json.Formatting.None);
            }
            catch (Exception ex)
            {
                var j = new JObject { ["success"] = false, ["error"] = ex.ToString() };
                return j.ToString(Newtonsoft.Json.Formatting.None);
            }
        }


        #region Command implementations

        private static JObject Cmd_CreateDomain(JObject req)
        {
            string requestedId = (string)req["domainId"];
            string domainId = string.IsNullOrEmpty(requestedId) ? Guid.NewGuid().ToString("N") : requestedId;

            lock (sync)
            {
                if (domains.ContainsKey(domainId))
                    return JObject.FromObject(new { success = false, error = "Domain already exists: " + domainId });

                var setup = new AppDomainSetup { ApplicationBase = AppDomain.CurrentDomain.SetupInformation.ApplicationBase };
                var evidence = AppDomain.CurrentDomain.Evidence;
                PermissionSet permSet = new PermissionSet(PermissionState.Unrestricted);

                string domName = "ManagedBridgeDomain_" + domainId;
                var domain = AppDomain.CreateDomain(domName, evidence, setup, permSet);
                string clrHelperPath = typeof(DomainProxy).Assembly.Location;
                object proxyObj = domain.CreateInstanceFromAndUnwrap(clrHelperPath, typeof(DomainProxy).FullName);

                var proxy = (DomainProxy)proxyObj;
                var rec = new DomainRecord { Id = domainId, Domain = domain, Proxy = proxy };
                domains[domainId] = rec;

                return JObject.FromObject(new { success = true, domainId = domainId });
            }
        }


        private static JObject Cmd_UnloadDomain(JObject req)
        {
            string domainId = (string)req["domainId"];
            if (string.IsNullOrEmpty(domainId))
                return JObject.FromObject(new { success = false, error = "domainId missing" });

            lock (sync)
            {
                if (!domains.TryGetValue(domainId, out var rec))
                    return JObject.FromObject(new { success = false, error = "domain not found" });

                try
                {
                    AppDomain.Unload(rec.Domain);
                }
                catch (Exception ex)
                {
                    return JObject.FromObject(new { success = false, error = "Unload failed: " + ex.Message });
                }

                domains.Remove(domainId);
                return JObject.FromObject(new { success = true });
            }
        }


        private static JObject Cmd_LoadFromFile(JObject req)
        {
            string domainId = (string)req["domainId"];
            string path = (string)req["path"];
            string alias = (string)req["assemblyAlias"];

            if (string.IsNullOrEmpty(domainId) || string.IsNullOrEmpty(path)) return JObject.FromObject(new { success = false, error = "domainId/path required" });
            lock (sync) { if (!domains.TryGetValue(domainId, out var rec)) return JObject.FromObject(new { success = false, error = "domain not found" }); }
            try
            {
                string asmName = domains[domainId].Proxy.LoadFromFile(path, alias);
                return JObject.FromObject(new { success = true, assemblyName = asmName });
            }
            catch (Exception ex) { return JObject.FromObject(new { success = false, error = ex.ToString() }); }
        }


        private static JObject Cmd_LoadFromMemory(JObject req)
        {
            string domainId = (string)req["domainId"];
            string bytesBase64 = (string)req["bytesBase64"];
            string assemblySimpleName = (string)req["assemblySimpleName"] ?? (string)req["assemblyAlias"];
            if (string.IsNullOrEmpty(domainId) || string.IsNullOrEmpty(bytesBase64)) return JObject.FromObject(new { success = false, error = "domainId/bytesBase64 required" });
            lock (sync) { if (!domains.TryGetValue(domainId, out var rec)) return JObject.FromObject(new { success = false, error = "domain not found" }); }
            try
            {
                byte[] raw = Convert.FromBase64String(bytesBase64);
                string asmName = domains[domainId].Proxy.LoadFromMemory(raw, assemblySimpleName);
                return JObject.FromObject(new { success = true, assemblyName = asmName });
            }
            catch (Exception ex) { return JObject.FromObject(new { success = false, error = ex.ToString() }); }
        }


        private static JObject Cmd_CreateInstance(JObject req)
        {
            string domainId = (string)req["domainId"];
            string typeName = (string)req["typeName"];
            string ctorArgsJson = (string)req["ctorArgsJson"] ?? "null";
            if (string.IsNullOrEmpty(domainId) || string.IsNullOrEmpty(typeName)) return JObject.FromObject(new { success = false, error = "domainId/typeName required" });
            lock (sync) { if (!domains.TryGetValue(domainId, out var rec)) return JObject.FromObject(new { success = false, error = "domain not found" }); }
            try { string instId = domains[domainId].Proxy.CreateInstance(typeName, ctorArgsJson); return JObject.FromObject(new { success = true, instanceId = instId }); } catch (Exception ex) { return JObject.FromObject(new { success = false, error = ex.ToString() }); }
        }


        private static JObject Cmd_Invoke(JObject req)
        {
            string domainId = (string)req["domainId"];
            string typeName = (string)req["typeName"];
            string methodName = (string)req["methodName"];
            bool isStatic = (bool?)req["isStatic"] ?? true;
            JToken instTok = req["instanceId"];
            string instanceId = instTok != null && instTok.Type != JTokenType.Null ? instTok.ToString() : null;
            string argsJson = (string)req["argsJson"] ?? "null";
            if (string.IsNullOrEmpty(domainId) || string.IsNullOrEmpty(methodName)) return JObject.FromObject(new { success = false, error = "domainId/methodName required" });
            lock (sync) { if (!domains.TryGetValue(domainId, out var rec)) return JObject.FromObject(new { success = false, error = "domain not found" }); }
            try { object result = domains[domainId].Proxy.Invoke(typeName, methodName, isStatic, instanceId, argsJson); return JObject.FromObject(new { success = true, result = result }); } catch (Exception ex) { return JObject.FromObject(new { success = false, error = ex.ToString() }); }
        }

        private static JObject Cmd_ReleaseInstance(JObject req)
        {
            string domainId = (string)req["domainId"];
            string instanceId = (string)req["instanceId"];
            if (string.IsNullOrEmpty(domainId) || string.IsNullOrEmpty(instanceId)) return JObject.FromObject(new { success = false, error = "domainId/instanceId required" });
            lock (sync) { if (!domains.TryGetValue(domainId, out var rec)) return JObject.FromObject(new { success = false, error = "domain not found" }); }
            try { bool ok = domains[domainId].Proxy.ReleaseInstance(instanceId); return JObject.FromObject(new { success = ok }); } catch (Exception ex) { return JObject.FromObject(new { success = false, error = ex.ToString() }); }
        }

        #endregion
    }

    public class DomainProxy : MarshalByRefObject
    {
        static DomainProxy()
        {
            AppDomain currentDomain = AppDomain.CurrentDomain;
            currentDomain.AssemblyResolve += CurrentDomain_AssemblyResolve;
        }


        private static Assembly CurrentDomain_AssemblyResolve(object sender, ResolveEventArgs args)
        {
            if (args.Name.StartsWith("Managed_Bridge,") || args.RequestingAssembly?.FullName.StartsWith("Managed_Bridge,") == true)
            {
                Assembly executingAsm = Assembly.GetExecutingAssembly();
                if (executingAsm.FullName.StartsWith("Managed_Bridge,"))
                    return executingAsm;

                foreach (Assembly asm in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (asm.FullName.StartsWith("Managed_Bridge,"))
                        return asm;
                }
            }

            return null;
        }


        public override object InitializeLifetimeService() => null;
        Dictionary<string, Assembly> assemblies = new Dictionary<string, Assembly>(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, object> instances = new Dictionary<string, object>();


        public string LoadFromFile(string path, string alias = null)
        {
            if (!File.Exists(path))
                throw new FileNotFoundException("File not found: " + path);

            byte[] raw = File.ReadAllBytes(path);
            Assembly asm = Assembly.Load(raw);
            string name = !string.IsNullOrEmpty(alias) ? alias : asm.GetName().Name;
            assemblies[name] = asm;
            return name;
        }


        public string LoadFromMemory(byte[] raw, string assemblySimpleName = null)
        {
            var asm = Assembly.Load(raw);
            var name = asm.GetName().Name;
            if (!string.IsNullOrEmpty(assemblySimpleName)) name = assemblySimpleName;
            assemblies[name] = asm;
            return name;
        }


        public string CreateInstance(string typeName, string ctorArgsJson)
        {
            Type t = ResolveType(typeName) ?? throw new TypeLoadException("Type not found: " + typeName);

            if (string.IsNullOrWhiteSpace(ctorArgsJson) || ctorArgsJson == "null") ctorArgsJson = "[]";
            var objArr = Newtonsoft.Json.JsonConvert.DeserializeObject<object[]>(ctorArgsJson) ?? new object[0];

            ConstructorInfo targetCtor = null;
            object[] finalArgs = null;

            foreach (var ctor in t.GetConstructors(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance))
            {
                var pInfos = ctor.GetParameters();
                if (pInfos.Length == objArr.Length)
                {
                    try
                    {
                        finalArgs = new object[pInfos.Length];
                        for (int i = 0; i < pInfos.Length; i++)
                        {
                            finalArgs[i] = Newtonsoft.Json.JsonConvert.DeserializeObject(
                                Newtonsoft.Json.JsonConvert.SerializeObject(objArr[i]),
                                pInfos[i].ParameterType);
                        }
                        targetCtor = ctor;
                        break;
                    }
                    catch {}
                }
            }

            if (targetCtor == null)
                throw new MissingMethodException($"Constructor for {typeName} with argument count {objArr.Length} not found");

            object inst = targetCtor.Invoke(finalArgs);
            string id = "inst_" + Guid.NewGuid().ToString("N");
            instances[id] = inst;
            return id;
        }


        public bool ReleaseInstance(string id)
        {
            if (instances.ContainsKey(id))
            {
                instances.Remove(id);
                return true;
            }
            return false;
        }


        public object Invoke(string typeName, string methodName, bool isStatic, string instanceId, string argsJson, int timeoutMs = 0)
        {
            object target = null;
            Type type = null;

            if (!string.IsNullOrEmpty(instanceId))
            {
                if (!instances.TryGetValue(instanceId, out target)) throw new ArgumentException("InstanceId not found: " + instanceId);
                type = target.GetType();
            }
            else { type = ResolveType(typeName) ?? throw new TypeLoadException("Type not found: " + typeName); }

            if (string.IsNullOrWhiteSpace(argsJson) || argsJson == "null") argsJson = "[]";
            var objArr = Newtonsoft.Json.JsonConvert.DeserializeObject<object[]>(argsJson) ?? new object[0];

            var flags = BindingFlags.Public | BindingFlags.NonPublic | (isStatic ? BindingFlags.Static : BindingFlags.Instance);

            MethodInfo targetMethod = null;
            object[] finalArgs = null;

            // Умный поиск метода (теперь поддерживает перегрузки методов с одинаковым именем!)
            foreach (var method in type.GetMethods(flags).Where(m => m.Name == methodName))
            {
                var pInfos = method.GetParameters();
                if (pInfos.Length == objArr.Length)
                {
                    try
                    {
                        finalArgs = new object[pInfos.Length];
                        for (int i = 0; i < pInfos.Length; i++)
                        {
                            finalArgs[i] = Newtonsoft.Json.JsonConvert.DeserializeObject(
                                Newtonsoft.Json.JsonConvert.SerializeObject(objArr[i]),
                                pInfos[i].ParameterType);
                        }
                        targetMethod = method;
                        break;
                    }
                    catch { }
                }
            }

            if (targetMethod == null)
                throw new MissingMethodException($"Method {methodName} with {objArr.Length} arguments not found.");

            return targetMethod.Invoke(isStatic ? null : target, finalArgs);
        }

        private Type ResolveType(string fullName)
        {
            if (string.IsNullOrEmpty(fullName))
                return null;

            foreach (var asm in assemblies.Values)
            {
                try
                {
                    var t = asm.GetType(fullName, throwOnError: false, ignoreCase: false);
                    if (t != null)
                        return t;
                }
                catch { }
            }

            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                try
                {
                    var t = asm.GetType(fullName, throwOnError: false, ignoreCase: false);
                    if (t != null)
                        return t;
                }
                catch { }
            }

            return null;
        }


        public string[] GetLoadedAliases()
        {
            return assemblies.Keys.ToArray();
        }

    }
}
