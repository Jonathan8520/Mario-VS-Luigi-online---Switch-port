#!/usr/bin/env python3
"""prepare_project.py -- turn a pristine checkout of the official
NSMB-MarioVsLuigi tag into the Android build the Switch wrapper expects.

    python3 unity/prepare_project.py <path-to-game-checkout>

It does three things and nothing else:
  1. copies this repo's unity/Editor and unity/Runtime into Assets/NxPort/;
  2. applies a handful of small, textual patches to the game's own scripts
     (each must match EXACTLY once, otherwise the script fails -- a silently
     skipped patch is worse than a red CI run);
  3. removes the official Photon AppId from the project, and verifies it no
     longer appears anywhere under Assets/.

No game file is ever copied into this repository; the patches below are the
whole diff.  MIT -- part of mvlo_nx.
"""
import hashlib
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
OFFICIAL_APPID_SHA256 = "6620c1452bbdfd47779641f1a56185c361e08a97f03673fa9c48a604528db8c6"


def fail(msg):
    print("prepare_project: FATAL: " + msg, file=sys.stderr)
    sys.exit(1)


def patch(root, rel, pattern, repl, regex=False, count=1):
    path = os.path.join(root, rel)
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read()
    if "\r\n" in src:
        repl = repl.replace("\n", "\r\n")
    if regex:
        new, n = re.subn(pattern, repl, src)
    else:
        n = src.count(pattern)
        new = src.replace(pattern, repl)
    if n != count:
        fail("%s: expected %d match(es) for %r, found %d" % (rel, count, pattern, n))
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(new)
    print("patched %-60s (%d)" % (rel, n))


def main():
    if len(sys.argv) != 2:
        fail("usage: prepare_project.py <game checkout>")
    root = sys.argv[1]
    assets = os.path.join(root, "Assets")
    if not os.path.isdir(assets):
        fail("no Assets/ in " + root)

    # 1. our sources
    dst = os.path.join(assets, "NxPort")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(os.path.join(HERE, "Runtime", "NxPort"), dst)
    shutil.copytree(os.path.join(HERE, "Editor"), os.path.join(dst, "Editor"))
    print("copied unity/Runtime + unity/Editor -> Assets/NxPort")

    # 2. game patches
    net = "Assets/Scripts/Networking/NetworkHandler.cs"
    patch(root, net, r'AppIdQuantum = "' + GUID.pattern + r'",',
          "AppIdQuantum = NxPort.NxConfig.AppIdQuantum,", regex=True)
    patch(root, net, "FixedRegion = region,",
          "FixedRegion = NxPort.NxConfig.FixedRegion(region),")
    patch(root, net, "AppVersion = GameVersion.Current.ToStringIgnoreHotfix() + buildIdentifier,",
          "AppVersion = GameVersion.Current.ToStringIgnoreHotfix() + NxPort.NxConfig.BuildIdentifier(buildIdentifier),")

    auth = "Assets/Scripts/Networking/AuthenticationHandler.cs"
    patch(root, auth, "public static async Task<AuthenticationValues> Authenticate() {",
          "public static async Task<AuthenticationValues> Authenticate() {\n"
          "            if (NxPort.NxConfig.SkipOfficialAuth) { return NxPort.NxConfig.CreateAuthValues(); }")

    upd = "Assets/Scripts/Networking/UpdateChecker.cs"
    patch(root, upd, "public async static void CheckIfUpToDate(UpdateCallback callback) {",
          "public async static void CheckIfUpToDate(UpdateCallback callback) {\n"
          "            if (NxPort.NxConfig.SkipUpdateCheck) { return; }")

    # Input handlers that outlive their scene. OnScreenKeyboard subscribes to
    # UI/Navigate while open and only unsubscribes in Close(): left open when a
    # match starts (common after the Switch keyboard), the destroyed keyboard
    # keeps receiving Navigate 'canceled' -> NullReferenceException. Same for
    # the pause menu destroyed while paused. Unsubscribing twice is harmless.
    osk = "Assets/Scripts/UI/MainMenu/OnScreenKeyboard.cs"
    patch(root, osk, "Settings.Controls.UI.Cancel.performed -= OnCancel;",
          "Settings.Controls.UI.Cancel.performed -= OnCancel; "
          "Settings.Controls.UI.Navigate.performed -= OnNavigate; Settings.Controls.UI.Navigate.canceled -= OnNavigate;")
    pause = "Assets/Scripts/UI/Pause/PauseMenuManager.cs"
    patch(root, pause, "Settings.Controls.UI.Pause.performed -= OnPause;",
          "Settings.Controls.UI.Pause.performed -= OnPause; "
          "Settings.Controls.UI.Navigate.performed -= OnNavigate; Settings.Controls.UI.Navigate.canceled -= OnNavigate; "
          "Settings.Controls.UI.Submit.performed -= OnSubmit; Settings.Controls.UI.Cancel.performed -= OnCancel;")

    # Quantum re-bakes every scene's map data inside BuildPlayer
    # (AutoBuildOnBuild). The baked data is committed upstream (it is also
    # baked on every scene save), and the first CI run died silently right in
    # that step. Off by default; NXPORT_QUANTUM_BAKE=1 restores it.
    if os.environ.get("NXPORT_QUANTUM_BAKE") != "1":
        patch(root, "Assets/QuantumUser/Editor/QuantumEditorSettings.asset",
              r"AutoBuildOnBuild: *\d+", "AutoBuildOnBuild: 0", regex=True)

    # 3. AppId removal
    pss = "Assets/QuantumUser/Resources/PhotonServerSettings.asset"
    patch(root, pss, r"AppIdQuantum: *" + GUID.pattern, "AppIdQuantum: ", regex=True)

    # The official AppId is known here only by its SHA-256, so it never
    # appears in this repository: every GUID in the project is hashed and
    # compared.
    for d, _, files in os.walk(assets):
        for fn in files:
            if fn.endswith((".cs", ".asset", ".json", ".txt", ".prefab", ".unity")):
                p = os.path.join(d, fn)
                try:
                    with open(p, encoding="utf-8", errors="ignore") as f:
                        found = set(GUID.findall(f.read().lower()))
                except OSError:
                    continue
                if any(hashlib.sha256(g.encode()).hexdigest() == OFFICIAL_APPID_SHA256 for g in found):
                    fail("official AppId still present in " + p)
    print("official Photon AppId removed from the project")


if __name__ == "__main__":
    main()
