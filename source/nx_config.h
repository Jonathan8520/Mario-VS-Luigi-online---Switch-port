/* nx_config.h -- sdmc:/switch/mvlo/mvlo.ini, shared by the wrapper and the
 * managed bridge (the C# side reads the same text through nxbridge_get_config).
 * MIT -- part of mvlo_nx. */
#ifndef NX_CONFIG_H
#define NX_CONFIG_H

/* Load <root>/mvlo.ini, writing the default template there first if it does
 * not exist. Call once after the data root is resolved. */
void nxcfg_load(const char *root);

/* "section.key" lookups; def is returned for a missing or empty value. */
const char *nxcfg_get(const char *key, const char *def);
int         nxcfg_int(const char *key, int def);

/* Raw file text (NUL-terminated, may be empty). */
const char *nxcfg_text(void);

/* Log every key/value except secrets (appid is masked). */
void nxcfg_report(void);

#endif
