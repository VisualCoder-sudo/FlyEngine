// SampleScripts.cs — sample C# scripts for testing the FlyScript SDK.
// Compiled into FlyScript.dll along with the SDK.

using System;
using System.Collections;
using System.Collections.Generic;
using FlyScript;

public class RotateScript : IScript
{
    public IEnumerable<object> Run()
    {
        var self = GameObject.Self;
        if (!self.IsValid)
        {
            Native.Log("[RotateScript] No self object - running as standalone");
            yield break;
        }

        Native.Log($"[RotateScript] Started on {self.Handle}");

        while (true)
        {
            var rot = self.Rotation;
            rot.Y += 45f * ScriptHost.DeltaTime;
            self.Rotation = rot;

            yield return Tick.Wait(0f);
        }
    }
}

public class SpawnerScript : IScript
{
    public IEnumerable<object> Run()
    {
        Native.Log("[SpawnerScript] Started");

        int count = 0;
        while (true)
        {
            yield return Tick.WaitForSeconds(2f);

            var obj = GameObject.Create("Cube");
            if (obj.IsValid)
            {
                obj.Position = new Vec3(0, 3 + count * 2, 0);
                obj.Size = new Vec3(1, 1, 1);
                obj.Color = new Color3(
                    (byte)(count * 50 % 255),
                    (byte)(100 + count * 30 % 155),
                    (byte)(200 + count * 20 % 55)
                );
                obj.Anchored = false;
                Native.Log($"[SpawnerScript] Spawned cube #{count}");
                count++;
            }
        }
    }
}

public class LightPulseScript : IScript
{
    public IEnumerable<object> Run()
    {
        Native.Log("[LightPulseScript] Started - pulsing ambient light");

        float t = 0f;
        while (true)
        {
            float ambient = 0.6f + 0.4f * MathF.Sin(t);
            Game.Lighting.Ambient = ambient;

            t += 0.05f;
            yield return Tick.Wait(0f);
        }
    }
}

public class PhysicsTestScript : IScript
{
    public IEnumerable<object> Run()
    {
        var self = GameObject.Self;
        if (!self.IsValid) yield break;

        Native.Log("[PhysicsTestScript] Applying upward impulse");

        self.Velocity = new Vec3(0, 15, 0);
        self.Anchored = false;

        while (true)
        {
            var pos = self.Position;
            if (pos.Y < 0.5f)
            {
                Native.Log("[PhysicsTestScript] Object hit ground");
                break;
            }
            yield return Tick.Wait(0f);
        }
    }
}