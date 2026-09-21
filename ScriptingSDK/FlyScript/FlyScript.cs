// FlyScript SDK — C# wrapper around the engine's FlyNative_* C API.
// This assembly is compiled by the engine at runtime when a project is opened.
// Game scripts reference this as a project reference and call its APIs.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace FlyScript
{
    // Opaque handle for scene objects (maps to C++ id64).
    public readonly struct Id64 : IEquatable<Id64>
    {
        public readonly ulong Value;
        public Id64(ulong v) => Value = v;
        public static readonly Id64 None = new Id64(0);
        public static implicit operator ulong(Id64 h) => h.Value;
        public static implicit operator Id64(ulong v) => new Id64(v);
        public bool Equals(Id64 other) => Value == other.Value;
        public override bool Equals(object? obj) => obj is Id64 other && Equals(other);
        public override int GetHashCode() => Value.GetHashCode();
        public static bool operator ==(Id64 a, Id64 b) => a.Value == b.Value;
        public static bool operator !=(Id64 a, Id64 b) => a.Value != b.Value;
        public override string ToString() => Value == 0 ? "Id64.None" : $"Id64(0x{Value:X})";
    }

    // Collision fidelity enum (must match C++ pcoll::CollisionAccuracy).
    public enum CollisionAccuracy : int
    {
        Box = 0,
        Hull = 1,
        Default = 2,
        Precise = 3,
    }

    // The C#-side API. All methods P/Invoke into Flyengine.exe exports.
    public static partial class Native
    {
        // These P/Invoke calls target "Flyengine" — but the exports actually live
        // in Flyengine.exe itself (CoreCLR is embedded in the exe, there is no
        // separate Flyengine.dll). On Windows, LoadLibrary("Flyengine") silently
        // appends ".dll" and looks for a file that doesn't exist, so without this
        // resolver every call below throws DllNotFoundException — swallowed by
        // ScriptHost.Run's catch block and only visible in an attached debugger's
        // Output window, never in the in-editor console. Redirect "Flyengine" to
        // the already-loaded main program module so GetProcAddress can find the
        // dllexport'd FlyNative_* symbols.
        [ModuleInitializer]
        internal static void RegisterResolver()
        {
            NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, assembly, searchPath) =>
            {
                if (name == "Flyengine") return NativeLibrary.GetMainProgramHandle();
                return IntPtr.Zero;
            });
        }

        // ---- Runtime binding ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_BindRuntime")]
        private static partial void BindRuntimeImpl(IntPtr runtime);

        public static void BindRuntime(IntPtr runtimePtr) => BindRuntimeImpl(runtimePtr);

        // ---- Object registry ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_FindObject", StringMarshalling = StringMarshalling.Utf8)]
        private static partial ulong FindObject_Impl(string name);

        public static Id64 FindObject(string name)
        {
            if (string.IsNullOrEmpty(name)) return Id64.None;
            return FindObject_Impl(name);
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetSelf")]
        private static partial ulong GetSelf();

        public static Id64 Self => GetSelf();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_BindSelf")]
        private static partial void BindSelfImpl(ulong handle);

        // The C# host calls this around each per-object script coroutine tick so
        // GameObject.Self resolves to the object the script is attached to.
        public static void BindSelf(Id64 handle) => BindSelfImpl(handle.Value);

        // ---- Vec3 properties ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetPosition")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetPosition(ulong h, out float x, out float y, out float z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetPosition")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetPosition(ulong h, float x, float y, float z);

        public static bool TryGetPosition(Id64 h, out float x, out float y, out float z)
            => GetPosition(h.Value, out x, out y, out z) != 0;

        public static void SetPosition(Id64 h, float x, float y, float z)
            => SetPosition(h.Value, x, y, z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetSize")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetSize(ulong h, out float x, out float y, out float z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetSize")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetSize(ulong h, float x, float y, float z);

        public static bool TryGetSize(Id64 h, out float x, out float y, out float z)
            => GetSize(h.Value, out x, out y, out z) != 0;

        public static void SetSize(Id64 h, float x, float y, float z)
            => SetSize(h.Value, x, y, z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetRotation")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetRotation(ulong h, out float x, out float y, out float z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetRotation")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetRotation(ulong h, float x, float y, float z);

        public static bool TryGetRotation(Id64 h, out float x, out float y, out float z)
            => GetRotation(h.Value, out x, out y, out z) != 0;

        public static void SetRotation(Id64 h, float x, float y, float z)
            => SetRotation(h.Value, x, y, z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetOrigin")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetOrigin(ulong h, out float x, out float y, out float z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetOrigin")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetOrigin(ulong h, float x, float y, float z);

        public static bool TryGetOrigin(Id64 h, out float x, out float y, out float z)
            => GetOrigin(h.Value, out x, out y, out z) != 0;

        public static void SetOrigin(Id64 h, float x, float y, float z)
            => SetOrigin(h.Value, x, y, z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetVelocity")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetVelocity(ulong h, out float x, out float y, out float z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetVelocity")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetVelocity(ulong h, float x, float y, float z);

        public static bool TryGetVelocity(Id64 h, out float x, out float y, out float z)
            => GetVelocity(h.Value, out x, out y, out z) != 0;

        public static void SetVelocity(Id64 h, float x, float y, float z)
            => SetVelocity(h.Value, x, y, z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetAngularVelocity")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetAngularVelocity(ulong h, out float x, out float y, out float z);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetAngularVelocity")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetAngularVelocity(ulong h, float x, float y, float z);

        public static bool TryGetAngularVelocity(Id64 h, out float x, out float y, out float z)
            => GetAngularVelocity(h.Value, out x, out y, out z) != 0;

        public static void SetAngularVelocity(Id64 h, float x, float y, float z)
            => SetAngularVelocity(h.Value, x, y, z);

        // ---- Color (r,g,b 0..255) ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetColor")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetColor(ulong h, out byte r, out byte g, out byte b);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetColor")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short SetColor(ulong h, byte r, byte g, byte b);

        public static bool TryGetColor(Id64 h, out byte r, out byte g, out byte b)
            => GetColor(h.Value, out r, out g, out b) != 0;

        public static void SetColor(Id64 h, byte r, byte g, byte b)
            => SetColor(h.Value, r, g, b);

        // ---- Boolean / scalar properties ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetAnchored")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetAnchored(ulong h);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetAnchored")]
        private static partial void SetAnchored(ulong h, short v);

        public static bool GetAnchored(Id64 h) => GetAnchored(h.Value) != 0;
        public static void SetAnchored(Id64 h, bool v) => SetAnchored(h.Value, (short)(v ? 1 : 0));

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetCanCollide")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetCanCollide(ulong h);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetCanCollide")]
        private static partial void SetCanCollide(ulong h, short v);

        public static bool GetCanCollide(Id64 h) => GetCanCollide(h.Value) != 0;
        public static void SetCanCollide(Id64 h, bool v) => SetCanCollide(h.Value, (short)(v ? 1 : 0));

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetMass")]
        private static partial float GetMass(ulong h);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetMass")]
        private static partial void SetMass(ulong h, float mass);

        public static float Mass(Id64 h) => GetMass(h.Value);
        public static void SetMass(Id64 h, float m) => SetMass(h.Value, m);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetTransparency")]
        private static partial float GetTransparency(ulong h);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetTransparency")]
        private static partial void SetTransparency(ulong h, float t);

        public static float Transparency(Id64 h) => GetTransparency(h.Value);
        public static void SetTransparency(Id64 h, float t) => SetTransparency(h.Value, t);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetCollisionAccuracy")]
        private static partial int GetCollisionAccuracy(ulong h);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetCollisionAccuracy")]
        private static partial void SetCollisionAccuracy(ulong h, int accuracy);

        public static CollisionAccuracy GetCollisionAccuracy(Id64 h)
            => (CollisionAccuracy)GetCollisionAccuracy(h.Value);

        public static void SetCollisionAccuracy(Id64 h, CollisionAccuracy a)
            => SetCollisionAccuracy(h.Value, (int)a);

        // ---- World services ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_IsPlaying")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short IsPlaying();

        public static bool Playing => IsPlaying() != 0;

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetShadowsEnabled")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetShadowsEnabled();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetShadowsEnabled")]
        private static partial void SetShadowsEnabled(short on);

        public static bool ShadowsEnabled
        {
            get => GetShadowsEnabled() != 0;
            set => SetShadowsEnabled((short)(value ? 1 : 0));
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetShadowQuality")]
        private static partial int GetShadowQuality();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetShadowQuality")]
        private static partial void SetShadowQuality(int q);

        public static int ShadowQuality
        {
            get => GetShadowQuality();
            set => SetShadowQuality(value);
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetAmbient")]
        private static partial float GetAmbient(out float intensity);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetAmbient")]
        private static partial void SetAmbient(float a);

        public static float Ambient
        {
            get { float v = 0; GetAmbient(out v); return v; }
            set => SetAmbient(value);
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetGridVisible")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetGridVisible();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetGridVisible")]
        private static partial void SetGridVisible(short on);

        public static bool GridVisible
        {
            get => GetGridVisible() != 0;
            set => SetGridVisible((short)(value ? 1 : 0));
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetWireframe")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short GetWireframe();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetWireframe")]
        private static partial void SetWireframe(short on);

        public static bool Wireframe
        {
            get => GetWireframe() != 0;
            set => SetWireframe((short)(value ? 1 : 0));
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetFov")]
        private static partial float GetFov(out float fov);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetFov")]
        private static partial void SetFov(float f);

        public static float Fov
        {
            get { float v = 0; GetFov(out v); return v; }
            set => SetFov(value);
        }

        // Physics (game.Physics.*)
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetGravity")]
        private static partial float GetGravity();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetGravity")]
        private static partial void SetGravity(float g);

        public static float Gravity
        {
            get => GetGravity();
            set => SetGravity(value);
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetFriction")]
        private static partial float GetFriction();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetFriction")]
        private static partial void SetFriction(float f);

        public static float Friction
        {
            get => GetFriction();
            set => SetFriction(value);
        }

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_GetRestitution")]
        private static partial float GetRestitution();

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_SetRestitution")]
        private static partial void SetRestitution(float r);

        public static float Restitution
        {
            get => GetRestitution();
            set => SetRestitution(value);
        }

        // ---- Object creation ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_CreateObject", StringMarshalling = StringMarshalling.Utf8)]
        private static partial ulong CreateObject_Impl(string shapeName);

        public static Id64 CreateObject(string shapeName)
            => CreateObject_Impl(shapeName ?? "Cube");

        // ---- Script lifecycle (editor calls these directly via P/Invoke) ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_StartObjectScript", StringMarshalling = StringMarshalling.Utf8)]
        private static partial ulong StartObjectScript_Impl(ulong objectHandle, string typeName);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_StopObjectScript")]
        private static partial void StopObjectScript_Impl(ulong objectHandle);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_IsObjectScriptRunning")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short IsObjectScriptRunning_Impl(ulong objectHandle);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_StartStandaloneScript", StringMarshalling = StringMarshalling.Utf8)]
        private static partial ulong StartStandaloneScript_Impl(int index, string typeName);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_StopStandaloneScript")]
        private static partial void StopStandaloneScript_Impl(int index);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_IsStandaloneScriptRunning")]
        [return: MarshalAs(UnmanagedType.I2)]
        private static partial short IsStandaloneScriptRunning_Impl(int index);

        [LibraryImport("Flyengine", EntryPoint = "FlyNative_ClearAllScripts")]
        private static partial void ClearAllScripts_Impl();

        public static ulong StartObjectScript(Id64 handle, string typeName)
            => StartObjectScript_Impl(handle.Value, typeName);

        public static void StopObjectScript(Id64 handle)
            => StopObjectScript_Impl(handle.Value);

        public static bool IsObjectScriptRunning(Id64 handle)
            => IsObjectScriptRunning_Impl(handle.Value) != 0;

        public static ulong StartStandaloneScript(int index, string typeName)
            => StartStandaloneScript_Impl(index, typeName);

        public static void StopStandaloneScript(int index)
            => StopStandaloneScript_Impl(index);

        public static bool IsStandaloneScriptRunning(int index)
            => IsStandaloneScriptRunning_Impl(index) != 0;

        public static void ClearAllScripts() => ClearAllScripts_Impl();

        // ---- Logging ----
        [LibraryImport("Flyengine", EntryPoint = "FlyNative_Print", StringMarshalling = StringMarshalling.Utf8)]
        private static partial void Print(string text);

        public static void Log(string text) => Print(text);
    }

    // ---- Convenience structs for tuples (x,y,z) ----
    public struct Vec3
    {
        public float X, Y, Z;
        public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }
        public static implicit operator (float X, float Y, float Z)(Vec3 v) => (v.X, v.Y, v.Z);
        public static implicit operator Vec3((float X, float Y, float Z) t) => new Vec3(t.X, t.Y, t.Z);
        public override string ToString() => $"({X}, {Y}, {Z})";
    }

    public struct Color3
    {
        public byte R, G, B;
        public Color3(byte r, byte g, byte b) { R = r; G = g; B = b; }
        public override string ToString() => $"rgb({R}, {G}, {B})";
    }

    // ---- High-level object wrapper (the "game.Workspace.Name" and "self" API) ----
    public readonly struct GameObject
    {
        public readonly Id64 Handle;
        public GameObject(Id64 h) => Handle = h;
        public static GameObject None => new GameObject(Id64.None);
        public bool IsValid => Handle != Id64.None;

        public static GameObject Find(string name) => new GameObject(Native.FindObject(name));

        // "self" — the object this script is attached to. Returns None if not available.
        public static GameObject Self => new GameObject(Native.Self);

        // Properties
        public Vec3 Position
        {
            get { Native.TryGetPosition(Handle, out float x, out float y, out float z); return new Vec3(x, y, z); }
            set => Native.SetPosition(Handle, value.X, value.Y, value.Z);
        }
        public Vec3 Size
        {
            get { Native.TryGetSize(Handle, out float x, out float y, out float z); return new Vec3(x, y, z); }
            set => Native.SetSize(Handle, value.X, value.Y, value.Z);
        }
        public Vec3 Rotation
        {
            get { Native.TryGetRotation(Handle, out float x, out float y, out float z); return new Vec3(x, y, z); }
            set => Native.SetRotation(Handle, value.X, value.Y, value.Z);
        }
        public Vec3 Origin
        {
            get { Native.TryGetOrigin(Handle, out float x, out float y, out float z); return new Vec3(x, y, z); }
            set => Native.SetOrigin(Handle, value.X, value.Y, value.Z);
        }
        public Vec3 Velocity
        {
            get { Native.TryGetVelocity(Handle, out float x, out float y, out float z); return new Vec3(x, y, z); }
            set => Native.SetVelocity(Handle, value.X, value.Y, value.Z);
        }
        public Vec3 AngularVelocity
        {
            get { Native.TryGetAngularVelocity(Handle, out float x, out float y, out float z); return new Vec3(x, y, z); }
            set => Native.SetAngularVelocity(Handle, value.X, value.Y, value.Z);
        }
        public Color3 Color
        {
            get { Native.TryGetColor(Handle, out byte r, out byte g, out byte b); return new Color3(r, g, b); }
            set => Native.SetColor(Handle, value.R, value.G, value.B);
        }
        public bool Anchored
        {
            get => Native.GetAnchored(Handle);
            set => Native.SetAnchored(Handle, value);
        }
        public bool CanCollide
        {
            get => Native.GetCanCollide(Handle);
            set => Native.SetCanCollide(Handle, value);
        }
        public float Mass
        {
            get => Native.Mass(Handle);
            set => Native.SetMass(Handle, value);
        }
        public float Transparency
        {
            get => Native.Transparency(Handle);
            set => Native.SetTransparency(Handle, value);
        }
        public CollisionAccuracy CollisionAccuracy
        {
            get => Native.GetCollisionAccuracy(Handle);
            set => Native.SetCollisionAccuracy(Handle, value);
        }

        // Create a new primitive in the world.
        public static GameObject Create(string shapeName) => new GameObject(Native.CreateObject(shapeName));
    }

    // ---- World services (game.Lighting, game.Rendering, game.Camera, game.Physics) ----
    public static class Game
    {
        public static class Lighting
        {
            public static bool GlobalShadows
            {
                get => Native.ShadowsEnabled;
                set => Native.ShadowsEnabled = value;
            }
            public static int ShadowQuality
            {
                get => Native.ShadowQuality;
                set => Native.ShadowQuality = value;
            }
            public static float Ambient
            {
                get => Native.Ambient;
                set => Native.Ambient = value;
            }
        }
        public static class Rendering
        {
            public static bool Grid
            {
                get => Native.GridVisible;
                set => Native.GridVisible = value;
            }
            public static bool Wireframe
            {
                get => Native.Wireframe;
                set => Native.Wireframe = value;
            }
        }
        public static class Camera
        {
            public static float FOV
            {
                get => Native.Fov;
                set => Native.Fov = value;
            }
        }
        public static class Physics
        {
            public static float Gravity
            {
                get => Native.Gravity;
                set => Native.Gravity = value;
            }
            public static float Friction
            {
                get => Native.Friction;
                set => Native.Friction = value;
            }
            public static float Restitution
            {
                get => Native.Restitution;
                set => Native.Restitution = value;
            }
        }
    }

    // ---- Coroutine / tick support ----
    public readonly struct Tick
    {
        public readonly float Frames;
        private Tick(float f) { Frames = f; }
        public static Tick Wait(float frames = 0f) => new Tick(frames);
        public static Tick WaitForSeconds(float seconds) => new Tick(seconds * 60f);
        public override string ToString() => $"Tick.Wait({Frames})";
    }

    // IScript is what the C# host expects a script class to implement.
    public interface IScript
    {
        // Called once when the script starts. Return an enumerator that yields Tick.Wait().
        IEnumerable<object> Run();
    }
}