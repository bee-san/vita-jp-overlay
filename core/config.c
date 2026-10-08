#include "config.h"

#include <stddef.h>
#include <string.h>

#include "port.h"
#include "local_dict.h"
#include "textfilter.h"
#include "utf.h"

static const char *const trigger_names[VJO_TRIGGER_COUNT] = {
    "select", "start", "l+r", "select+l", "select+r", "rear_double_tap",
};

const char *vjo_trigger_name(int trigger)
{
    if (trigger < 0 || trigger >= VJO_TRIGGER_COUNT)
        return "?";
    return trigger_names[trigger];
}

static const VjoDictInfo dicts[VJO_DICT_COUNT] = {
    [VJO_DICT_JPDB] = {"jpdb", "jpdb.io", "jpdb_api_key", "VJO_JPDB_KEY"},
    [VJO_DICT_JITEN] = {"jiten", "jiten.moe", "jiten_api_key", "VJO_JITEN_KEY"},
    [VJO_DICT_HACHIDORI] = {"hachidori", "Hachidori relay", NULL, NULL},
    [VJO_DICT_LOCAL] = {"local", "Local dictionaries", NULL, NULL},
};

const VjoDictInfo *vjo_dict_info(int dictionary)
{
    return &dicts[dictionary >= 0 && dictionary < VJO_DICT_COUNT ? dictionary : VJO_DICT_JPDB];
}

const char *vjo_dict_name(int dictionary)
{
    return vjo_dict_info(dictionary)->name;
}

const char *vjo_config_api_key(const VjoConfig *c)
{
    return c->dictionary == VJO_DICT_HACHIDORI || c->dictionary == VJO_DICT_LOCAL ? "" : c->api_key[c->dictionary];
}

int vjo_config_dict_ready(const VjoConfig *c)
{
    /* File availability is checked by the worker, without blocking the UI thread. */
    if (c->dictionary == VJO_DICT_LOCAL)
        return 1;
    if (c->dictionary == VJO_DICT_HACHIDORI) {
        char host[64];
        int port;
        return vjo_hachidori_endpoint(c->hachidori_host, host, sizeof(host), &port) == 0;
    }
    return vjo_config_api_key(c)[0] != '\0';
}

static const struct {
    const char *key, *def; /* defaults: the Lapis note type */
} anki_fields[VJO_ANKI_FIELD_COUNT] = {
    [VJO_ANKI_WORD] = {"anki_field_word", "Expression"},
    [VJO_ANKI_READING] = {"anki_field_reading", "ExpressionReading"},
    [VJO_ANKI_FURIGANA] = {"anki_field_furigana", "ExpressionFurigana"},
    [VJO_ANKI_DEFINITION] = {"anki_field_definition", "MainDefinition"},
    [VJO_ANKI_SENTENCE] = {"anki_field_sentence", "Sentence"},
    [VJO_ANKI_PICTURE] = {"anki_field_picture", "Picture"},
    [VJO_ANKI_FREQUENCY] = {"anki_field_frequency", "FreqSort"},
    [VJO_ANKI_AUDIO] = {"anki_field_audio", "ExpressionAudio"},
};

void vjo_config_defaults(VjoConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->dictionary = VJO_DICT_HACHIDORI;
    vjo_snprintf(c->local_dictionary_dir, sizeof(c->local_dictionary_dir), "ux0:data/VitaJPOverlay/dictionaries");
    vjo_snprintf(c->local_dictionaries, sizeof(c->local_dictionaries), "main.vjdict");
    c->non_japanese_filter = VJO_FILTER_LINES;
    c->font_size_ja = 18;
    c->font_size_en = 14;
    c->toggle_button = VJO_TRIGGER_L_R;
    c->subtitle_button = VJO_TRIGGER_SELECT_R;
    c->ocr_mode = VJO_OCR_AUTO;
    c->ocr_backend = VJO_OCR_LENS;
    vjo_snprintf(c->ocr_model_dir, sizeof(c->ocr_model_dir), "ux0:data/VitaJPOverlay/ocr");
    vjo_snprintf(c->anki_deck, sizeof(c->anki_deck), "Default");
    vjo_snprintf(c->anki_note_type, sizeof(c->anki_note_type), "Lapis");
    vjo_snprintf(c->anki_tags, sizeof(c->anki_tags), "vita-jp-overlay");
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++)
        vjo_snprintf(c->anki_field[i], sizeof(c->anki_field[i]), "%s", anki_fields[i].def);
}

const char *vjo_config_default_text(void)
{
    return "; Vita JP Overlay settings. Changes apply the next time the overlay opens.\n"
           "\n"
           "; Dictionary used for word lookups: hachidori | local | jpdb | jiten\n"
           "dictionary = hachidori\n"
           "\n"
           "; Hachidori relay computer: host[:port], default port 19633 (no API key)\n"
           "; Plain HTTP: use only on a trusted LAN. Anki and a sharing Hachidori host\n"
           "; with dictionaries must be running; enable Sharing > Also with my other computers.\n"
           "hachidori_host =\n"
           "\n"
           "; Local Yomitan dictionaries: convert on a computer, then copy .vjdict files here.\n"
           "local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries\n"
           "; Up to 8 filenames, separated by commas, in priority order.\n"
           "local_dictionaries = main.vjdict\n"
           "\n"
           "; jpdb.io API key (needed for dictionary = jpdb): jpdb.io -> Settings -> API key\n"
           "jpdb_api_key =\n"
           "\n"
           "; jiten.moe API key (needed for dictionary = jiten): jiten.moe -> Settings -> API key\n"
           "jiten_api_key =\n"
           "\n"
           "; Drop OCR lines without Japanese characters: none | lines\n"
           "non_japanese_filter = lines\n"
           "\n"
           "; Text size, 8-40: Japanese (sentence, words, readings) and English (meanings, messages)\n"
           "font_size_ja = 18\n"
           "font_size_en = 14\n"
           "\n"
           "; Opens/closes the overlay: a button combination, or a double tap on the rear\n"
           "; touchpad (buttons are hidden from the game; rear taps are not):\n"
           "; l+r | select | start | select+l | select+r | rear_double_tap\n"
           "toggle_button = l+r\n"
           "\n"
           "; Turns subtitles on/off: the recognized text in a strip at the top of the\n"
           "; screen while the game runs. Same values as toggle_button, but not the same\n"
           "; one. When one is part of the other (select and select+r), the shorter one\n"
           "; acts on release.\n"
           "subtitle_button = select+r\n"
           "\n"
           "; OCR: lens (online) | ncnn (experimental CPU-only PP-OCRv5 mobile)\n"
           "; ncnn requires the model download and a selected dialogue region; see docs/local-ocr.md.\n"
           "; Local OCR runs only on a button press, including subtitle refresh. No cloud fallback.\n"
           "ocr_backend = lens\n"
           "ocr_model_dir = ux0:data/VitaJPOverlay/ocr\n"
           "\n"
           "; auto: recognize text in the background when the region changes (Lens only)\n"
           "; on_press: recognize only when the overlay is opened\n"
           "ocr_mode = auto\n"
           "\n"
           "; Debugging: IP of a computer running tools/udp_log_listener.py (empty = off)\n"
           "log_host =\n"
           "\n"
           "; Debugging: write ux0:data/VitaJPOverlay/log.txt (max 256 KB + one rotated file): on | off\n"
           "log_file = off\n"
           "\n"
           "; ---- Anki (optional, see README) ----\n"
           "; × in the overlay adds the selected word to Anki through AnkiConnect on a computer.\n"
           "\n"
           "; The computer running Anki: empty = off, auto = search the local network,\n"
           "; or its IP address (optionally with :port, default 8765)\n"
           "anki_host =\n"
           "\n"
           "; Deck (created if missing), note type, and tags separated by spaces\n"
           "anki_deck = Default\n"
           "anki_note_type = Lapis\n"
           "anki_tags = vita-jp-overlay\n"
           "\n"
           "anki_field_word = Expression\n"
           "anki_field_reading = ExpressionReading\n"
           "anki_field_furigana = ExpressionFurigana\n"
           "anki_field_definition = MainDefinition\n"
           "anki_field_sentence = Sentence\n"
           "anki_field_picture = Picture\n"
           "anki_field_frequency = FreqSort\n"
           "anki_field_audio = ExpressionAudio\n"
           "\n"
           "; Word audio: a Yomitan audio source URL with {term} and {reading} (empty = off)\n"
           "anki_audio_url =\n";
}

/* host[:port], the n bytes at s: a name or IP address, and a port
 * (default_port when there is none). Returns 0, or -1 when invalid. */
static int host_port(const char *s, size_t n, char *host, size_t cap, int default_port, int *port)
{
    const char *colon = memchr(s, ':', n);
    size_t hn = colon ? (size_t)(colon - s) : n;
    int v = 0;
    if (hn == 0 || hn >= cap)
        return -1;
    for (size_t i = 0; i < hn; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' ||
              ch == '-'))
            return -1;
    }
    if (colon) {
        const char *p = colon + 1, *end = s + n;
        if (p == end)
            return -1;
        for (; p < end; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            v = v * 10 + (*p - '0');
            if (v > 65535)
                return -1;
        }
        if (v == 0)
            return -1;
    }
    *port = colon ? v : default_port;
    memcpy(host, s, hn);
    host[hn] = '\0';
    return 0;
}

int vjo_anki_endpoint(const char *setting, char *host, size_t cap, int *port)
{
    if (!*setting)
        return VJO_ANKI_OFF;
    if (vjo_ieq(setting, "auto"))
        return VJO_ANKI_AUTO;
    if (host_port(setting, strlen(setting), host, cap, VJO_ANKI_PORT, port) < 0)
        return -1;
    return VJO_ANKI_MANUAL;
}

int vjo_hachidori_endpoint(const char *setting, char *host, size_t cap, int *port)
{
    int rc;
    size_t label = 0;
    if (!setting || vjo_ieq(setting, "auto"))
        return -1;
    rc = vjo_anki_endpoint(setting, host, cap, port);
    if (rc != VJO_ANKI_MANUAL)
        return -1;
    /* DNS labels must not be empty or start/end with a hyphen. */
    for (size_t i = 0; ; i++) {
        char ch = host[i];
        if (ch == '.' || ch == '\0') {
            if (!label || host[i - 1] == '-')
                return -1;
            label = 0;
            if (!ch)
                break;
        } else {
            if (!label && ch == '-')
                return -1;
            label++;
        }
    }
    if (!strchr(setting, ':'))
        *port = VJO_HACHIDORI_PORT;
    return 0;
}

int vjo_anki_audio_endpoint(const char *url, char *host, int *port, int *tls, const char **path)
{
    size_t n = 0;
    int term = 0;
    if (!strncmp(url, "https://", 8)) {
        *tls = 1;
        url += 8;
    } else if (!strncmp(url, "http://", 7)) {
        *tls = 0;
        url += 7;
    } else {
        return -1;
    }
    while (url[n] && url[n] != '/' && url[n] != '?')
        n++;
    for (const char *s = url + n; *s; s++) {
        if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 0x7F || *s == '#')
            return -1; /* not one URL (a space, a fragment), or not ASCII */
        term |= !strncmp(s, "{term}", 6);
    }
    if (!term || host_port(url, n, host, VJO_HOST_MAX, *tls ? 443 : 80, port) < 0)
        return -1;
    *path = url + n;
    return 0;
}

static void warn(VjoConfig *c, const char *fmt, const char *a, const char *b)
{
    if (c->n_warnings >= VJO_CONFIG_MAX_WARNINGS)
        return;
    vjo_snprintf(c->warnings[c->n_warnings++], sizeof(c->warnings[0]), fmt, a, b);
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

static int parse_int(const char *s, int *out)
{
    long v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return -1;
        v = v * 10 + (*s - '0');
    }
    *out = (int)v;
    return 0;
}

int vjo_dict_find(const char *id)
{
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        if (vjo_ieq(id, dicts[i].id))
            return i;
    return -1;
}

/* VJO_DICT_* whose API key `key` sets, or -1. */
static int api_key_dict(const char *key)
{
    if (vjo_ieq(key, "api_key")) /* the old name of jpdb_api_key (tools/migrate_config.py RENAMED) */
        return VJO_DICT_JPDB;
    for (int i = 0; i < VJO_DICT_COUNT; i++)
        if (dicts[i].key_setting && vjo_ieq(key, dicts[i].key_setting))
            return i;
    return -1;
}

static int valid_hachidori_host(const char *v)
{
    char host[64];
    int port;
    return !*v || vjo_hachidori_endpoint(v, host, sizeof(host), &port) == 0;
}

static int valid_anki_host(const char *v)
{
    char host[64];
    int port;
    return vjo_anki_endpoint(v, host, sizeof(host), &port) >= 0;
}

static int valid_audio_url(const char *v)
{
    char host[VJO_HOST_MAX];
    const char *path;
    int port, tls;
    return !*v || vjo_anki_audio_endpoint(v, host, &port, &tls, &path) == 0;
}

/* Plain string settings. Deck, note type, tags, field names and the
 * audio URL may contain ';' and '#' (raw: no inline comment). */
typedef struct {
    const char *key;
    size_t offset, size;
    int required; /* an empty value keeps the default, with a warning */
    int raw;
    int (*valid)(const char *v); /* NULL = anything; an invalid value turns the setting off ("") */
    const char *expected;        /* the warning's description of a valid value */
    int quiet;                   /* the warning leaves out the value (long, or secret) */
} StrSetting;

#define STR_SETTING(name, required, raw) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), required, raw, NULL, NULL, 0}
#define CHECKED_SETTING(name, raw, valid, expected, quiet) \
    {#name, offsetof(VjoConfig, name), sizeof(((VjoConfig *)0)->name), 0, raw, valid, expected, quiet}
static const StrSetting str_settings[] = {
    CHECKED_SETTING(hachidori_host, 0, valid_hachidori_host, "host[:port], not a URL", 0),
    STR_SETTING(local_dictionary_dir, 1, 1),
    STR_SETTING(ocr_model_dir, 1, 1),
    CHECKED_SETTING(local_dictionaries, 0, vjo_local_names_valid, "1-8 relative .vjdict filenames separated by commas", 1),
    STR_SETTING(log_host, 0, 0),
    CHECKED_SETTING(anki_host, 0, valid_anki_host, "empty, auto, or IP[:port]", 0),
    STR_SETTING(anki_deck, 1, 1),
    STR_SETTING(anki_note_type, 1, 1),
    STR_SETTING(anki_tags, 0, 1),
    CHECKED_SETTING(anki_audio_url, 1, valid_audio_url, "empty, or http(s)://host[:port]/path with {term}", 1),
};
#define N_STR_SETTINGS ((int)(sizeof(str_settings) / sizeof(str_settings[0])))

static const StrSetting *find_str_setting(const char *key)
{
    for (int i = 0; i < N_STR_SETTINGS; i++)
        if (vjo_ieq(key, str_settings[i].key))
            return &str_settings[i];
    return NULL;
}

static int anki_field_index(const char *key)
{
    for (int i = 0; i < VJO_ANKI_FIELD_COUNT; i++)
        if (vjo_ieq(key, anki_fields[i].key))
            return i;
    return -1;
}

static int keeps_comment_chars(const char *key)
{
    const StrSetting *ss = find_str_setting(key);
    return (ss && ss->raw) || anki_field_index(key) >= 0;
}

static void set_str(VjoConfig *c, const char *key, const char *val, char *dst, size_t size)
{
    if (strlen(val) >= size)
        warn(c, "%s: value too long%s", key, "");
    else
        memcpy(dst, val, strlen(val) + 1);
}

static void set_font(VjoConfig *c, const char *key, const char *val, int *dst)
{
    int v;
    if (parse_int(val, &v) == 0 && v >= 8 && v <= 40)
        *dst = v;
    else
        warn(c, "%s: invalid value '%s' (8-40)", key, val);
}

static void set_trigger(VjoConfig *c, const char *key, const char *val, int *dst)
{
    for (int i = 0; i < VJO_TRIGGER_COUNT; i++) {
        if (vjo_ieq(val, trigger_names[i])) {
            *dst = i;
            return;
        }
    }
    warn(c, "%s: invalid value '%s'", key, val);
}

static void set_kv(VjoConfig *c, const char *key, const char *val)
{
    const StrSetting *ss;
    int d;
    if (vjo_ieq(key, "dictionary")) {
        d = vjo_dict_find(val);
        if (d >= 0)
            c->dictionary = d;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((d = api_key_dict(key)) >= 0) {
        set_str(c, key, val, c->api_key[d], sizeof(c->api_key[d]));
    } else if (vjo_ieq(key, "frequency_filter") || vjo_ieq(key, "font_size") || vjo_ieq(key, "hw_jpeg") ||
               vjo_ieq(key, "combo_delay_ms")) {
        /* removed settings (tools/migrate_config.py REMOVED): no warning */
    } else if (vjo_ieq(key, "non_japanese_filter")) {
        if (vjo_ieq(val, "lines"))
            c->non_japanese_filter = VJO_FILTER_LINES;
        else if (vjo_ieq(val, "none"))
            c->non_japanese_filter = VJO_FILTER_NONE;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "font_size_ja")) {
        set_font(c, key, val, &c->font_size_ja);
    } else if (vjo_ieq(key, "font_size_en")) {
        set_font(c, key, val, &c->font_size_en);
    } else if (vjo_ieq(key, "toggle_button")) {
        set_trigger(c, key, val, &c->toggle_button);
    } else if (vjo_ieq(key, "subtitle_button")) {
        set_trigger(c, key, val, &c->subtitle_button);
    } else if (vjo_ieq(key, "ocr_backend")) {
        if (vjo_ieq(val, "lens"))
            c->ocr_backend = VJO_OCR_LENS;
        else if (vjo_ieq(val, "ncnn"))
            c->ocr_backend = VJO_OCR_NCNN;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if (vjo_ieq(key, "ocr_mode")) {
        if (vjo_ieq(val, "auto"))
            c->ocr_mode = VJO_OCR_AUTO;
        else if (vjo_ieq(val, "on_press"))
            c->ocr_mode = VJO_OCR_ON_PRESS;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else if ((ss = find_str_setting(key)) != NULL) {
        char *dst = (char *)c + ss->offset;
        if (!*val && ss->required)
            warn(c, "%s: empty (keeping %s)", key, dst);
        else
            set_str(c, key, val, dst, ss->size);
        if (ss->valid && !ss->valid(dst)) {
            if (ss->quiet) {
                warn(c, "%s: invalid value (%s)", key, ss->expected);
            } else {
                char what[96];
                vjo_snprintf(what, sizeof(what), "'%s' (%s)", val, ss->expected);
                warn(c, "%s: invalid value %s", key, what);
            }
            dst[0] = '\0';
        }
    } else if ((d = anki_field_index(key)) >= 0) {
        set_str(c, key, val, c->anki_field[d], sizeof(c->anki_field[d]));
    } else if (vjo_ieq(key, "log_file")) {
        if (vjo_ieq(val, "on"))
            c->log_file = 1;
        else if (vjo_ieq(val, "off"))
            c->log_file = 0;
        else
            warn(c, "%s: invalid value '%s'", key, val);
    } else {
        warn(c, "unknown setting '%s'%s", key, "");
    }
}

void vjo_config_parse(VjoConfig *c, const char *text, size_t len)
{
    size_t i = 0;
    /* Skip a UTF-8 BOM (Notepad). */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        i = 3;
    while (i < len) {
        char line[1024], key[64], val[sizeof(((VjoConfig *)0)->local_dictionaries)]; /* the longest value */
        size_t n = 0, s, e, eq;
        int cut = 0;
        while (i < len && text[i] != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = text[i];
            else
                cut = 1;
            i++;
        }
        i++;
        line[n] = '\0';

        s = 0;
        while (s < n && is_space(line[s]))
            s++;
        if (s == n || line[s] == ';' || line[s] == '#')
            continue;
        {
            char *p = strchr(line + s, '=');
            if (!p) {
                warn(c, "ignored line '%s'%s", line + s, "");
                continue;
            }
            eq = (size_t)(p - line);
        }
        e = eq;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (e - s >= sizeof(key))
            e = s + sizeof(key) - 1;
        memcpy(key, line + s, e - s);
        key[e - s] = '\0';

        /* Inline comment in the value: ';' or '#' preceded by whitespace
         * (then a cut only shortened the comment). */
        if (!keeps_comment_chars(key)) {
            for (e = eq + 1; e < n; e++) {
                if ((line[e] == ';' || line[e] == '#') && is_space(line[e - 1])) {
                    n = e;
                    cut = 0;
                    break;
                }
            }
        }
        s = eq + 1;
        while (s < n && is_space(line[s]))
            s++;
        e = n;
        while (e > s && is_space(line[e - 1]))
            e--;
        if (cut || e - s >= sizeof(val)) {
            warn(c, "%s: value too long%s", key, "");
            continue;
        }
        memcpy(val, line + s, e - s);
        val[e - s] = '\0';
        set_kv(c, key, val);
    }
    /* The toggle keeps its button (it was there first). */
    if (c->toggle_button == c->subtitle_button) {
        c->subtitle_button = c->toggle_button == VJO_TRIGGER_SELECT_R ? VJO_TRIGGER_SELECT_L : VJO_TRIGGER_SELECT_R;
        warn(c, "toggle_button and subtitle_button are both %s: subtitle_button is %s",
             trigger_names[c->toggle_button], trigger_names[c->subtitle_button]);
    }
}
