/**
 * Highly Complete for Audacious
 */

#include <cstdlib>
#include <algorithm>
#include <string.h>

#if DEBUG
#include <ctime>
#include <sys/time.h>
#endif

#include <libaudcore/audio.h>

#include "plugin.h"
#include "vfs.h"

#include "Highly_Experimental/Core/psx.h"
#include "Highly_Experimental/Core/iop.h"
#include "Highly_Experimental/Core/r3000.h"
#include "Highly_Experimental/Core/spu.h"
#include "Highly_Experimental/Core/bios.h"

#include "Highly_Theoretical/Core/sega.h"

#include "Highly_Quixotic/Core/qsound.h"

#undef uint8
#undef uint16
#undef uint32

#include "mGBA/include/mgba/core/core.h"
#include "mGBA/include/mgba/core/blip_buf.h"
#include "mGBA/include/mgba-util/vfs.h"
#include "mGBA/include/mgba/core/log.h"

#include "lazyusf2/usf/usf.h"

#include "vio2sf/src/vio2sf/desmume/state.h"

#include "psflib/psflib.h"
#include "psflib/psf2fs.h"

#include "hebios.h"

#include <zlib.h>

# define strdup(s)							      \
  (__extension__							      \
    ({									      \
      const char *__old = (s);						      \
      size_t __len = strlen (__old) + 1;				      \
      char *__new = (char *) malloc (__len);			      \
      (char *) memcpy (__new, __old, __len);				      \
    }))

#define CFG_ID "xsf" // ID for storing in audacious
#define MIN_BUFFER_SIZE 576

/* global state */
/*EXPORT*/ XsfPlugin aud_plugin_instance;
audacious_settings settings;
int psf_version = 0;
char * file_path = NULL;
void * emulator = NULL;
void * emulator_extra = NULL;

struct psf_tag {
    char * name;
    char * value;
    struct psf_tag * next;
};

struct psf_info_meta_state {
    int tag_song_ms;
    int tag_fade_ms;

    int utf8;

    struct psf_tag *tags;
};

psf_info_meta_state info_state = {0};

/* Audacious will first send the file to a plugin based on this static extension list. If none
 * accepts it'll try again all plugins, ordered by priority, until one accepts the file. Problem is,
 * mpg123 plugin has higher priority and tendency to accept files that aren't even MP3. To fix this
 * we declare a few conflicting formats so we have a better chance.
 * The extension affects only this priority and in all cases file must accepted during "is_our_file".
 */
const char *const XsfPlugin::exts[] = {
        "psf", "minipsf", "psf2", "minipsf2", "ssf", "minissf", "dsf", "minidsf", "qsf", "miniqsf", "usf", "miniusf", "gsf", "minigsf", "2sf", "mini2sf", nullptr
};


const char *const XsfPlugin::defaults[] = {
    "loop_forever",     "FALSE",
    "fade_length",      "10.0",
    "fade_delay",       "170.0",
    NULL
};

// N_(...) for i18n but not much point here
const char XsfPlugin::about[] =
    "Highly Complete 0.1 " __DATE__ "\n"
    "by kode54\n"
    "\n"
    "https://g.losno.co/chris/audacious-xsf";

const PreferencesWidget XsfPlugin::widgets[] = {
    WidgetLabel(N_("<b>Highly Complete config</b>")),
    WidgetCheck(N_("Loop forever:"), WidgetBool(settings.loop_forever)),
    WidgetSpin(N_("Default fade:"), WidgetFloat(settings.fade_length), {0, 60, 0.1}),
    WidgetSpin(N_("Default length:"), WidgetFloat(settings.fade_delay), {0, 600, 0.1}),
};

void xsf_settings_load() {
    AUDINFO("load settings\n");
    aud_config_set_defaults(CFG_ID, XsfPlugin::defaults);
    settings.loop_forever = aud_get_bool(CFG_ID, "loop_forever");
    settings.fade_length = aud_get_double(CFG_ID, "fade_length");
    settings.fade_delay = aud_get_double(CFG_ID, "fade_delay");
}

void xsf_settings_save() {
    AUDINFO("save settings\n");
    aud_set_bool(CFG_ID, "loop_forever", settings.loop_forever);
    aud_set_double(CFG_ID, "fade_length", settings.fade_length);
    aud_set_double(CFG_ID, "fade_delay", settings.fade_delay);
}

const PluginPreferences XsfPlugin::prefs = {
    {widgets}, xsf_settings_load, xsf_settings_save
};

static int
get_srate(int version) {
    switch (version)
    {
        case 1: case 0x11: case 0x12: case 0x21:
        case 0x24:
            return 44100;

        case 2:
            return 48000;

        case 0x22:
            return 32768;

        case 0x41:
            return 24038;
    }
    return -1;
}

bool XsfPlugin::is_our_file(const char *filename, VFSFile &file) {
    AUDDBG("test file=%s\n", filename);

    unsigned char magic[4];
    if (file.fread(magic, 1, 4) < 4)
        return false;

    if (memcmp(magic, "PSF", 3) != 0)
        return false;

    if (get_srate(magic[3]) <= 0)
        return false;

    return true;
}

static void GSFLogger(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args)
{
    (void)logger;
    (void)category;
    (void)level;
    (void)format;
    (void)args;
}

static struct mLogger gsf_logger = {
    .log = GSFLogger,
};

// called on startup (main thread)
bool XsfPlugin::init() {
    AUDINFO("plugin start\n");

    bios_set_image( hebios, HEBIOS_SIZE );
    psx_init();
    sega_init();
    qsound_init();
    mLogSetDefaultLogger(&gsf_logger);

    xsf_settings_load();

    return true;
}

// called on stop (main thread)
void XsfPlugin::cleanup() {
    AUDINFO("plugin end\n");

    xsf_settings_save();
}

static struct psf_tag *
add_tag( struct psf_tag * tags, const char * name, const char * value ) {
    struct psf_tag * tag = (struct psf_tag *) malloc( sizeof( struct psf_tag ) );
    if ( !tag ) return tags;

    tag->name = strdup( name );
    if ( !tag->name ) {
        free( tag );
        return tags;
    }
    tag->value = strdup( value );
    if ( !tag->value ) {
        free( tag->name );
        free( tag );
        return tags;
    }
    tag->next = tags;
    return tag;
}

static void
free_tags( struct psf_tag * tags ) {
    struct psf_tag * tag, * next;

    tag = tags;

    while ( tag ) {
        next = tag->next;
        free( tag->name );
        free( tag->value );
        free( tag );
        tag = next;
    }
}

#define BORK_TIME 0xC0CAC01A

inline unsigned get_be16( void const* p )
{
    return  (unsigned) ((unsigned char const*) p) [0] << 8 |
            (unsigned) ((unsigned char const*) p) [1];
}

inline unsigned get_le32( void const* p )
{
    return  (unsigned) ((unsigned char const*) p) [3] << 24 |
            (unsigned) ((unsigned char const*) p) [2] << 16 |
            (unsigned) ((unsigned char const*) p) [1] <<  8 |
            (unsigned) ((unsigned char const*) p) [0];
}

inline unsigned get_be32( void const* p )
{
    return  (unsigned) ((unsigned char const*) p) [0] << 24 |
            (unsigned) ((unsigned char const*) p) [1] << 16 |
            (unsigned) ((unsigned char const*) p) [2] <<  8 |
            (unsigned) ((unsigned char const*) p) [3];
}

void set_le32( void* p, unsigned n )
{
    ((unsigned char*) p) [0] = (unsigned char) n;
    ((unsigned char*) p) [1] = (unsigned char) (n >> 8);
    ((unsigned char*) p) [2] = (unsigned char) (n >> 16);
    ((unsigned char*) p) [3] = (unsigned char) (n >> 24);
}

static unsigned long parse_time_crap(const char *input)
{
    unsigned long value = 0;
    unsigned long multiplier = 1000;
    const char * ptr = input;
    unsigned long colon_count = 0;
    
    while (*ptr && ((*ptr >= '0' && *ptr <= '9') || *ptr == ':'))
    {
        colon_count += *ptr == ':';
        ++ptr;
    }
    if (colon_count > 2) return BORK_TIME;
    if (*ptr && *ptr != '.' && *ptr != ',') return BORK_TIME;
    if (*ptr) ++ptr;
    while (*ptr && *ptr >= '0' && *ptr <= '9') ++ptr;
    if (*ptr) return BORK_TIME;
    
    ptr = strrchr(input, ':');
    if (!ptr)
        ptr = input;
    for (;;)
    {
        char * end;
        if (ptr != input) ++ptr;
        if (multiplier == 1000)
        {
            double temp = strtod(ptr, &end);
            if (temp >= 60.0) return BORK_TIME;
            value = (long)(temp * 1000.0f);
        }
        else
        {
            unsigned long temp = strtoul(ptr, &end, 10);
            if (temp >= 60 && multiplier < 3600000) return BORK_TIME;
            value += temp * multiplier;
        }
        if (ptr == input) break;
        ptr -= 2;
        while (ptr > input && *ptr != ':') --ptr;
        multiplier *= 60;
    }
    
    return value;
}

static int
psf_info_meta(void * context, const char * name, const char * value) {
    struct psf_info_meta_state * state = ( struct psf_info_meta_state * ) context;

    if ( !strcasecmp( name, "length" ) ) {
        unsigned long n = parse_time_crap( value );
        if ( n != BORK_TIME ) state->tag_song_ms = n;
    }
    else if ( !strcasecmp( name, "fade" ) ) {
        unsigned long n = parse_time_crap( value );
        if ( n != BORK_TIME ) state->tag_fade_ms = n;
    }
    else if ( !strcasecmp( name, "utf8" ) ) {
        state->utf8 = 1;
    }
    else if ( *name != '_' ) {
        if ( !strcasecmp( name, "game" ) ) name = "album";
        else if ( !strcasecmp( name, "year" ) ) name = "date";
        else if ( !strcasecmp( name, "tracknumber" ) ) name = "track";
        else if ( !strcasecmp( name, "discnumber" ) ) name = "disc";

        state->tags = add_tag( state->tags, name, value );
    }

    return 0;
}

static void
psf_error_log(void * unused, const char * message) {
    AUDINFO("%s\n", message);
}

static const char *
get_codec(int version) {
    switch (version) {
        case 0x01: return "Sony Playstation Audio";
        case 0x02: return "Sony Playstation 2 Audio";
        case 0x11: return "Sega Saturn Audio";
        case 0x12: return "Sega Dreamcast Audio";
        case 0x21: return "Nintendo 64 Audio";
        case 0x22: return "Nintendo GBA Audio";
        case 0x24: return "Nintendo DS Audio";
        case 0x41: return "QSound Audio";
        default: return "Unknown xSF Audio";
    }
}

// internal helper, called every time user adds a new file to playlist
static bool read_info(const char * filename, Tuple & tuple) {
    AUDINFO("read file=%s\n", filename);

    struct psf_info_meta_state info_state;
    memset(&info_state, 0, sizeof(info_state));

    int version = psf_load_vfs( filename, 0, 0, 0, psf_info_meta, &info_state, 0, psf_error_log, 0);
    if (version <= 0) return false;

    //todo apply_config
    int output_channels = 2;
    int sample_rate = get_srate(version);
    if (sample_rate <= 0) return false;

    int bitrate = 0;
    int ms = info_state.tag_song_ms + info_state.tag_fade_ms;
    if (!info_state.tag_song_ms)
         ms = (settings.fade_delay + settings.fade_length) * 1000LL;

    // short form, not sure if better way
    tuple.set_format("xsf", output_channels, sample_rate, bitrate);
    tuple.set_filename(filename); //used?
    tuple.set_int(Tuple::Bitrate, bitrate); //in kb/s
    tuple.set_int(Tuple::Length, ms);

    tuple.set_str(Tuple::Codec, get_codec(version));
    tuple.set_str(Tuple::Quality, N_("sequenced"));

    struct psf_tag * tag = info_state.tags;
    while ( tag ) {
        if ( !strncasecmp( tag->name, "replaygain_", strlen("replaygain_") ) ) {
            bool album = !strncasecmp( tag->name + strlen("replaygain_"), "album_", strlen("album_") );
            bool track = !strncasecmp( tag->name + strlen("replaygain_"), "track_", strlen("track_") );
            if ( album || track ) {
                bool gain = !strcasecmp( tag->name + strlen("replaygain_track_"), "gain");
                bool peak = !strcasecmp( tag->name + strlen("replaygain_track_"), "peak");
                if ( gain || peak ) {
                    Tuple::Field field, unit_field;
                    if (album) field = gain ? Tuple::AlbumGain : Tuple::AlbumPeak;
                    else field = gain ? Tuple::TrackGain : Tuple::TrackPeak;
                    unit_field = gain ? Tuple::GainDivisor : Tuple::PeakDivisor;
                    tuple.set_gain (field, unit_field, tag->value);
                }
            }
        }
        else if ( !strcasecmp( tag->name, "title" ) )
            tuple.set_str (Tuple::Title, tag->value);
        else if ( !strcasecmp( tag->name, "artist" ) )
            tuple.set_str (Tuple::Artist, tag->value);
        else if ( !strcasecmp( tag->name, "album" ) )
            tuple.set_str (Tuple::Album, tag->value);
        else if ( !strcasecmp( tag->name, "date" ) )
            tuple.set_str (Tuple::Date, tag->value);
        else if ( !strcasecmp( tag->name, "genre" ) )
            tuple.set_str (Tuple::Genre, tag->value);
        else if ( !strcasecmp( tag->name, "copyright" ) )
            tuple.set_str (Tuple::Copyright, tag->value);
        else if ( !strcasecmp( tag->name, "comment" ) )
            tuple.set_str (Tuple::Comment, tag->value);
        else if ( !strcasecmp( tag->name, "track" ) )
            tuple.set_int (Tuple::Track, atoi(tag->value));
        tag = tag->next;
    }

    free_tags( info_state.tags );

    return true;
}

// thread safe (for Audacious <= 3.7, unused otherwise)
Tuple XsfPlugin::read_tuple(const char *filename, VFSFile &file) {
    Tuple tuple;
    read_info(filename, tuple);
    return tuple;
}

// thread safe (for Audacious >= 3.8, unused otherwise)
bool XsfPlugin::read_tag(const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image) {
    return read_info(filename, tuple);
}

static int
xsf_reset(void);

static int
xsf_render(int16_t * buf, unsigned count);

// internal util to seek during play
static void seek_helper(int seek_value, int &current_sample_pos) {
    AUDINFO("seeking\n");

    // compute from ms to samples
    int seek_needed_samples = (long long)seek_value * get_srate(psf_version) / 1000L;
    short buffer[MIN_BUFFER_SIZE * 2];
    int max_buffer_samples = MIN_BUFFER_SIZE;

    int samples_to_do = 0;
    if (seek_needed_samples < current_sample_pos) {
        // go back in time, reopen file
        AUDINFO("resetting file to seek backwards\n");
        xsf_reset();
        current_sample_pos = 0;
        samples_to_do = seek_needed_samples;
    } else if (current_sample_pos < seek_needed_samples) {
        // go forward in time
        samples_to_do = seek_needed_samples - current_sample_pos;
    }

    // do the actual seeking
    if (samples_to_do >= 0) {
        AUDINFO("rendering forward\n");

        // render till seeked sample
        while (samples_to_do > 0) {
            int seek_samples = std::min(max_buffer_samples, samples_to_do);
            current_sample_pos += seek_samples;
            samples_to_do -= seek_samples;
            xsf_render(buffer, seek_samples);
        }
    }
}

typedef struct {
    uint32_t pc0;
    uint32_t gp0;
    uint32_t t_addr;
    uint32_t t_size;
    uint32_t d_addr;
    uint32_t d_size;
    uint32_t b_addr;
    uint32_t b_size;
    uint32_t s_ptr;
    uint32_t s_size;
    uint32_t sp,fp,gp,ret,base;
} exec_header_t;

typedef struct {
    char key[8];
    uint32_t text;
    uint32_t data;
    exec_header_t exec;
    char title[60];
} psxexe_hdr_t;

struct psf1_load_state {
    void * emu;
    bool first;
    unsigned refresh;
};

static int
psf1_info(void * context, const char * name, const char * value) {
    psf1_load_state * state = ( psf1_load_state * ) context;
    
    if ( !state->refresh && !strcasecmp(name, "_refresh") ) {
        state->refresh = atoi( value );
    }
    
    return 0;
}

int
psf1_load(void * context, const uint8_t * exe, size_t exe_size,
          const uint8_t * reserved, size_t reserved_size) {
    psf1_load_state * state = ( psf1_load_state * ) context;
    
    psxexe_hdr_t *psx = (psxexe_hdr_t *) exe;
    
    if ( exe_size < 0x800 ) return -1;
    
    uint32_t addr = get_le32( &psx->exec.t_addr );
    uint32_t size = (uint32_t)exe_size - 0x800;
    
    addr &= 0x1fffff;
    if ( ( addr < 0x10000 ) || ( size > 0x1f0000 ) || ( addr + size > 0x200000 ) ) return -1;
    
    void * pIOP = psx_get_iop_state( state->emu );
    iop_upload_to_ram( pIOP, addr, exe + 0x800, size );
    
    if ( !state->refresh ) {
        if (!strncasecmp((const char *) exe + 113, "Japan", 5)) state->refresh = 60;
        else if (!strncasecmp((const char *) exe + 113, "Europe", 6)) state->refresh = 50;
        else if (!strncasecmp((const char *) exe + 113, "North America", 13)) state->refresh = 60;
    }
    
    if ( state->first ) {
        void * pR3000 = iop_get_r3000_state( pIOP );
        r3000_setreg(pR3000, R3000_REG_PC, get_le32( &psx->exec.pc0 ) );
        r3000_setreg(pR3000, R3000_REG_GEN+29, get_le32( &psx->exec.s_ptr ) );
        state->first = false;
    }
    
    return 0;
}

static int EMU_CALL
virtual_readfile(void *context, const char *path, int offset, char *buffer, int length) {
    return psf2fs_virtual_readfile(context, path, offset, buffer, length);
}

struct sdsf_loader_state {
    uint8_t * data;
    size_t data_size;
};

int
sdsf_loader(void * context, const uint8_t * exe, size_t exe_size,
            const uint8_t * reserved, size_t reserved_size) {
    if ( exe_size < 4 ) return -1;
    
    struct sdsf_loader_state * state = ( struct sdsf_loader_state * ) context;
    
    uint8_t * dst = state->data;
    
    if ( state->data_size < 4 ) {
        state->data = dst = ( uint8_t * ) malloc( exe_size );
        state->data_size = exe_size;
        memcpy( dst, exe, exe_size );
        return 0;
    }
    
    uint32_t dst_start = get_le32( dst );
    uint32_t src_start = get_le32( exe );
    dst_start &= 0x7fffff;
    src_start &= 0x7fffff;
    size_t dst_len = state->data_size - 4;
    size_t src_len = exe_size - 4;
    if ( dst_len > 0x800000 ) dst_len = 0x800000;
    if ( src_len > 0x800000 ) src_len = 0x800000;
    
    if ( src_start < dst_start ) {
        uint32_t diff = dst_start - src_start;
        state->data_size = dst_len + 4 + diff;
        state->data = dst = ( uint8_t * ) realloc( dst, state->data_size );
        memmove( dst + 4 + diff, dst + 4, dst_len );
        memset( dst + 4, 0, diff );
        dst_len += diff;
        dst_start = src_start;
        set_le32( dst, dst_start );
    }
    if ( ( src_start + src_len ) > ( dst_start + dst_len ) ) {
        size_t diff = ( src_start + src_len ) - ( dst_start + dst_len );
        state->data_size = dst_len + 4 + diff;
        state->data = dst = ( uint8_t * ) realloc( dst, state->data_size );
        memset( dst + 4 + dst_len, 0, diff );
    }
    
    memcpy( dst + 4 + ( src_start - dst_start ), exe + 4, src_len );
    
    return 0;
}

struct qsf_loader_state {
    uint8_t * key;
    uint32_t key_size;

    uint8_t * z80_rom;
    uint32_t z80_size;

    uint8_t * sample_rom;
    uint32_t sample_size;
};

static int
qsf_upload_section( struct qsf_loader_state * state, const char * section, uint32_t start,
                    const uint8_t * data, uint32_t size ) {
    uint8_t ** array = NULL;
    uint32_t * array_size = NULL;
    uint32_t max_size = 0x7fffffff;

    if ( !strcmp( section, "KEY" ) ) { array = &state->key; array_size = &state->key_size; max_size = 11; }
    else if ( !strcmp( section, "Z80" ) ) { array = &state->z80_rom; array_size = &state->z80_size; }
    else if ( !strcmp( section, "SMP" ) ) { array = &state->sample_rom; array_size = &state->sample_size; }
    else return -1;

    if ( ( start + size ) < start ) return -1;

    uint32_t new_size = start + size;
    uint32_t old_size = *array_size;
    if ( new_size > max_size ) return -1;

    if ( new_size > old_size ) {
        *array = (uint8_t *) realloc( *array, new_size );
        *array_size = new_size;
        memset( *array + old_size, 0, new_size - old_size );
    }

    memcpy( *array + start, data, size );

    return 0;
}

static int
qsf_load(void * context, const uint8_t * exe, size_t exe_size,
         const uint8_t * reserved, size_t reserved_size) {
    struct qsf_loader_state * state = ( struct qsf_loader_state * ) context;

    for (;;) {
        char s[4];
        if ( exe_size < 11 ) break;
        memcpy( s, exe, 3 ); exe += 3; exe_size -= 3;
        s [3] = 0;
        uint32_t dataofs  = get_le32( exe ); exe += 4; exe_size -= 4;
        uint32_t datasize = get_le32( exe ); exe += 4; exe_size -= 4;
        if ( datasize > exe_size )
            return -1;

        if ( qsf_upload_section( state, s, dataofs, exe, datasize ) < 0 )
            return -1;

        exe += datasize;
        exe_size -= datasize;
    }

    return 0;
}

struct gsf_loader_state {
    int entry_set;
    uint32_t entry;
    uint8_t * data;
    size_t data_size;
};

static int
gsf_loader(void * context, const uint8_t * exe, size_t exe_size,
           const uint8_t * reserved, size_t reserved_size) {
    if ( exe_size < 12 ) return -1;
    
    struct gsf_loader_state * state = ( struct gsf_loader_state * ) context;
    
    unsigned char *iptr;
    size_t isize;
    unsigned char *xptr;
    unsigned xentry = get_le32(exe + 0);
    unsigned xsize = get_le32(exe + 8);
    unsigned xofs = get_le32(exe + 4) & 0x1ffffff;
    if ( xsize < exe_size - 12 ) return -1;
    if (!state->entry_set) {
        state->entry = xentry;
        state->entry_set = 1;
    }
    {
        iptr = state->data;
        isize = state->data_size;
        state->data = 0;
        state->data_size = 0;
    }
    if (!iptr) {
        size_t rsize = xofs + xsize;
        {
            rsize -= 1;
            rsize |= rsize >> 1;
            rsize |= rsize >> 2;
            rsize |= rsize >> 4;
            rsize |= rsize >> 8;
            rsize |= rsize >> 16;
            rsize += 1;
        }
        iptr = (unsigned char *) malloc(rsize + 10);
        if (!iptr)
            return -1;
        memset(iptr, 0, rsize + 10);
        isize = rsize;
    }
    else if (isize < xofs + xsize) {
        size_t rsize = xofs + xsize;
        {
            rsize -= 1;
            rsize |= rsize >> 1;
            rsize |= rsize >> 2;
            rsize |= rsize >> 4;
            rsize |= rsize >> 8;
            rsize |= rsize >> 16;
            rsize += 1;
        }
        xptr = (unsigned char *) realloc(iptr, xofs + rsize + 10);
        if (!xptr) {
            free(iptr);
            return -1;
        }
        iptr = xptr;
        isize = rsize;
    }
    memcpy(iptr + xofs, exe + 12, xsize);
    {
        state->data = iptr;
        state->data_size = isize;
    }
    return 0;
}

struct gsf_running_state {
    struct mAVStream stream;
    void * rom;
    int16_t samples[MIN_BUFFER_SIZE * 2];
    int buffered;
};

static void
_gsf_postAudioBuffer(struct mAVStream * stream, blip_t * left, blip_t * right) {
    struct gsf_running_state * state = ( struct gsf_running_state * ) stream;
    blip_read_samples(left, state->samples, MIN_BUFFER_SIZE, true);
    blip_read_samples(right, state->samples + 1, MIN_BUFFER_SIZE, true);
    state->buffered = MIN_BUFFER_SIZE;
}

struct usf_loader_state {
    uint32_t enablecompare;
    uint32_t enablefifofull;
    
    void * emu_state;
};

static int
usf_loader(void * context, const uint8_t * exe, size_t exe_size,
           const uint8_t * reserved, size_t reserved_size) {
    struct usf_loader_state * uUsf = ( struct usf_loader_state * ) context;
    if ( exe && exe_size > 0 ) return -1;
    
    return usf_upload_section( uUsf->emu_state, reserved, reserved_size );
}

static int
usf_info(void * context, const char * name, const char * value) {
    struct usf_loader_state * uUsf = ( struct usf_loader_state * ) context;
    
    if ( !strcasecmp( name, "_enablecompare" ) && strlen( value ) )
        uUsf->enablecompare = 1;
    else if ( !strcasecmp( name, "_enablefifofull" ) && strlen( value ) )
        uUsf->enablefifofull = 1;
    
    return 0;
}

struct twosf_loader_state {
    uint8_t * rom;
    uint8_t * state;
    size_t rom_size;
    size_t state_size;
    
    int initial_frames;
    int sync_type;
    int clockdown;
    int arm9_clockdown_level;
    int arm7_clockdown_level;
    
    twosf_loader_state()
    : rom(0), state(0), rom_size(0), state_size(0),
    initial_frames(-1), sync_type(0), clockdown(0),
    arm9_clockdown_level(0), arm7_clockdown_level(0) {
    }
    
    ~twosf_loader_state() {
        if (rom) free(rom);
        if (state) free(state);
    }
};

static int
load_twosf_map(struct twosf_loader_state *state, int issave, const unsigned char *udata, unsigned usize) {
    if (usize < 8) return -1;
    
    unsigned char *iptr;
    size_t isize;
    unsigned char *xptr;
    unsigned xsize = get_le32(udata + 4);
    unsigned xofs = get_le32(udata + 0);
    if (issave) {
        iptr = state->state;
        isize = state->state_size;
        state->state = 0;
        state->state_size = 0;
    }
    else {
        iptr = state->rom;
        isize = state->rom_size;
        state->rom = 0;
        state->rom_size = 0;
    }
    if (!iptr) {
        size_t rsize = xofs + xsize;
        if (!issave) {
            rsize -= 1;
            rsize |= rsize >> 1;
            rsize |= rsize >> 2;
            rsize |= rsize >> 4;
            rsize |= rsize >> 8;
            rsize |= rsize >> 16;
            rsize += 1;
        }
        iptr = (unsigned char *) malloc(rsize + 10);
        if (!iptr)
            return -1;
        memset(iptr, 0, rsize + 10);
        isize = rsize;
    }
    else if (isize < xofs + xsize) {
        size_t rsize = xofs + xsize;
        if (!issave) {
            rsize -= 1;
            rsize |= rsize >> 1;
            rsize |= rsize >> 2;
            rsize |= rsize >> 4;
            rsize |= rsize >> 8;
            rsize |= rsize >> 16;
            rsize += 1;
        }
        xptr = (unsigned char *) realloc(iptr, xofs + rsize + 10);
        if (!xptr) {
            free(iptr);
            return -1;
        }
        iptr = xptr;
        isize = rsize;
    }
    memcpy(iptr + xofs, udata + 8, xsize);
    if (issave) {
        state->state = iptr;
        state->state_size = isize;
    }
    else {
        state->rom = iptr;
        state->rom_size = isize;
    }
    return 0;
}

static int
load_twosf_mapz(struct twosf_loader_state *state, int issave, const unsigned char *zdata,
                unsigned zsize, unsigned zcrc) {
    int ret;
    int zerr;
    uLongf usize = 8;
    uLongf rsize = usize;
    unsigned char *udata;
    unsigned char *rdata;
    
    udata = (unsigned char *) malloc(usize);
    if (!udata)
        return -1;
    
    while (Z_OK != (zerr = uncompress(udata, &usize, zdata, zsize))) {
        if (Z_MEM_ERROR != zerr && Z_BUF_ERROR != zerr) {
            free(udata);
            return -1;
        }
        if (usize >= 8) {
            usize = get_le32(udata + 4) + 8;
            if (usize < rsize) {
                rsize += rsize;
                usize = rsize;
            }
            else
                rsize = usize;
        }
        else {
            rsize += rsize;
            usize = rsize;
        }
        rdata = (unsigned char *) realloc(udata, usize);
        if (!rdata) {
            free(udata);
            return -1;
        }
        udata = rdata;
    }
    
    rdata = (unsigned char *) realloc(udata, usize);
    if (!rdata) {
        free(udata);
        return -1;
    }
    
    if (0) {
        uLong ccrc = crc32(crc32(0L, Z_NULL, 0), rdata, (uInt) usize);
        if (ccrc != zcrc)
            return -1;
    }
    
    ret = load_twosf_map(state, issave, rdata, (unsigned) usize);
    free(rdata);
    return ret;
}

static int
twosf_loader(void * context, const uint8_t * exe, size_t exe_size,
             const uint8_t * reserved, size_t reserved_size) {
    struct twosf_loader_state * state = ( struct twosf_loader_state * ) context;
    
    if ( exe_size >= 8 ) {
        if ( load_twosf_map(state, 0, exe, (unsigned) exe_size) )
            return -1;
    }
    
    if ( reserved_size ) {
        size_t resv_pos = 0;
        if ( reserved_size < 16 )
            return -1;
        while ( resv_pos + 12 < reserved_size ) {
            unsigned save_size = get_le32(reserved + resv_pos + 4);
            unsigned save_crc = get_le32(reserved + resv_pos + 8);
            if (get_le32(reserved + resv_pos + 0) == 0x45564153) {
                if (resv_pos + 12 + save_size > reserved_size)
                    return -1;
                if (load_twosf_mapz(state, 1, reserved + resv_pos + 12, save_size, save_crc))
                    return -1;
            }
            resv_pos += 12 + save_size;
        }
    }
    
    return 0;
}

static int
twosf_info(void * context, const char * name, const char * value) {
    struct twosf_loader_state * state = ( struct twosf_loader_state * ) context;
    char *end;
    
    if ( !strcasecmp( name, "_frames" ) ) {
        state->initial_frames = (int)strtol( value, &end, 10 );
    }
    else if ( !strcasecmp( name, "_clockdown" ) ) {
        state->clockdown = (int)strtol( value, &end, 10 );
    }
    else if ( !strcasecmp( name, "_vio2sf_sync_type") ) {
        state->sync_type = (int)strtol( value, &end, 10 );
    }
    else if ( !strcasecmp( name, "_vio2sf_arm9_clockdown_level" ) ) {
        state->arm9_clockdown_level = (int)strtol( value, &end, 10 );
    }
    else if ( !strcasecmp( name, "_vio2sf_arm7_clockdown_level" ) ) {
        state->arm7_clockdown_level = (int)strtol( value, &end, 10 );
    }
    
    return 0;
}

static void
xsf_close(void) {
    if (emulator) {
        if (psf_version == 0x21) {
            usf_shutdown(emulator);
            free(emulator);
        }
        else if (psf_version == 0x22) {
            struct mCore * core = ( struct mCore * ) emulator;
            core->deinit(core);
        }
        else if (psf_version == 0x24) {
            NDS_state * state = (NDS_state *) emulator;
            state_deinit(state);
            free(state);
        }
        else {
            free(emulator);
        }
        emulator = NULL;
    }
    if (emulator_extra) {
        if (psf_version == 0x02)
            psf2fs_delete(emulator_extra);
        else if (psf_version == 0x22) {
            struct gsf_running_state * rstate = ( struct gsf_running_state * ) emulator_extra;
            free( rstate->rom );
            free( rstate );
        }
        else if (psf_version == 0x24)
            free(emulator_extra);
        else if (psf_version == 0x41) {
            struct qsf_loader_state * state = (struct qsf_loader_state *) emulator_extra;
            free(state->key);
            free(state->z80_rom);
            free(state->sample_rom);
            free(state);
        }
        emulator_extra = NULL;
    }
    if (info_state.tags) {
        free_tags(info_state.tags);
        info_state.tags = NULL;
    }
    if (file_path) {
        free(file_path);
        file_path = NULL;
    }
}

static int
xsf_open(const char * uri) {
    xsf_close();

    psf_version = psf_load_vfs( uri, 0, 0, 0, psf_info_meta, &info_state, 0, psf_error_log, 0 );

    if (psf_version <= 0) {
        AUDINFO("xsf: invalid PSF file\n");
        return -1;
    }

    file_path = strdup(uri);
    if (!file_path) {
        AUDINFO("xsf: out of memory\n");
        return -1;
    }
    
    if (psf_version == 1 || psf_version == 2) {
        emulator = malloc(psx_get_state_size(psf_version));
        
        if (!emulator) {
            AUDINFO( "xsf: out of memory\n" );
            return -1;
        }
        
        psx_clear_state(emulator, psf_version);
        
        if (psf_version == 1) {
            psf1_load_state state;
            
            state.emu = emulator;
            state.first = true;
            state.refresh = 0;
            
            if (psf_load_vfs(uri, 1, psf1_load, &state, psf1_info, &state, 1, psf_error_log, 0) <= 0) {
                AUDINFO( "xsf: invalid PSF file\n" );
                return -1;
            }
            
            if (state.refresh)
                psx_set_refresh(emulator, state.refresh);
        }
        else if (psf_version == 2) {
            emulator_extra = psf2fs_create();
            if (!emulator_extra) {
                AUDINFO( "xsf: out of memory\n" );
                return -1;
            }
            
            psf1_load_state state;
            
            state.refresh = 0;
            
            if (psf_load_vfs(uri, 2, psf2fs_load_callback, emulator_extra, psf1_info, &state, 1, psf_error_log, 0) <= 0) {
                AUDINFO( "xsf: invalid PSF file\n" );
                return -1;
            }
            
            if (state.refresh)
                psx_set_refresh(emulator, state.refresh);
            
            psx_set_readfile(emulator, virtual_readfile, emulator_extra);
        }
    }
    else if (psf_version == 0x11 || psf_version == 0x12) {
        struct sdsf_loader_state state;
        memset(&state, 0, sizeof(state));
        
        if (psf_load_vfs(uri, psf_version, sdsf_loader, &state, 0, 0, 0, psf_error_log, 0) <= 0) {
            AUDINFO( "xsf: invalid PSF file\n" );
            return -1;
        }
        
        emulator = malloc(sega_get_state_size(psf_version - 0x10));
        
        if (!emulator) {
            AUDINFO( "xsf: out of memory\n" );
            free(state.data);
            return -1;
        }
        
        sega_clear_state(emulator, psf_version - 0x10);
        
        sega_enable_dry(emulator, 1);
        sega_enable_dsp(emulator, 1);
        
        sega_enable_dsp_dynarec(emulator, 0);
        
        uint32_t start = get_le32(state.data);
        size_t length = state.data_size;
        const size_t max_length = (psf_version == 0x12) ? 0x800000 : 0x80000;
        if ((start + (length - 4)) > max_length)
            length = max_length - start + 4;
        sega_upload_program(emulator, state.data, (uint32_t)length);
        
        free(state.data);
    }
    else if (psf_version == 0x21) {
        struct usf_loader_state state;
        memset(&state, 0, sizeof(state));
        
        state.emu_state = malloc(usf_get_state_size());
        if (!state.emu_state) {
            AUDINFO( "psf: out of memory\n" );
            return -1;
        }
        
        usf_clear(state.emu_state);
        
        usf_set_hle_audio(state.emu_state, 1);
        
        emulator = (void *) state.emu_state;
        
        if (psf_load_vfs(uri, 0x21, usf_loader, &state, usf_info, &state, 1, psf_error_log, 0) <= 0) {
            AUDINFO( "xsf: invalid PSF file\n" );
            return -1;
        }
        
        usf_set_compare(state.emu_state, state.enablecompare);
        usf_set_fifo_full(state.emu_state, state.enablefifofull);
    }
    else if (psf_version == 0x22) {
        struct gsf_loader_state state;
        memset(&state, 0, sizeof(state));
        
        if (psf_load_vfs(uri, 0x22, gsf_loader, &state, 0, 0, 0, psf_error_log, 0) <= 0) {
            AUDINFO( "xsf: invalid PSF file\n" );
            return -1;
        }
        
        if (state.data_size > UINT_MAX)
        {
            AUDINFO( "xsf: GSF ROM image too large\n" );
            free(state.data);
            return -1;
        }
        
        struct VFile * rom = VFileFromConstMemory(state.data, state.data_size);
        if ( !rom ) {
            free( state.data );
            AUDINFO( "xsf: unable to load ROM\n" );
            return -1;
        }
        
        struct mCore * core = mCoreFindVF( rom );
        if ( !core ) {
            free(state.data);
            AUDINFO( "xsf: unable to find GBA core\n" );
            return -1;
        }
        
        struct gsf_running_state * rstate = (struct gsf_running_state *) calloc(1, sizeof(struct gsf_running_state));
        if ( !rstate ) {
            core->deinit(core);
            free(state.data);
            AUDINFO( "xsf: out of memory\n" );
            return -1;
        }
        
        rstate->rom = state.data;
        rstate->stream.postAudioBuffer = _gsf_postAudioBuffer;
        
        core->init(core);
        core->setAVStream(core, &rstate->stream);
        mCoreInitConfig(core, NULL);
        
        core->setAudioBufferSize(core, MIN_BUFFER_SIZE);
        
        blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), 32768);
        blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), 32768);
        
        struct mCoreOptions opts = {
            .skipBios = true,
            .useBios = false,
            .sampleRate = 32768,
            .volume = 0x100,
        };
        
        mCoreConfigLoadDefaults(&core->config, &opts);
        
        core->loadROM(core, rom);
        core->reset(core);

        emulator = (void *) core;
        emulator_extra = (void *) rstate;
    }
    else if (psf_version == 0x24) {
        struct twosf_loader_state state;
        memset(&state, 0, sizeof(state));
        
        NDS_state * nds_state = (NDS_state *) calloc(1, sizeof(*nds_state));
        if (!nds_state) {
            AUDINFO( "xsf: out of memory\n" );
            return -1;
        }
        
        emulator = (void *) nds_state;
        
        if (state_init(nds_state)) {
            AUDINFO( "xsf: out of memory\n" );
            return -1;
        }
        
        if (psf_load_vfs(uri, 0x24, twosf_loader, &state, twosf_info, &state, 1, psf_error_log, 0) <= 0) {
            AUDINFO( "xsf: invalid PSF file\n" );
            return -1;
        }
        
        if (!state.arm7_clockdown_level)
            state.arm7_clockdown_level = state.clockdown;
        if (!state.arm9_clockdown_level)
            state.arm9_clockdown_level = state.clockdown;
        
        nds_state->dwInterpolation = 1;
        nds_state->dwChannelMute = 0;
        
        nds_state->initial_frames = state.initial_frames;
        nds_state->sync_type = state.sync_type;
        nds_state->arm7_clockdown_level = state.arm7_clockdown_level;
        nds_state->arm9_clockdown_level = state.arm9_clockdown_level;
        
        if (state.rom)
            state_setrom(nds_state, state.rom, (u32)state.rom_size, 0);
        
        state_loadstate(nds_state, state.state, (u32)state.state_size);
        
        emulator_extra = state.rom;
        state.rom = 0; // So twosf_loader_state doesn't free it when it goes out of scope
    }
    else if (psf_version == 0x41) {
        struct qsf_loader_state * state = (struct qsf_loader_state *) calloc(1, sizeof(*state));
        
        if (!state) {
            AUDINFO( "xsf: out of memory\n" );
            return -1;
        }
        
        emulator_extra = (void *) state;
        
        if ( psf_load_vfs(uri, 0x41, qsf_load, state, 0, 0, 0, psf_error_log, 0) <= 0 ) {
            AUDINFO( "xsf: invalid PSF file\n" );
            return -1;
        }
        
        emulator = malloc(qsound_get_state_size());
        if (!emulator) {
            AUDINFO( "xsf: out of memory\n" );
            return -1;
        }
        
        qsound_clear_state(emulator);
        
        if(state->key_size == 11) {
            uint8_t * ptr = state->key;
            uint32_t swap_key1 = get_be32(ptr +  0);
            uint32_t swap_key2 = get_be32(ptr +  4);
            uint32_t addr_key  = get_be16(ptr +  8);
            uint8_t  xor_key   =        *(ptr + 10);
            qsound_set_kabuki_key(emulator, swap_key1, swap_key2, addr_key, xor_key);
        } else {
            qsound_set_kabuki_key(emulator, 0, 0, 0, 0);
        }
        qsound_set_z80_rom(emulator, state->z80_rom, state->z80_size);
        qsound_set_sample_rom(emulator, state->sample_rom, state->sample_size);
    }
    else {
        AUDINFO( "psf: unsupported PSF version %d\n", psf_version );
        return -1;
    }
    
    return 0;
}

static int
xsf_reset() {
    if (file_path) {
        char * path = file_path;
        int ret = xsf_open(path);
        free(path);
        return ret;
    }
    return -1;
}

static int
xsf_render(int16_t * buf, unsigned count)
{
    int err = 0;
    const char * errmsg;
    switch (psf_version)
    {
        case 1:
        case 2:
            err = psx_execute( emulator, 0x7FFFFFFF, buf, & count, 0 );
            if ( err == -2 ) AUDINFO( "xsf: Execution halted with an error." );
            break;
            
        case 0x11:
        case 0x12:
            err = sega_execute( emulator, 0x7FFFFFFF, buf, & count );
            break;
            
        case 0x21:
            errmsg = usf_render_resampled( emulator, buf, count, 44100 );
            if (errmsg) {
                AUDINFO("xsf: %s\n", errmsg);
                err = -1;
            }
            break;
            
        case 0x22: {
            struct mCore * core = ( struct mCore * ) emulator;
            struct gsf_running_state * rstate = ( struct gsf_running_state * ) emulator_extra;
            
            unsigned long frames_to_render = count;
            
            do {
                unsigned long frames_rendered = rstate->buffered;
                
                if ( frames_rendered >= frames_to_render ) {
                    if (buf) memcpy( buf, rstate->samples, frames_to_render * 4 );
                    frames_rendered -= frames_to_render;
                    memcpy( rstate->samples, rstate->samples + frames_to_render * 2, frames_rendered * 4 );
                    frames_to_render = 0;
                }
                else {
                    if (buf) {
                        memcpy( buf, rstate->samples, frames_rendered * 4 );
                        buf = (int16_t *)(((uint8_t *) buf) + frames_rendered * 4);
                    }
                    frames_to_render -= frames_rendered;
                    frames_rendered = 0;
                }
                rstate->buffered = (int) frames_rendered;
                
                if (frames_to_render) {
                    while ( !rstate->buffered )
                        core->runFrame(core);
                }
            }
            while (frames_to_render);
            count -= (unsigned) frames_to_render;
        }
            break;
            
        case 0x24:
            state_render( (NDS_state *)emulator, buf, count );
            break;
            
        case 0x41:
            err = qsound_execute( emulator, 0x7FFFFFFF, buf, &count );
            break;
    }
    if ( !count ) return -1;
    return err;
}


// called on play (play thread)
bool XsfPlugin::play(const char *filename, VFSFile &file) {
    AUDINFO("play file=%s\n", filename);

    // just in case
    if (psf_version && emulator)
        xsf_close();

    if (xsf_open(filename) < 0) {
        AUDERR("failed opening file %s\n", filename);
        return false;
    }

    int output_channels = 2;
    int sample_rate = get_srate(psf_version);

    //FMT_S8 / FMT_S16_NE / FMT_S24_NE / FMT_S32_NE / FMT_FLOAT
    open_audio(FMT_S16_LE, sample_rate, 2);

    // play
    short buffer[MIN_BUFFER_SIZE * 2];
    int max_buffer_samples = MIN_BUFFER_SIZE;
    int stream_samples_amount = (int)((long long)(info_state.tag_song_ms ? info_state.tag_song_ms + info_state.tag_fade_ms : (settings.fade_delay + settings.fade_length) * 1000) * sample_rate / 1000LL);
    int fade_samples = (int)((long long)(info_state.tag_song_ms ? info_state.tag_fade_ms:settings.fade_length * 1000) * sample_rate / 1000LL);
    int current_sample_pos = 0;

    while (!check_stop()) {
        int toget = max_buffer_samples;

        // handle seek request
        int seek_value = check_seek();
        if (seek_value >= 0)
            seek_helper(seek_value, current_sample_pos);

        // check stream finished
        if (!settings.loop_forever) {
            if (current_sample_pos >= stream_samples_amount)
                break;
            if (current_sample_pos + toget > stream_samples_amount)
                toget = stream_samples_amount - current_sample_pos;
        }

        xsf_render(buffer, toget);

        if (fade_samples > 0 &&
                !settings.loop_forever) {
            int samples_into_fade =
                    current_sample_pos - (stream_samples_amount - fade_samples);
            if (samples_into_fade + toget > 0) {
                for (int j = 0; j < toget; j++, samples_into_fade++) {
                    if (samples_into_fade > 0) {
                        double fadedness =
                                (double)(fade_samples - samples_into_fade) / fade_samples;
                        for (int k = 0; k < 2; k++)
                            buffer[j * 2 + k] *= fadedness;
                    }
                }
            }
        }

        write_audio(buffer, toget * sizeof(short) * output_channels);
        current_sample_pos += toget;
    }

    AUDINFO("play finished\n");

    xsf_close();
    return true;
}
