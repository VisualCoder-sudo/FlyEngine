// ScriptHost.cs - the C# entry point that the C++ CoreCLRHost calls each frame.
// Maintains a registry of script instances and drives their coroutines.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Scripting;
using Microsoft.CodeAnalysis.Scripting;

namespace FlyScript
{
    // A running script instance (either attached to an object or standalone).
    public sealed class ScriptInstance
    {
        public readonly int Id;
        public readonly GameObject Target;    // object this script runs on (or None for standalone)
        public readonly IScript Script;       // the user's script implementation
        public readonly string TypeName;      // Script.GetType().FullName, cached for logging
        public readonly IEnumerator Coroutine; // the active enumerator

        private ScriptInstance(int id, GameObject target, IScript script)
        {
            Id = id;
            Target = target;
            Script = script;
            TypeName = script.GetType().FullName ?? script.GetType().Name;
            Coroutine = script.Run().GetEnumerator();
        }

        public static ScriptInstance Create(int id, GameObject target, IScript script)
            => new ScriptInstance(id, target, script);

        // Advance the coroutine by one frame. Returns true if it should continue.
        // Throws on failure - callers are responsible for logging with context
        // (which script, which id) and removing the instance.
        public bool Tick(float dt)
        {
            if (!Coroutine.MoveNext()) return false;

            object current = Coroutine.Current;
            // Support yielding Tick.Wait(frames) - we just tick down the frames.
            if (current is Tick wait)
            {
                // The host doesn't have a per-coroutine timer; we just return true
                // and the C++ host will call us again next frame.
                // For proper frame-wait we'd need a scheduler, but for now:
                return true;
            }

            // Yield return null or anything else = continue next frame.
            return true;
        }
    }

    // Diagnostic helpers - turns a caught exception into a message plus a short
    // actionable suggestion, and reports it through Native.Log so it lands in
    // the in-editor OUTPUT panel instead of only an attached debugger.
    internal static class ScriptDiagnostics
    {
        public static void ReportFailure(string context, Exception ex)
        {
            // Unwrap reflection/TargetInvocation wrappers to get to the real cause.
            Exception real = ex;
            while (real is System.Reflection.TargetInvocationException tie && tie.InnerException != null)
                real = tie.InnerException;

            string suggestion = Suggest(real);

            // ex.ToString() (not real.ToString()) so we keep the full outer chain
            // too - includes "at Namespace.Type.Method() in File.cs:line N" frames
            // when the assembly's PDB is present next to FlyScript.dll.
            Native.Log($"[FlyScript] {context} FAILED: {real.GetType().Name}: {real.Message}");
            if (!string.IsNullOrEmpty(suggestion))
                Native.Log($"[FlyScript]   suggestion: {suggestion}");
            Native.Log($"[FlyScript]   full trace:\n{ex}");
        }

        private static string Suggest(Exception ex) => ex switch
        {
            DllNotFoundException => "A native P/Invoke target couldn't be loaded. Confirm the DllImportResolver in FlyScript.cs targets \"Flyengine\" and returns NativeLibrary.GetMainProgramHandle().",
            EntryPointNotFoundException epnf => $"Native function '{epnf.Message}' wasn't found. Confirm it has __declspec(dllexport) in FlyScriptApi.cpp and the engine was rebuilt.",
            NullReferenceException => "Something was used before it was set - check whether a GameObject handle is valid (obj.IsValid) or whether BindWorld()/BindRuntime() ran before this script started.",
            InvalidCastException => "A property or Native.* call received a value of the wrong type - check the argument types against the SDK signature.",
            MissingMethodException or MissingMemberException => "The compiled FlyScript.dll is out of date relative to this source - force a rebuild (delete Scripts/FlyScript.dll and the obj/bin folders under Scripts/).",
            _ => string.Empty
        };
    }

    // The static host called from C++ via delegates.
    // C++ signatures:
    //   Run(IntPtr host, float dt, int scriptId)
    //   StartObjectScript(IntPtr host, ulong handle, string typeName)
    //   StopObjectScript(IntPtr host, ulong handle)
    //   StartStandalone(IntPtr host, int index)
    //   StopStandalone(IntPtr host, int index)
    //   ClearAll(IntPtr host)
    //   OnPlayStarted(IntPtr host)
    //   OnPlayStopped(IntPtr host)
    public static class ScriptHost
    {
        private static readonly Dictionary<int, ScriptInstance> s_scripts = new Dictionary<int, ScriptInstance>();
        private static readonly HashSet<Type> s_runningTypes = new HashSet<Type>();
        private static int s_nextId = 1;

        public static float DeltaTime { get; set; }

        private static bool s_loggedFirstRun = false;

        public static void Run(IntPtr hostPtr, float dt, int scriptId)
        {
            if (!s_loggedFirstRun)
            {
                s_loggedFirstRun = true;
                // If you don't see this line at all, ScriptHost.Run is never being
                // called from C++ - check CoreCLRHost::Update()'s delegate call, not
                // this file. If you DO see it, C++ -> C# dispatch works and any
                // problem is below this point (native calls, script logic, etc).
                // If Native.Log itself throws here, no P/Invoke call is reaching
                // native code at all - the DllImportResolver/dllexport fix isn't
                // taking effect yet (stale build?), and nothing below this point
                // will be visible in-editor either, only via an attached debugger.
                try { Native.Log("[FlyScript] ScriptHost.Run reached for the first time - C++ -> C# dispatch is working."); }
                catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[FlyScript] Native.Log itself failed: {ex}"); }
            }

            try
            {
                // Bind the runtime pointer so C# can call back into C++.
                Native.BindRuntime(hostPtr);
                DeltaTime = dt;

                var toRemove = new List<int>();

                foreach (var kv in s_scripts)
                {
                    var inst = kv.Value;
                    try
                    {
                        // Bind the object this script runs on so GameObject.Self
                        // resolves correctly; clear it again (even on failure) so
                        // later ticks don't see a stale self.
                        Native.BindSelf(inst.Target.Handle);
                        bool ticked = inst.Tick(DeltaTime);
                        Native.BindSelf(Id64.None);
                        if (!ticked)
                        {
                            Native.Log($"[FlyScript] '{inst.TypeName}' (id={kv.Key}) finished (Run() returned/yield break).");
                            toRemove.Add(kv.Key);
                        }
                    }
                    catch (Exception ex)
                    {
                        Native.BindSelf(Id64.None);
                        ScriptDiagnostics.ReportFailure($"'{inst.TypeName}' (id={kv.Key}) tick", ex);
                        toRemove.Add(kv.Key);
                    }
                }

                foreach (var id in toRemove)
                    s_scripts.Remove(id);
            }
            catch (Exception ex)
            {
                ScriptDiagnostics.ReportFailure("ScriptHost.Run", ex);
            }
        }

        // ---- C++ -> C# lifecycle entry points (resolved via delegates) ----

        public static void StartObjectScript(IntPtr hostPtr, ulong handle, string typeName)
        {
            if (handle == 0) return;
            PerObjectScripts.Start(new Id64(handle), typeName);
        }

        public static void StopObjectScript(IntPtr hostPtr, ulong handle)
        {
            if (handle == 0) return;
            PerObjectScripts.Stop(new Id64(handle));
        }

        public static void StartStandalone(IntPtr hostPtr, int index, string typeName)
        {
            StandaloneScripts.Start(index, typeName);
        }

        public static void StopStandalone(IntPtr hostPtr, int index, string typeName)
        {
            StandaloneScripts.Stop(index);
        }

        public static void ClearAll(IntPtr hostPtr)
        {
            s_scripts.Clear();
            s_runningTypes.Clear();
            StandaloneScripts.ClearAll();
        }

        public static void OnPlayStarted(IntPtr hostPtr)
        {
            // Curated runOnPlay scripts are started by the C++ runtime before it
            // flips play state; managed-side bookkeeping (if any) goes here.
        }

        public static void OnPlayStopped(IntPtr hostPtr)
        {
            ClearAll(hostPtr);
        }

        // Register a new script (per-object or standalone). Returns the script ID,
        // or 0 if construction failed (logged with full detail either way).
        public static int RegisterScript(GameObject target, IScript script)
        {
            int id = s_nextId++;
            try
            {
                var inst = ScriptInstance.Create(id, target, script);
                s_scripts[id] = inst;
                s_runningTypes.Add(script.GetType());
                Native.Log($"[FlyScript] Started '{inst.TypeName}' (id={id}, target={(target.IsValid ? target.Handle.ToString() : "standalone")}).");
                return id;
            }
            catch (Exception ex)
            {
                ScriptDiagnostics.ReportFailure($"'{script.GetType().FullName}' construction (id={id})", ex);
                return 0;
            }
        }

        // Unregister a script by ID.
        public static void UnregisterScript(int id)
        {
            if (s_scripts.TryGetValue(id, out var inst)) s_runningTypes.Remove(inst.Script.GetType());
            s_scripts.Remove(id);
        }

        // Enumerate the concrete IScript types in the loaded assemblies and write them,
// one full type name per line (NUL-terminated), into `buffer` (UTF-8). Returns
// the number of bytes written, or -1 if `buffer` is too small. Used by the C++
// host to seed the explorer's SCRIPTS group so every script shows up and runs.
public static int GetScriptTypeNames(IntPtr hostPtr, IntPtr buffer, int capacity)
        {
            if (buffer == IntPtr.Zero || capacity <= 0) return -1;
            var names = new List<string>();
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                foreach (var t in SafeTypes(asm))
                {
                    if (!typeof(IScript).IsAssignableFrom(t) || t.IsAbstract || t.IsInterface) continue;
                    if (t == typeof(IScript)) continue;
                    names.Add(t.FullName ?? t.Name);
                }
            }

            if (names.Count == 0)
                Native.Log("[FlyScript] No IScript types found in any loaded assembly. Check that your script class is 'public', implements IScript, and that FlyScript.dll actually rebuilt (check the [ScriptCompiler] build log above for errors).");
            else
                Native.Log($"[FlyScript] Discovered {names.Count} script type(s): {string.Join(", ", names)}");

            string joined = string.Join("\n", names) + "\0";
            byte[] bytes = new System.Text.UTF8Encoding().GetBytes(joined);
            if (bytes.Length > capacity)
            {
                Native.Log($"[FlyScript] GetScriptTypeNames buffer too small ({capacity} bytes, needed {bytes.Length}).");
                return -1;
            }
            System.Runtime.InteropServices.Marshal.Copy(bytes, 0, buffer, bytes.Length);
            return bytes.Length;
        }

        // Evaluate a line of C# from the engine's command bar using Roslyn
        // scripting. `codePtr` is a NUL-terminated UTF-8 buffer; `buffer`
        // receives the result text (or the compiler/runtime error) as UTF-8.
        // Returns 1 on success, 0 on error. The evaluated script has direct
        // access to every FlyScript SDK type (Game, GameObject, Native, Vec3,
        // Tick, ...) plus System/Linq.
        public static int ExecuteConsole(IntPtr hostPtr, IntPtr codePtr, IntPtr buffer, int capacity)
        {
            if (buffer == IntPtr.Zero || capacity <= 0) return 0;
            string code = codePtr == IntPtr.Zero ? "" : (Marshal.PtrToStringUTF8(codePtr) ?? "");

            try
            {
                var options = ScriptOptions.Default
                    .WithReferences(LoadedReferences())
                    .WithImports("FlyScript", "System", "System.Collections.Generic", "System.Linq");

                object? result = CSharpScript.EvaluateAsync(code, options).GetAwaiter().GetResult();
                string text = result == null ? "OK" : (result.ToString() ?? "OK");
                WriteUtf8(buffer, capacity, text);
                return 1;
            }
            catch (CompilationErrorException cex)
            {
                WriteUtf8(buffer, capacity,
                    "Error: " + string.Join("\n", cex.Diagnostics.Select(d => d.ToString())));
                return 0;
            }
            catch (Exception ex)
            {
                WriteUtf8(buffer, capacity, "Error: " + ex.Message);
                return 0;
            }
        }

        // Deduplicated metadata references for every loaded, file-backed assembly,
        // so Roslyn scripts can use the SDK types (and anything else in the host).
        private static List<MetadataReference> LoadedReferences()
        {
            var refs = new List<MetadataReference>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (asm.IsDynamic) continue;
                string? loc = asm.Location;
                if (string.IsNullOrEmpty(loc)) continue;
                if (!seen.Add(loc)) continue;
                refs.Add(MetadataReference.CreateFromFile(loc));
            }
            return refs;
        }

        private static void WriteUtf8(IntPtr buffer, int capacity, string text)
        {
            byte[] bytes = new System.Text.UTF8Encoding().GetBytes(text ?? string.Empty);
            if (bytes.Length > capacity - 1) Array.Resize(ref bytes, capacity - 1);
            Marshal.Copy(bytes, 0, buffer, bytes.Length);
            Marshal.WriteByte(buffer, bytes.Length, 0); // NUL-terminate
        }

        // Find a script by target object (for per-object scripts).
        public static int FindScriptForObject(Id64 handle)
        {
            foreach (var kv in s_scripts)
                if (kv.Value.Target.Handle == handle) return kv.Key;
            return 0;
        }

        // Resolve a user-written IScript type by name across the loadable
        // assemblies, so short and fully-qualified names both work.
        public static Type? ResolveType(string name)
        {
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                Type? t = asm.GetType(name);
                if (t != null && typeof(IScript).IsAssignableFrom(t)) return t;
            }
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                foreach (var t in SafeTypes(asm))
                {
                    if (t.Name == name || t.FullName == name)
                        if (typeof(IScript).IsAssignableFrom(t)) return t;
                }
            }
            return null;
        }

        private static IEnumerable<Type> SafeTypes(System.Reflection.Assembly asm)
        {
            try { return asm.GetTypes(); }
            catch { return Array.Empty<Type>(); }
        }
    }

    // ---- Per-object script API (replaces Flyscript's StartScript/StopScript) ----
    // These are called from C++ when the user clicks "Run" / "Stop" in the editor.
    public static class PerObjectScripts
    {
        // Start a script on a specific object from a IScript type name.
        // Returns script ID (>0) or 0 on error.
        public static int Start(Id64 handle, string typeName)
        {
            if (handle == Id64.None) return 0;
            try
            {
                Type? t = ScriptHost.ResolveType(typeName);
                if (t == null || !typeof(IScript).IsAssignableFrom(t))
                {
                    Native.Log($"[FlyScript] Could not start object script '{typeName}' on {handle}: no public IScript type with that name was found in FlyScript.dll.");
                    return 0;
                }
                IScript script = (IScript)Activator.CreateInstance(t)!;
                var obj = new GameObject(handle);
                return ScriptHost.RegisterScript(obj, script);
            }
            catch (Exception ex)
            {
                ScriptDiagnostics.ReportFailure($"Starting object script '{typeName}' on {handle}", ex);
                return 0;
            }
        }

        public static void Stop(Id64 handle)
        {
            int id = ScriptHost.FindScriptForObject(handle);
            if (id > 0) ScriptHost.UnregisterScript(id);
        }

        public static bool IsRunning(Id64 handle)
            => ScriptHost.FindScriptForObject(handle) > 0;

        public static void ClearAll()
        {
            // Per-object scripts are unregistered when play stops via
            // ScriptHost.ClearAll; nothing extra is required here.
        }
    }

    public static class StandaloneScripts
    {
        // Same as PerObjectScripts but for the explorer's "Scripts" group (index-based).
        private static readonly Dictionary<int, int> s_indexToScriptId = new Dictionary<int, int>();

        public static int Start(int index, string typeName)
        {
            try
            {
                if (string.IsNullOrEmpty(typeName))
                {
                    Native.Log($"[FlyScript] Could not start standalone script at index {index}: empty type name.");
                    return 0;
                }
                Type? t = ScriptHost.ResolveType(typeName);
                if (t == null || !typeof(IScript).IsAssignableFrom(t))
                {
                    Native.Log($"[FlyScript] Could not start standalone script '{typeName}' (index {index}): no public IScript type with that name was found in FlyScript.dll.");
                    return 0;
                }

                IScript script = (IScript)Activator.CreateInstance(t)!;
                int id = ScriptHost.RegisterScript(GameObject.None, script);
                if (id > 0) s_indexToScriptId[index] = id;
                return id;
            }
            catch (Exception ex)
            {
                ScriptDiagnostics.ReportFailure($"Starting standalone script '{typeName}' (index {index})", ex);
                return 0;
            }
        }

        public static void Stop(int index)
        {
            if (s_indexToScriptId.TryGetValue(index, out int id))
            {
                ScriptHost.UnregisterScript(id);
                s_indexToScriptId.Remove(index);
            }
        }

        public static bool IsRunning(int index)
            => s_indexToScriptId.ContainsKey(index);

        public static void ClearAll()
        {
            foreach (var kv in s_indexToScriptId)
                ScriptHost.UnregisterScript(kv.Value);
            s_indexToScriptId.Clear();
        }
    }
}