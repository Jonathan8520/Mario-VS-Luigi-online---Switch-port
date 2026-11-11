// NxPortBuild.cs -- headless Android build used by the Switch port CI.
//
// Copied into Assets/NxPort/Editor/ of a fresh checkout of the official
// NSMB-MarioVsLuigi tag by unity/prepare_project.py. Nothing here is game code:
// it only chooses the player settings the Switch wrapper (mvlo_nx) can service.
//
// Why each setting:
//   IL2CPP + ARM64 only     the wrapper loads arm64-v8a .so files.
//   GLES3 only (no Vulkan)  switch-mesa provides EGL + GLES; there is no Vulkan.
//   Activity entry point    the wrapper drives the classic UnityPlayer JNI
//                           natives (initJni/nativeRender...), not GameActivity.
//   No MT rendering         the render thread would submit presents outside the
//                           wrapper's frame pacing (see cloverpit_nx notes).
//   No Swappy               frame pacing is done by the wrapper.
//   ETC2 textures           mandatory in GLES3, always decodable by mesa.
//   Symbols (Public)        libil2cpp.sym.so is what tools/re/derive_il2cpp.py
//                           reads to locate the Boehm GC globals.
//   Single APK, no AAB      the CI unzips lib/ and assets/ out of it.
//
// MIT -- part of mvlo_nx.
#if UNITY_EDITOR
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;
using UnityEngine.Rendering;

public static class NxPortBuild {
    static string Arg(string name, string fallback) {
        string[] args = Environment.GetCommandLineArgs();
        for (int i = 0; i < args.Length - 1; i++) {
            if (args[i] == "-" + name) return args[i + 1];
        }
        return fallback;
    }

    public static void BuildAndroid() {
        string outPath = Arg("customBuildPath", "Builds/Android/mvlo.apk");
        if (!outPath.EndsWith(".apk")) outPath = Path.Combine(outPath, "mvlo.apk");
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath)));

        var nt = NamedBuildTarget.Android;
        EditorUserBuildSettings.SwitchActiveBuildTarget(BuildTargetGroup.Android, BuildTarget.Android);

        PlayerSettings.SetScriptingBackend(nt, ScriptingImplementation.IL2CPP);
        PlayerSettings.SetIl2CppCompilerConfiguration(nt, Il2CppCompilerConfiguration.Release);
        PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, new[] { GraphicsDeviceType.OpenGLES3 });
        try { PlayerSettings.SetMobileMTRendering(nt, false); } catch (Exception e) { Debug.LogWarning("[NxPort] MT rendering: " + e.Message); }
        PlayerSettings.Android.optimizedFramePacing = false;
        PlayerSettings.Android.applicationEntry = AndroidApplicationEntry.Activity;
        PlayerSettings.Android.splitApplicationBinary = false;
        PlayerSettings.Android.minifyRelease = false;
        PlayerSettings.Android.minifyDebug = false;
        EditorUserBuildSettings.buildAppBundle = false;
        EditorUserBuildSettings.exportAsGoogleAndroidProject = false;
        EditorUserBuildSettings.androidBuildSubtarget = MobileTextureSubtarget.ETC2;
        EditorUserBuildSettings.androidCreateSymbols = AndroidCreateSymbols.Public;
        EditorUserBuildSettings.development = false;

        Debug.Log("[NxPort] backend=" + PlayerSettings.GetScriptingBackend(nt)
                  + " arch=" + PlayerSettings.Android.targetArchitectures
                  + " gfx=" + string.Join(",", PlayerSettings.GetGraphicsAPIs(BuildTarget.Android))
                  + " entry=" + PlayerSettings.Android.applicationEntry
                  + " unity=" + Application.unityVersion);

        var options = new BuildPlayerOptions {
            scenes = EditorBuildSettings.scenes.Where(s => s.enabled).Select(s => s.path).ToArray(),
            target = BuildTarget.Android,
            targetGroup = BuildTargetGroup.Android,
            locationPathName = outPath,
            options = BuildOptions.None,
        };
        BuildReport report = BuildPipeline.BuildPlayer(options);
        BuildSummary s = report.summary;
        Console.WriteLine("[NxPort] result=" + s.result + " errors=" + s.totalErrors
                          + " size=" + s.totalSize + " time=" + s.totalTime);
        EditorApplication.Exit(s.result == BuildResult.Succeeded ? 0 : 101);
    }
}
#endif
