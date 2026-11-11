// NxBridge.cs -- managed side of the mvlo_nx <-> game bridge.
//
// On the Switch, the wrapper (mvlo_nx) answers dlopen("libnxbridge.so") and
// dlsym("nxbridge_*") from its own shim table (source/nx_bridge.c), so these
// P/Invokes land in native wrapper code. On any other platform the library does
// not exist: the first call throws DllNotFoundException / EntryPointNotFound,
// which IsSwitch catches once, and every feature below turns itself off.
//
// MIT -- part of mvlo_nx. This file is NOT part of the original game; the CI
// copies it into Assets/NxPort/ of the official sources at build time.
using System;
using System.Runtime.InteropServices;
using System.Text;
using UnityEngine;

namespace NxPort {
    [StructLayout(LayoutKind.Sequential)]
    public struct NxPadState {
        public uint buttons;     // libnx HidNpadButton bits
        public int connected;    // 0/1
        public float lx, ly, rx, ry;  // -1..1, +y = up
        public int style;        // HidNpadStyleTag bits
        public int reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NxTouch {
        public int id;           // stable finger id from hid
        public float x, y;       // 0..1, origin TOP-left
        public int reserved;
    }

    public static class NxBridge {
        const string Lib = "nxbridge";

        [DllImport(Lib)] static extern int nxbridge_version();
        [DllImport(Lib)] static extern int nxbridge_get_config(byte[] buf, int cap);
        [DllImport(Lib)] static extern void nxbridge_log(byte[] utf8z);
        [DllImport(Lib)] static extern int nxbridge_poll_pads([Out] NxPadState[] pads, int max);
        [DllImport(Lib)] static extern int nxbridge_poll_touch([Out] NxTouch[] touches, int max);
        [DllImport(Lib)] static extern void nxbridge_rumble(int pad, float low, float high);
        [DllImport(Lib)] static extern int nxbridge_kbd_closed();
        static bool s_noKbdEvent;

        static int s_state; // 0 unknown, 1 switch, -1 not switch

        public static bool IsSwitch {
            get {
                if (s_state == 0) {
                    try {
                        int v = nxbridge_version();
                        s_state = v > 0 ? 1 : -1;
                        if (s_state > 0) Debug.Log("[NxPort] native bridge v" + v + " present -- running on Switch");
                    } catch (Exception e) {
                        s_state = -1;
                        Debug.Log("[NxPort] native bridge absent (" + e.GetType().Name + ") -- Switch features off");
                    }
                }
                return s_state > 0;
            }
        }

        public static string GetConfigText() {
            if (!IsSwitch) return null;
            byte[] buf = new byte[16384];
            int n = nxbridge_get_config(buf, buf.Length);
            if (n <= 0) return "";
            return Encoding.UTF8.GetString(buf, 0, Math.Min(n, buf.Length));
        }

        public static void Log(string msg) {
            if (!IsSwitch || msg == null) return;
            byte[] b = Encoding.UTF8.GetBytes(msg + "\0");
            nxbridge_log(b);
        }

        public static int PollPads(NxPadState[] pads) => IsSwitch ? nxbridge_poll_pads(pads, pads.Length) : 0;
        public static int PollTouch(NxTouch[] t) => IsSwitch ? nxbridge_poll_touch(t, t.Length) : 0;
        public static void Rumble(int pad, float low, float high) { if (IsSwitch) nxbridge_rumble(pad, low, high); }

        // True once after each Switch keyboard (swkbd) result. An older .nro
        // without the export just never reports it.
        public static bool TakeKeyboardClosed() {
            if (!IsSwitch || s_noKbdEvent) return false;
            try { return nxbridge_kbd_closed() != 0; }
            catch (Exception) { s_noKbdEvent = true; return false; }
        }
    }
}
