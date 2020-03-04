#ifndef __PLUGIN__
#define __PLUGIN__

#include <libaudcore/audstrings.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

#ifndef AUDACIOUS_XSF_PRIORITY
// set higher than stock Audio Overload xSF but lower than common plugins that use around 3
#ifdef _AUD_PLUGIN_DEFAULT_PRIO
# define AUDACIOUS_XSF_PRIORITY  (_AUD_PLUGIN_DEFAULT_PRIO - 1)
#else
# define AUDACIOUS_XSF_PRIORITY  4
#endif
#endif

class XsfPlugin : public InputPlugin {
public:
    static const char *const exts[];
    static const char *const defaults[];
    static const char about[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Highly Complete Decoder"), N_("xsf"), about, &prefs,
    };

    constexpr XsfPlugin() : InputPlugin (info,
            InputInfo() //InputInfo(FlagSubtunes)  // allow subsongs
            .with_priority(AUDACIOUS_XSF_PRIORITY)  // where 0=highest, 10=lowest (older) or 5 (newer)
            .with_exts(exts)) {}  // priority exts (accepted exts are still validated at runtime)

    bool init();
    void cleanup();
    bool is_our_file(const char *filename, VFSFile &file);
    Tuple read_tuple(const char *filename, VFSFile &file);
    bool read_tag(const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image);
    bool play(const char *filename, VFSFile &file);
};


typedef struct {
    bool loop_forever;
    double fade_length;
    double fade_delay;
} audacious_settings;

extern audacious_settings settings;

void xsf_settings_load();
void xsf_settings_save();

#endif
