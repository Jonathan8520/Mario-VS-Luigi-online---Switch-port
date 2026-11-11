// NxInput.cs -- feed Switch controllers and the touchscreen into Unity's
// Input System as ordinary virtual devices.
//
// The game uses the new Input System (Controls.inputactions binds <Gamepad>/...
// and <Touchscreen>/...). Rather than emulate Android InputEvents at the native
// layer, the wrapper exposes raw HID state through nxbridge_poll_pads/_touch and
// this component turns it into Gamepad and Touchscreen state events once per
// input update. Nothing in the game has to know it is running on a Switch.
//
// Button layout ([input] ab_layout in mvlo.ini):
//   nintendo (default)  by LABEL: A = confirm (South), B = back (East),
//                       X = West, Y = North -- each button shows its own
//                       letter in the game's bindings, as Switch players expect.
//   xbox                by POSITION: B (bottom) = South, A (right) = East,
//                       Y (left) = West, X (top) = North.
//
// MIT -- part of mvlo_nx. Not part of the original game.
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.InputSystem;
using UnityEngine.InputSystem.Layouts;
using UnityEngine.InputSystem.LowLevel;

namespace NxPort {
    // A Gamepad whose rumble goes to the Joy-Con HD rumble via the bridge.
    [UnityEngine.Scripting.Preserve]
    [InputControlLayout(displayName = "Nintendo Switch Controller (mvlo_nx)", stateType = typeof(GamepadState))]
    public class NxGamepad : Gamepad {
        [UnityEngine.Scripting.Preserve] public NxGamepad() { }
        public int nxIndex;
        public override void SetMotorSpeeds(float lowFrequency, float highFrequency) {
            NxBridge.Rumble(nxIndex, lowFrequency, highFrequency);
        }
        public override void PauseHaptics() { NxBridge.Rumble(nxIndex, 0, 0); }
        public override void ResetHaptics() { NxBridge.Rumble(nxIndex, 0, 0); }
    }

    public class NxInput : MonoBehaviour {
        // libnx HidNpadButton bits
        const uint A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3;
        const uint StickL = 1u << 4, StickR = 1u << 5, L = 1u << 6, R = 1u << 7;
        const uint ZL = 1u << 8, ZR = 1u << 9, Plus = 1u << 10, Minus = 1u << 11;
        const uint Left = 1u << 12, Up = 1u << 13, Right = 1u << 14, Down = 1u << 15;

        const int MaxPads = 4, MaxTouch = 10;

        readonly NxPadState[] pads = new NxPadState[MaxPads];
        readonly Gamepad[] devices = new Gamepad[MaxPads];
        bool customLayout = true;
        readonly NxTouch[] touches = new NxTouch[MaxTouch];
        readonly Dictionary<int, Vector2> activeTouches = new Dictionary<int, Vector2>();
        readonly List<int> ended = new List<int>();
        Touchscreen touchscreen;
        bool nintendoLayout = true, touchEnabled = true;
        static bool s_started;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        static void Bootstrap() {
            if (s_started || !NxBridge.IsSwitch) return;
            s_started = true;
            var go = new GameObject("[NxPort Input]");
            DontDestroyOnLoad(go);
            go.AddComponent<NxInput>();
        }

        void Awake() {
            nintendoLayout = !NxConfig.Get("input.ab_layout", "nintendo").Equals("xbox", System.StringComparison.OrdinalIgnoreCase);
            touchEnabled = NxConfig.GetBool("input.touch", true);
            try { InputSystem.RegisterLayout<NxGamepad>(); }
            catch (System.Exception e) { customLayout = false; Debug.LogWarning("[NxPort] NxGamepad layout unavailable (" + e.Message + "), using plain Gamepad (no rumble)"); }
            if (touchEnabled) touchscreen = InputSystem.AddDevice<Touchscreen>("NxTouchscreen");
            InputSystem.onBeforeUpdate += Pump;
            Debug.Log("[NxPort] input bridge up: layout=" + (nintendoLayout ? "nintendo" : "xbox") + " touch=" + touchEnabled);
        }

        void OnDestroy() { InputSystem.onBeforeUpdate -= Pump; }

        static float Axis(float v) => v < -1f ? -1f : (v > 1f ? 1f : v);

        void Pump() {
            // The Switch keyboard replaced the game's own on-screen keyboard
            // for this field: close the latter, otherwise it stays open,
            // keeps re-activating the field, and -- left open when a match
            // starts -- its Navigate handler outlives the menu scene
            // (NullReferenceException in UI/Navigate 'canceled').
            if (NxBridge.TakeKeyboardClosed()) {
                var osk = NSMB.UI.MainMenu.OnScreenKeyboard.Instance;
                if (osk && osk.IsOpen) osk.Close();
            }
            int n = NxBridge.PollPads(pads);
            for (int i = 0; i < MaxPads; i++) {
                bool on = i < n && pads[i].connected != 0;
                if (on && devices[i] == null) {
                    if (customLayout) {
                        try {
                            var d = InputSystem.AddDevice<NxGamepad>("NxPad" + (i + 1));
                            d.nxIndex = i;
                            devices[i] = d;
                        } catch (System.Exception e) {
                            customLayout = false;
                            Debug.LogWarning("[NxPort] NxGamepad failed (" + e.Message + "), falling back to Gamepad");
                        }
                    }
                    if (devices[i] == null) devices[i] = InputSystem.AddDevice<Gamepad>("NxPad" + (i + 1));
                    Debug.Log("[NxPort] controller " + (i + 1) + " connected (style 0x" + pads[i].style.ToString("x") + ")");
                } else if (!on && devices[i] != null) {
                    InputSystem.RemoveDevice(devices[i]);
                    devices[i] = null;
                    Debug.Log("[NxPort] controller " + (i + 1) + " disconnected");
                }
                if (devices[i] != null) InputSystem.QueueStateEvent(devices[i], ToState(pads[i]));
            }

            if (touchscreen == null) return;
            int t = NxBridge.PollTouch(touches);
            float w = Screen.width, h = Screen.height;
            ended.Clear();
            foreach (var id in activeTouches.Keys) ended.Add(id);
            for (int i = 0; i < t; i++) {
                int id = touches[i].id + 1; // touchId 0 is "no touch"
                var pos = new Vector2(touches[i].x * w, (1f - touches[i].y) * h);
                bool known = activeTouches.ContainsKey(id);
                ended.Remove(id);
                activeTouches[id] = pos;
                InputSystem.QueueStateEvent(touchscreen, new TouchState {
                    touchId = id, position = pos,
                    phase = known ? UnityEngine.InputSystem.TouchPhase.Moved : UnityEngine.InputSystem.TouchPhase.Began,
                });
            }
            foreach (int id in ended) {
                InputSystem.QueueStateEvent(touchscreen, new TouchState {
                    touchId = id, position = activeTouches[id], phase = UnityEngine.InputSystem.TouchPhase.Ended,
                });
                activeTouches.Remove(id);
            }
        }

        GamepadState ToState(in NxPadState p) {
            uint b = p.buttons;
            var s = new GamepadState {
                leftStick = new Vector2(Axis(p.lx), Axis(p.ly)),
                rightStick = new Vector2(Axis(p.rx), Axis(p.ry)),
                leftTrigger = (b & ZL) != 0 ? 1f : 0f,
                rightTrigger = (b & ZR) != 0 ? 1f : 0f,
            };
            void Set(uint mask, GamepadButton gb) { if ((b & mask) != 0) s = s.WithButton(gb); }
            if (nintendoLayout) {
                // By label, all four: the game names gamepad buttons the Xbox way
                // (buttonWest is "x", buttonNorth is "y" in the Input System), so
                // Switch X must be West for a binding on X to read "X".
                Set(A, GamepadButton.South); Set(B, GamepadButton.East);
                Set(X, GamepadButton.West); Set(Y, GamepadButton.North);
            } else {
                Set(B, GamepadButton.South); Set(A, GamepadButton.East);
                Set(X, GamepadButton.North); Set(Y, GamepadButton.West);
            }
            Set(L, GamepadButton.LeftShoulder); Set(R, GamepadButton.RightShoulder);
            Set(ZL, GamepadButton.LeftTrigger); Set(ZR, GamepadButton.RightTrigger);
            Set(Plus, GamepadButton.Start); Set(Minus, GamepadButton.Select);
            Set(StickL, GamepadButton.LeftStick); Set(StickR, GamepadButton.RightStick);
            Set(Up, GamepadButton.DpadUp); Set(Down, GamepadButton.DpadDown);
            Set(Left, GamepadButton.DpadLeft); Set(Right, GamepadButton.DpadRight);
            return s;
        }
    }
}
