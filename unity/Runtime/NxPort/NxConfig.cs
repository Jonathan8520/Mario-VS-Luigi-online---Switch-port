// NxConfig.cs -- runtime configuration read from sdmc:/switch/mvlo/mvlo.ini.
//
// The Photon Quantum AppId is NEVER compiled into this port: the CI blanks it in
// PhotonServerSettings.asset and rewrites NetworkHandler.cs to read
// NxConfig.AppIdQuantum instead of the official literal. Each user creates their
// own AppId on dashboard.photonengine.com and writes it into mvlo.ini.
//
// MIT -- part of mvlo_nx. Not part of the original game.
using System;
using System.Collections.Generic;
using Photon.Realtime;
using UnityEngine;

namespace NxPort {
    public static class NxConfig {
        static Dictionary<string, string> s_values;

        static void Load() {
            if (s_values != null) return;
            s_values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            string text = null;
            try { text = NxBridge.GetConfigText(); } catch (Exception e) { Debug.LogWarning("[NxPort] config: " + e); }
            if (text == null) return;
            string section = "";
            foreach (string raw in text.Split('\n')) {
                string line = raw.Trim();
                if (line.Length == 0 || line[0] == '#' || line[0] == ';') continue;
                if (line[0] == '[' && line.EndsWith("]")) { section = line.Substring(1, line.Length - 2).Trim(); continue; }
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                string k = line.Substring(0, eq).Trim();
                string v = line.Substring(eq + 1).Trim();
                if (v.Length >= 2 && v[0] == '"' && v[v.Length - 1] == '"') v = v.Substring(1, v.Length - 2);
                s_values[section.Length > 0 ? section + "." + k : k] = v;
            }
            Debug.Log("[NxPort] mvlo.ini: " + s_values.Count + " key(s); photon.appid_quantum "
                      + (string.IsNullOrEmpty(Get("photon.appid_quantum", "")) ? "MISSING" : "set"));
        }

        public static string Get(string key, string def) {
            Load();
            return s_values.TryGetValue(key, out string v) && v.Length > 0 ? v : def;
        }

        public static bool GetBool(string key, bool def) {
            string v = Get(key, null);
            if (v == null) return def;
            return v == "1" || v.Equals("true", StringComparison.OrdinalIgnoreCase) || v.Equals("yes", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>AppId used by NetworkHandler.ConnectToRegion (patched in at build time).</summary>
        public static string AppIdQuantum {
            get {
                string id = Get("photon.appid_quantum", "");
                if (string.IsNullOrEmpty(id)) {
                    Debug.LogError("[NxPort] No Photon Quantum AppId: set appid_quantum in the [photon] section of sdmc:/switch/mvlo/mvlo.ini (see docs/TEST.md)");
                    NxBridge.Log("[nxport] ERROR: photon.appid_quantum missing from mvlo.ini -- online play cannot connect");
                }
                return id;
            }
        }

        /// <summary>Optional fixed region ("eu", "us", ...) or null for best region.</summary>
        public static string FixedRegion(string requested) {
            string r = Get("photon.fixed_region", "");
            return string.IsNullOrEmpty(r) ? requested : r;
        }

        /// <summary>
        /// The official build authenticates through mariovsluigi.azurewebsites.net
        /// (Photon custom auth bound to the OFFICIAL AppId). A user's own AppId has
        /// no custom auth, and UnityWebRequest needs Android's Java HTTP stack,
        /// which does not exist under the wrapper. So by default we skip it.
        /// </summary>
        public static bool SkipOfficialAuth => !NxBridge.IsSwitch ? false : !GetBool("photon.use_official_auth", false);

        public static AuthenticationValues CreateAuthValues() {
            string id = PlayerPrefs.GetString("nxport_userid", "");
            if (string.IsNullOrEmpty(id)) {
                id = Guid.NewGuid().ToString();
                PlayerPrefs.SetString("nxport_userid", id);
                PlayerPrefs.Save();
            }
            Debug.Log("[NxPort] official auth skipped; anonymous Photon auth as " + id);
            return new AuthenticationValues { UserId = id };
        }

        public static bool SkipUpdateCheck => NxBridge.IsSwitch && !GetBool("game.check_updates", false);

        /// <summary>
        /// Suffix of the Photon AppVersion (patched into NetworkHandler at build
        /// time). Photon only matches players with the same AppId AND AppVersion:
        /// the public sources build as "vanilla", the official releases as
        /// "official". mvlo.ini [photon] build_identifier overrides it; empty
        /// keeps the game's own value.
        /// </summary>
        public static string BuildIdentifier(string gameSuffix) {
            string id = Get("photon.build_identifier", "");
            string suffix = string.IsNullOrEmpty(id) ? gameSuffix : "-" + id;
            Debug.Log("[NxPort] Photon AppVersion suffix: " + suffix);
            return suffix;
        }
    }
}
