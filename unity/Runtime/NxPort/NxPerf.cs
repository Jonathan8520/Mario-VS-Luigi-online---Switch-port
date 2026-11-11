// NxPerf.cs -- engine-wide settings that only matter for speed on the Switch.
//
// MIT -- part of mvlo_nx.

using System.Text.RegularExpressions;
using UnityEngine;

namespace NxPort {

    /// <summary>
    /// Engine-wide settings that only cost time on the Switch.
    /// </summary>
    public static class NxPerf {

        /// <summary>
        /// ChatFilter.FilterString (every chat line, including "X joined / left
        /// the room") runs 53 patterns through the static Regex.Match, whose
        /// cache keeps 15 compiled patterns: each call parsed and compiled ~40 of
        /// them again -- 127-245 ms on UnityMain at every join or leave in a
        /// match (02/11 profiles: RegexParser, RegexWriter). 128 keeps them all
        /// after their first use, which is the host's own "joined" line.
        /// </summary>
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void Init() {
            if (Regex.CacheSize < 128) {
                Regex.CacheSize = 128;
            }
            Debug.Log("[NxPort] Regex.CacheSize = " + Regex.CacheSize);
        }

        /// <summary>
        /// Creating the first room froze the game for 0.8 + 1.0 s at "Starting
        /// Quantum Runner" (02/11 profiles), the second one for 0.45 s only
        /// (03/11): IL2CPP filling its metadata caches on first use -- the
        /// name-to-class tables of each assembly, reflection types, custom
        /// attributes, generic methods -- as QuantumGame registers the
        /// simulation types. Those caches are process-wide, so a low-priority
        /// thread does the same once while the player is still on the title
        /// screen. [game] warmup = 0 turns it off.
        /// </summary>
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void WarmUp() {
            if (!NxBridge.IsSwitch || !NxConfig.GetBool("game.warmup", true)) {
                return;
            }
            var thread = new System.Threading.Thread(WarmUpTypes) {
                IsBackground = true,
                Priority = System.Threading.ThreadPriority.BelowNormal,
                Name = "NxPort warm-up",
            };
            thread.Start();
        }

        private static void WarmUpTypes() {
            // ~25 s into the boot, on the title screen: the 03/11 run took
            // 96 ms here, and a room can't be created before ~45 s.
            System.Threading.Thread.Sleep(15000);
            var watch = System.Diagnostics.Stopwatch.StartNew();
            int types = 0, failed = 0;
            System.Type registryType = null, staticsType = null;
            foreach (var asm in System.AppDomain.CurrentDomain.GetAssemblies()) {
                // A lookup by name builds the assembly's name-to-class table
                // (13% of the 915 ms room-creation frame).
                try {
                    asm.GetType("NxPort.NxPerf+NoSuchType");
                    registryType = registryType ?? asm.GetType("Quantum.TypeRegistry");
                    staticsType = staticsType ?? asm.GetType("Quantum.Statics");
                } catch {
                    failed++;
                }
                string name = asm.GetName().Name;
                if (!(name.StartsWith("Quantum") || name.StartsWith("Photon") || name.StartsWith("Assembly-CSharp"))) {
                    continue;
                }
                System.Type[] all;
                try {
                    all = asm.GetTypes();
                } catch (System.Reflection.ReflectionTypeLoadException e) {
                    all = e.Types;
                }
                foreach (var type in all) {
                    if (type == null) {
                        continue;
                    }
                    try {
                        type.GetCustomAttributes(false);
                        types++;
                    } catch {
                        failed++;
                    }
                }
            }
            long typesMs = watch.ElapsedMilliseconds;
            // What QuantumGame's constructor does at every room creation, on a
            // throwaway registry (by reflection: no compile-time dependency on
            // Quantum's assemblies).
            string registry = "not found";
            if (registryType != null && staticsType != null) {
                try {
                    var register = staticsType.GetMethod("RegisterSimulationTypes",
                        System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                    register.Invoke(null, new[] { System.Activator.CreateInstance(registryType) });
                    registry = "registered";
                } catch (System.Exception e) {
                    registry = e.GetType().Name + ": " + e.Message;
                }
            }
            Debug.Log($"[NxPort] warm-up: {types} types ({failed} failed) in {typesMs} ms, "
                + $"type registry {registry} by {watch.ElapsedMilliseconds} ms");
        }
    }
}
