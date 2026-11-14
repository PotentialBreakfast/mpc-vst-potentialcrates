/* PotentialCrates: dig for records and play them into an MPC track.
 *
 * A PotentialSuite plugin (the kit's engine interface, wrapper/engine.h) around Crate Digger's streaming core
 * (src/core/yt_stream_plugin.c: Discogs search, yt-dlp resolve, ffmpeg decode; by sd88me and Charles Vestal, MIT).
 * What this file adds on top of Crate Digger's own front end, which it follows closely:
 *   - ROLL (MILD / MEDIUM / WILD): random filters, a search and the first playable result, in one tap
 *   - CRATES: eight one-tap digs (Drum Breaks, Library, Soundtracks, ...), each a few real Discogs genre / style / decade picks
 *   - AUTO NEXT and NEXT: a track that ends or won't play moves on to the next result
 *   - the three PotentialSuite looks (Print Plate, Night Rack, Colour Block) and the LOOK switch
 *
 * Threads: the core is called only under s->lk. A worker renders the core into a ring (never on MPC's audio thread, as
 * Crate Digger does), drives the one-tap digs and refreshes the screen text. render() only copies from the ring.
 * The engine bundle (yt-dlp, Python, ffmpeg) is the folder Content/engine/bin next to the plugin, as in Crate Digger 2. On a drive that
 * can't run programs (an exFAT SD card or USB stick) it is copied once to the internal drive, as Crate Digger 2 does (locate_engine). */
extern char **environ;
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#include "engine.h"
#include "../shared/looks/potential_theme.h"   /* the LOOK switch (parameter "theme") */
#include "../shared/potential_manual.h"        /* the MANUAL tab's page */
#include "core/plugin_api_v1.h"
#include "cratedig_tables.h"

extern plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

/* ---------------- parameters (same order as vst/params.json) ---------------- */
enum {
    P_NOW, P_STATUS, P_TIME, P_SEARCH_INFO,
    P_GENRE, P_STYLE, P_DECADE, P_REGION, P_COUNTRY, P_CHANNEL, P_CRATE_INFO,
    P_PLAY, P_STOP, P_BACK, P_FWD, P_NEXT, P_SEARCH, P_PAGE_UP, P_PAGE_DOWN,
    P_GENRE_PREV, P_GENRE_NEXT, P_STYLE_PREV, P_STYLE_NEXT, P_DECADE_PREV, P_DECADE_NEXT,
    P_REGION_PREV, P_REGION_NEXT, P_COUNTRY_PREV, P_COUNTRY_NEXT, P_CHANNEL_PREV, P_CHANNEL_NEXT,
    P_ANY, P_DICE, P_CRATE1, P_CRATE8 = P_CRATE1 + 7,
    P_SOURCE, P_WILD, P_SYNC, P_AUTO, P_VOLUME, P_THEME,
    P_RES1, P_RES8 = P_RES1 + 7,
    NP
};
static const char *const PKEY[NP] = {
    "now_playing", "status", "time", "search_info",
    "genre", "style", "decade", "region", "country", "channel", "crate_info",
    "play_pause", "stop", "back_15", "fwd_15", "next_track", "search", "page_up", "page_down",
    "genre_prev", "genre_next", "style_prev", "style_next", "decade_prev", "decade_next",
    "region_prev", "region_next", "country_prev", "country_next", "channel_prev", "channel_next",
    "any", "dice", "crate_1", "crate_2", "crate_3", "crate_4", "crate_5", "crate_6", "crate_7", "crate_8",
    "source", "wild", "sync", "auto_next", "volume", "theme",
    "res_1", "res_2", "res_3", "res_4", "res_5", "res_6", "res_7", "res_8",
};
static int find(const char *key) {
    for (int i = 0; i < NP; i++) if (!strcmp(key, PKEY[i])) return i;
    return -1;
}

#define BLOCK 128
#define RING 16384            /* frames, a power of two */
#define TARGET_FILL 2048      /* ~46 ms kept ahead of MPC */
#define SLOTS 8               /* result rows on a page */
#define MAX_CHANNELS 16
#define DIG_TRIES 3           /* a ROLL or CRATE that finds nothing digs again, up to this many times */
#define TXT 160
enum { DIM_GENRE, DIM_STYLE, DIM_DECADE, DIM_REGION, DIM_COUNTRY, NDIMS };
enum { SRC_DISCOGS, SRC_CHANNEL };
enum { DIG_NONE, DIG_ROLL, DIG_CRATE };

/* ---------------- the eight crates: real Discogs genre / style pairs, a decade range, optional search text ---------------- */
typedef struct { const char *genre, *style, *query; int d0, d1; } dig_t;   /* d0..d1: indexes into CRATEDIG_DECADES */
typedef struct { const char *name; const dig_t *digs; int n; } crate_t;
#define D60 2
#define D70 3
#define D80 4
static const dig_t C_BREAKS[] = {{"Funk / Soul", "Funk", NULL, D60, D70}, {"Funk / Soul", "Soul", NULL, D70, D70},
                                 {"Jazz", "Jazz-Funk", NULL, D70, D70}, {"Latin", "Boogaloo", NULL, D60, D70},
                                 {"Funk / Soul", "Afrobeat", NULL, D70, D70}};
static const dig_t C_LIBRARY[] = {{"Stage & Screen", "", "library", D60, D70}, {"Jazz", "", "library", D60, D70},
                                  {"Electronic", "", "library", D60, D80}};
static const dig_t C_SOUNDTRACKS[] = {{"Stage & Screen", "Soundtrack", NULL, D60, D80}, {"Stage & Screen", "Score", NULL, D60, D80}};
static const dig_t C_JAZZFUNK[] = {{"Jazz", "Jazz-Funk", NULL, D70, D70}, {"Jazz", "Soul-Jazz", NULL, D60, D70},
                                   {"Jazz", "Fusion", NULL, D70, D80}};
static const dig_t C_PSYCH[] = {{"Rock", "Psychedelic Rock", NULL, D60, D70}, {"Rock", "Krautrock", NULL, D70, D70},
                                {"Funk / Soul", "Psychedelic", NULL, D60, D70}};
static const dig_t C_BOSSA[] = {{"Latin", "Bossanova", NULL, D60, D70}, {"Latin", "Samba", NULL, D60, D70},
                                {"Latin", "MPB", NULL, D60, D70}, {"Jazz", "Bossa Nova", NULL, D60, D60}};
static const dig_t C_DISCO[] = {{"Funk / Soul", "Disco", NULL, D70, D80}, {"Funk / Soul", "Boogie", NULL, D80, D80}};
static const dig_t C_DUB[] = {{"Reggae", "Dub", NULL, D70, D80}, {"Reggae", "Roots Reggae", NULL, D70, D70},
                              {"Reggae", "Rocksteady", NULL, D60, D60}};
#define CRATE(name, a) {name, a, (int)(sizeof a / sizeof *a)}
static const crate_t CRATES[8] = {
    CRATE("DRUM BREAKS", C_BREAKS), CRATE("LIBRARY", C_LIBRARY), CRATE("SOUNDTRACKS", C_SOUNDTRACKS),
    CRATE("JAZZ-FUNK", C_JAZZFUNK), CRATE("PSYCH", C_PSYCH), CRATE("BOSSA & SAMBA", C_BOSSA),
    CRATE("DISCO & BOOGIE", C_DISCO), CRATE("DUB & REGGAE", C_DUB),
};
/* ROLL picks from these genres at MILD and MEDIUM (the ones a digger samples); WILD takes any genre. */
static const char *const DIG_GENRES[] = {"Funk / Soul", "Jazz", "Latin", "Rock", "Reggae", "Stage & Screen",
                                         "Electronic", "Folk, World, & Country", "Blues", "Hip Hop"};

typedef struct { char link[256], name[96]; } channel_t;

typedef struct {
    plugin_api_v2_t *api;
    void *core;                      /* NULL until the worker has found the engine, and when it isn't there (the computer test) */
    char engine_error[64];           /* why there's no engine (shown on STATUS) */
    int engine_ready;                /* the worker has looked */
    char module_dir[700], data_dir[700];
    pthread_mutex_t lk;              /* every core call and the dig state */
    pthread_mutex_t tx;              /* the screen text below */
    int16_t ring[RING * 2];
    uint32_t wpos, rpos;             /* __atomic */
    pthread_t th;
    int th_ok, run;

    /* settings (under lk) */
    int dim[NDIMS];
    int source, wild, sync, auto_next;
    float volume;                    /* dB */
    int page;
    char query[64];                  /* a crate's search text ("library") */
    channel_t ch[MAX_CHANNELS];
    int nch, chan;
    time_t ch_mtime;
    /* a one-tap dig in progress */
    int dig, dig_crate, dig_tries, await_search;
    time_t await_since;
    char crate_info[96];
    /* MPC transport, for SYNC */
    int hold, host_playing, edge;    /* edge: __atomic, +1 play / -1 stop, the worker takes it */
    unsigned seed;
    time_t quiet_until;              /* no row plays for 3 s after a project loads */
    long long row_ms[SLOTS];

    /* screen text (under tx), refreshed by the worker */
    char t_now[TXT], t_status[48], t_time[32], t_search[TXT], t_dim[NDIMS][64], t_chan[96], t_crate[96];
    char t_res[SLOTS][TXT];
    int res_on[SLOTS];
    unsigned display_rev;
    uint64_t pending;                /* params this engine changed itself (mpc_engine_changed) */
    pt_theme_t theme;
    pm_manual_t manual; /* the MANUAL tab's page: not part of the sound or the saved state */
} pc_t;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static unsigned rnd(pc_t *s) { s->seed = s->seed * 1664525u + 1013904223u; return s->seed >> 8; }
static int rnd_in(pc_t *s, int a, int b) { return b <= a ? a : a + (int)(rnd(s) % (unsigned)(b - a + 1)); }

/* ---------------- the core ---------------- */
static int core_get(pc_t *s, const char *key, char *buf, int n) {
    buf[0] = 0;
    if (!s->core) return 0;
    int r = s->api->get_param(s->core, key, buf, n);
    if (r <= 0) { buf[0] = 0; return 0; }
    if (r >= n) r = n - 1;
    buf[r] = 0;
    return r;
}
static int core_get_idx(pc_t *s, const char *fmt, int i, char *buf, int n) {
    char key[64];
    snprintf(key, sizeof key, fmt, i);
    return core_get(s, key, buf, n);
}
static void core_set(pc_t *s, const char *key, const char *val) { if (s->core) s->api->set_param(s->core, key, val); }
static int core_int(pc_t *s, const char *key) { char b[32]; core_get(s, key, b, sizeof b); return atoi(b); }

static void upper(char *t) { for (; *t; t++) *t = (char)toupper((unsigned char)*t); }
static void clip(char *dst, size_t n, const char *src, size_t max) {
    size_t l = strlen(src);
    if (max >= n) max = n - 1;
    if (l <= max) { snprintf(dst, n, "%s", src); return; }
    snprintf(dst, n, "%.*s...", (int)(max - 3), src);
}
static int has(const char *hay, const char *needle) { return strcasestr(hay, needle) != NULL; }

/* ---------------- filters ---------------- */
static int dim_count(pc_t *s, int d) {
    switch (d) {
    case DIM_GENRE: return N_CRATEDIG_GENRES;
    case DIM_STYLE: return styles_for_genre(CRATEDIG_GENRES[s->dim[DIM_GENRE]])->n;
    case DIM_DECADE: return N_CRATEDIG_DECADES;
    case DIM_REGION: return N_CRATEDIG_REGIONS;
    default: return N_COUNTRIES_BY_REGION[s->dim[DIM_REGION]];
    }
}
static const char *dim_value(pc_t *s, int d) {
    int i = s->dim[d];
    switch (d) {
    case DIM_GENRE: return CRATEDIG_GENRES[i];
    case DIM_STYLE: return styles_for_genre(CRATEDIG_GENRES[s->dim[DIM_GENRE]])->styles[i];
    case DIM_DECADE: return CRATEDIG_DECADES[i];
    case DIM_REGION: return i ? CRATEDIG_REGIONS[i] : "";
    default: return CRATEDIG_COUNTRIES_BY_REGION[s->dim[DIM_REGION]][i];
    }
}
static void set_dim(pc_t *s, int d, int i) {
    int n = dim_count(s, d);
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    s->dim[d] = i;
    if (d == DIM_GENRE) s->dim[DIM_STYLE] = 0;      /* the style list depends on the genre */
    if (d == DIM_REGION) s->dim[DIM_COUNTRY] = 0;   /* the country list on the region */
}
static int genre_index(const char *g) {
    for (int i = 0; i < N_CRATEDIG_GENRES; i++) if (!strcmp(CRATEDIG_GENRES[i], g)) return i;
    return 0;
}
static int style_index(const char *genre, const char *style) {
    const GenreStyles *gs = styles_for_genre(genre);
    for (int i = 0; i < gs->n; i++) if (!strcmp(gs->styles[i], style)) return i;
    return 0;
}

/* ---------------- channels.txt: "link-or-@handle-or-UC-id | Name", one a line ---------------- */
/* 1.1.1: with no channels.txt the CHANNEL source only said "add channels to channels.txt", and there was no file to
 * edit. Now the first start makes one in the plugin folder: a copy of the list an old Crate Digger on the same drive
 * already has (its folder, cratedigger/ or Content/), else a short how-to with commented-out examples. Updates keep it
 * (it is the release's user data). */
static const char CHANNELS_HOWTO[] =
    "# PotentialCrates: the YouTube channels for SOURCE: CHANNEL.\n"
    "# One per line, up to 16: a channel link, an @handle, a UC... channel ID or a playlist link,\n"
    "# then | and the name to show on the screen, if you like. Lines starting with # are skipped.\n"
    "# Updates keep this file. Edit it on your computer (MPC TOOLS can copy it over) and the plugin\n"
    "# picks up the change by itself.\n"
    "#\n"
    "#https://www.youtube.com/@yourchannel | Your Channel\n"
    "#@anotherchannel\n"
    "#https://www.youtube.com/playlist?list=PLxxxxxxxxxxxx | A Playlist\n";

static int channels_in(const char *path) {     /* how many channel lines a file has (0: none, or no file) */
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (*l && *l != '#' && *l != '\r' && *l != '\n') n++;
    }
    fclose(f);
    return n;
}
static int copy_text(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    char tmp[820];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    FILE *b = fopen(tmp, "wb");
    if (!b) { fclose(a); return 0; }
    char buf[4096];
    size_t r;
    int ok = 1;
    while ((r = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, r, b) != r) ok = 0;
    fclose(a);
    if (fclose(b) != 0) ok = 0;
    if (ok && rename(tmp, to) != 0) ok = 0;
    if (!ok) remove(tmp);
    return ok;
}
/* an old Crate Digger's list in folder `dir` (a plugins folder): "<dir>/<...Crate Digger...>/[cratedigger/|Content/]channels.txt" */
static int find_old_list(const char *dir, const char *self, char *out, size_t n) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || !(strcasestr(nm, "crate digger") || strcasestr(nm, "cratedigger") || strcasestr(nm, "channel-digger")))
            continue;
        static const char *const SUB[] = {"", "/cratedigger", "/Content"};
        for (int i = 0; i < 3 && !found; i++) {
            char p[1000];
            snprintf(p, sizeof p, "%s/%s%s/channels.txt", dir, nm, SUB[i]);
            if (strncmp(p, self, strlen(self)) && channels_in(p) > 0) { snprintf(out, n, "%s", p); found = 1; }
        }
    }
    closedir(d);
    return found;
}
static void ensure_channels(pc_t *s) {
    char path[800], parent[700], drive[700], from[1000] = "";
    struct stat sb;
    if (!s->data_dir[0]) return;
    snprintf(path, sizeof path, "%s/channels.txt", s->data_dir);
    if (stat(path, &sb) == 0) return;
    snprintf(parent, sizeof parent, "%s", s->data_dir);
    char *sl = strrchr(parent, '/');
    if (!sl || sl == parent) return;
    *sl = 0;                                              /* ".../VST2 Plugins" */
    snprintf(drive, sizeof drive, "%s", parent);
    sl = strrchr(drive, '/');
    if (sl && sl != drive) *sl = 0; else drive[0] = 0;    /* the drive: "/media/USB1" */
    int got = find_old_list(parent, s->data_dir, from, sizeof from);
    static const char *const OLD[] = {"Synths", "Synths-old", "Synths old", "VST2 Plugins"};
    for (int i = 0; i < 4 && !got && drive[0]; i++) {
        char d[800];
        snprintf(d, sizeof d, "%s/%s", drive, OLD[i]);
        got = find_old_list(d, s->data_dir, from, sizeof from);
    }
    if (got && copy_text(from, path)) return;
    FILE *f = fopen(path, "w");
    if (f) { fputs(CHANNELS_HOWTO, f); fclose(f); }
}

static void load_channels(pc_t *s) {
    char path[800];
    struct stat sb;
    snprintf(path, sizeof path, "%s/channels.txt", s->data_dir);
    time_t mt = stat(path, &sb) == 0 ? sb.st_mtime : 0;
    if (mt == s->ch_mtime) return;
    s->ch_mtime = mt;
    s->nch = 0;
    FILE *f = s->data_dir[0] ? fopen(path, "r") : NULL;
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f) && s->nch < MAX_CHANNELS) {
            char *l = line;
            while (*l == ' ' || *l == '\t') l++;
            l[strcspn(l, "\r\n")] = 0;
            if (!*l || *l == '#') continue;
            char *bar = strchr(l, '|'), *name = NULL;
            if (bar) { *bar = 0; name = bar + 1; while (*name == ' ') name++; }
            for (char *e = l + strlen(l); e > l && isspace((unsigned char)e[-1]); ) *--e = 0;
            if (!*l) continue;
            snprintf(s->ch[s->nch].link, sizeof s->ch[s->nch].link, "%s", l);
            snprintf(s->ch[s->nch].name, sizeof s->ch[s->nch].name, "%s", name && *name ? name : l);
            s->nch++;
        }
        fclose(f);
    }
    if (s->chan >= s->nch) s->chan = 0;
}

/* ---------------- search and play ---------------- */
static void json_put(char *o, size_t n, const char *v) {
    size_t k = strlen(o);
    for (; *v && k + 2 < n; v++) {
        if (*v == '"' || *v == '\\') o[k++] = '\\';
        o[k++] = *v;
    }
    o[k] = 0;
}
static void do_search(pc_t *s) {
    char j[1400] = "{\"genre\":\"";
    json_put(j, sizeof j, dim_value(s, DIM_GENRE));
    strcat(j, "\",\"style\":\""); json_put(j, sizeof j, dim_value(s, DIM_STYLE));
    strcat(j, "\",\"decade\":\""); json_put(j, sizeof j, dim_value(s, DIM_DECADE));
    strcat(j, "\",\"country\":\""); json_put(j, sizeof j, dim_value(s, DIM_COUNTRY));
    int chan = s->source == SRC_CHANNEL && s->chan < s->nch;
    strcat(j, "\",\"source\":\""); strcat(j, chan ? "channel" : "discogs");
    strcat(j, "\",\"query\":\""); json_put(j, sizeof j, chan ? "" : s->query);
    strcat(j, "\",\"channel\":\""); json_put(j, sizeof j, chan ? s->ch[s->chan].link : "");
    strcat(j, "\",\"min_len\":0,\"max_len\":0,\"music\":0}");
    s->page = 0;
    core_set(s, "cratedig_filter", j);
    s->await_since = time(NULL);
}

static int play_result(pc_t *s, int idx) {
    char url[1024], prov[32], n[16];
    core_get_idx(s, "search_result_url_%d", idx, url, sizeof url);
    if (!url[0]) return 0;
    core_get_idx(s, "search_result_provider_%d", idx, prov, sizeof prov);
    s->hold = s->sync;
    core_set(s, "hold_start", s->hold ? "1" : "0");
    core_set(s, "stream_provider", prov[0] ? prov : "youtube");
    core_set(s, "stream_url", url);
    snprintf(n, sizeof n, "%d", idx);
    core_set(s, "cratedig_result_index", n);
    s->page = idx / SLOTS;
    return 1;
}
static int current_result(pc_t *s) {
    char u[16];
    core_get(s, "stream_url", u, sizeof u);
    return u[0] ? core_int(s, "cratedig_result_index") : -1;
}

/* ---------------- ROLL and CRATES ---------------- */
static void say_dig(pc_t *s, const char *what) {
    char t[96] = "";
    for (int d = DIM_GENRE; d <= DIM_COUNTRY; d++) {
        const char *v = dim_value(s, d);
        if (!v[0] || d == DIM_REGION) continue;
        if (t[0]) strncat(t, "  /  ", sizeof t - strlen(t) - 1);
        strncat(t, v, sizeof t - strlen(t) - 1);
    }
    if (s->query[0]) { if (t[0]) strncat(t, "  /  ", sizeof t - strlen(t) - 1); strncat(t, s->query, sizeof t - strlen(t) - 1); }
    snprintf(s->crate_info, sizeof s->crate_info, "%s: %s", what, t[0] ? t : "ANYTHING");
    upper(s->crate_info);
}

static void roll_filters(pc_t *s) {
    int w = s->wild < 0 ? 0 : s->wild > 2 ? 2 : s->wild;
    int ng = (int)(sizeof DIG_GENRES / sizeof *DIG_GENRES);
    s->query[0] = 0;
    if (w == 0) {          /* MILD: stay in the genre you're in (or a digger's genre), a style of it, the 60s or 70s */
        if (!s->dim[DIM_GENRE]) set_dim(s, DIM_GENRE, genre_index(DIG_GENRES[rnd_in(s, 0, 5)]));
        else set_dim(s, DIM_GENRE, s->dim[DIM_GENRE]);
        set_dim(s, DIM_STYLE, rnd_in(s, 1, dim_count(s, DIM_STYLE) - 1));
        set_dim(s, DIM_DECADE, rnd_in(s, D60, D70));
        set_dim(s, DIM_REGION, 0);
    } else if (w == 1) {   /* MEDIUM: a digger's genre, any of its styles (sometimes all of them), the 50s to the 90s */
        set_dim(s, DIM_GENRE, genre_index(DIG_GENRES[rnd_in(s, 0, ng - 1)]));
        set_dim(s, DIM_STYLE, rnd(s) % 4 ? rnd_in(s, 1, dim_count(s, DIM_STYLE) - 1) : 0);
        set_dim(s, DIM_DECADE, rnd_in(s, 1, 5));
        set_dim(s, DIM_REGION, 0);
    } else {               /* WILD: any genre, style, decade, and somewhere in the world */
        set_dim(s, DIM_GENRE, rnd_in(s, 1, N_CRATEDIG_GENRES - 1));
        set_dim(s, DIM_STYLE, rnd_in(s, 0, dim_count(s, DIM_STYLE) - 1));
        set_dim(s, DIM_DECADE, rnd_in(s, 0, N_CRATEDIG_DECADES - 1));
        set_dim(s, DIM_REGION, rnd(s) % 2 ? rnd_in(s, 1, N_CRATEDIG_REGIONS - 1) : 0);
        set_dim(s, DIM_COUNTRY, s->dim[DIM_REGION] ? rnd_in(s, 0, dim_count(s, DIM_COUNTRY) - 1) : 0);
    }
    static const char *const W[3] = {"ROLL (MILD)", "ROLL (MEDIUM)", "ROLL (WILD)"};
    say_dig(s, W[w]);
}

static void crate_filters(pc_t *s, int c) {
    const crate_t *cr = &CRATES[c];
    const dig_t *d = &cr->digs[rnd(s) % (unsigned)cr->n];
    set_dim(s, DIM_GENRE, genre_index(d->genre));
    set_dim(s, DIM_STYLE, style_index(d->genre, d->style));
    set_dim(s, DIM_DECADE, rnd_in(s, d->d0, d->d1));
    set_dim(s, DIM_REGION, 0);
    snprintf(s->query, sizeof s->query, "%s", d->query ? d->query : "");
    say_dig(s, cr->name);
}

/* One tap: filters, a search, and (in the worker) the first playable result. A dig always searches Discogs. */
static void start_dig(pc_t *s, int kind, int crate) {
    s->dig = kind;
    s->dig_crate = crate;
    s->dig_tries = 0;
    if (s->source != SRC_DISCOGS) { s->source = SRC_DISCOGS; s->pending |= 1ull << P_SOURCE; }
    if (kind == DIG_ROLL) roll_filters(s); else crate_filters(s, crate);
    s->await_search = 1;
    do_search(s);
}

/* the worker, under lk: a dig's search has finished? Play from it, or dig again. */
static void dig_step(pc_t *s) {
    if (!s->await_search || !s->core) return;
    char st[32];
    core_get(s, "search_status", st, sizeof st);
    if (!strcmp(st, "searching") || !strcmp(st, "queued")) {
        if (time(NULL) - s->await_since > 90) { s->await_search = 0; s->dig = DIG_NONE; }
        return;
    }
    int n = core_int(s, "search_count");
    if (!strcmp(st, "done") && n > 0) {
        s->await_search = 0;
        if (s->dig != DIG_NONE) {
            int pick = s->dig == DIG_ROLL && n > 1 ? rnd_in(s, 0, n - 1 < 7 ? n - 1 : 7) : 0;
            play_result(s, pick);
        }
        s->dig = DIG_NONE;
        return;
    }
    /* nothing found (or an error): a one-tap dig tries other filters; a plain SEARCH just says so */
    if (s->dig != DIG_NONE && ++s->dig_tries < DIG_TRIES && strcmp(st, "error")) {
        if (s->dig == DIG_ROLL) roll_filters(s); else crate_filters(s, s->dig_crate);
        do_search(s);
        return;
    }
    s->await_search = 0;
    s->dig = DIG_NONE;
}

/* SYNC: MPC Play releases a waiting track (or resumes a paused one); MPC Stop pauses it. */
static void transport_edge(pc_t *s, int playing) {
    char st[32];
    core_get(s, "stream_status", st, sizeof st);
    if (playing) {
        if (s->hold) { s->hold = 0; core_set(s, "hold_start", "0"); }
        else if (!strcmp(st, "paused")) core_set(s, "play_pause_toggle", "1");
    } else if (!s->hold && (!strcmp(st, "streaming") || !strcmp(st, "buffering"))) {
        core_set(s, "play_pause_toggle", "1");
    }
}

/* ---------------- screen text (Crate Digger's wording for what went wrong) ---------------- */
static const char *stream_failure(const char *err, int dead) {
    if (!err[0]) return dead ? "UNAVAILABLE" : "";
    if (has(err, "yt-dlp is unavailable")) return "YT-DLP MISSING";
    if (has(err, "private")) return "PRIVATE VIDEO";
    if (has(err, "removed") || has(err, "been deleted") || has(err, "terminated")) return "VIDEO REMOVED";
    if (has(err, "copyright")) return "COPYRIGHT";
    if (has(err, "your country") || has(err, "geo")) return "REGION BLOCKED";
    if (has(err, "confirm your age") || has(err, "age-restricted") || has(err, "sign in") || has(err, "inappropriate for some"))
        return "AGE / LOGIN";
    if (has(err, "429") || has(err, "too many requests")) return "RATE LIMITED";
    if (has(err, "timed out") || has(err, "timeout")) return "TIMED OUT";
    if (has(err, "name or service") || has(err, "name resolution") || has(err, "unreachable") || has(err, "urlopen") ||
        has(err, "connection")) return "NO NETWORK";
    if (dead || has(err, "unavailable") || has(err, "not available")) return "UNAVAILABLE";
    return "WON'T PLAY";
}
static const char *tidy_error(const char *e) {   /* "ERROR: [youtube] abc: Video unavailable" -> "Video unavailable" */
    if (!strncmp(e, "ERROR: ", 7)) e += 7;
    if (e[0] == '[') { const char *c = strstr(e, ": "); if (c) e = c + 2; }
    return e;
}

static void make_text(pc_t *s, char now[TXT], char status[48], char tm[32], char search[TXT], char res[SLOTS][TXT], int on[SLOTS]) {
    char st[32], err[300], a[TXT], b[TXT];
    now[0] = status[0] = tm[0] = search[0] = 0;
    if (!s->core) {
        if (!s->engine_ready) {
            snprintf(status, 48, "STARTING");
            snprintf(search, TXT, "SETTING UP THE ENGINE (THE FIRST TIME ON A USB STICK OR SD CARD CAN TAKE A MINUTE)...");
        } else {
            snprintf(status, 48, "%s", s->engine_error[0] ? s->engine_error : "ENGINE MISSING");
            snprintf(search, TXT, "%s", !strcmp(s->engine_error, "ENGINE NOT RUNNABLE")
                     ? "THIS DRIVE CAN'T RUN THE ENGINE AND IT COULDN'T BE COPIED TO THE MPC: SEE THE README"
                     : "THE ENGINE FOLDER (CONTENT/ENGINE) ISN'T NEXT TO THE PLUGIN: INSTALL THE WHOLE ZIP AGAIN");
        }
        for (int r = 0; r < SLOTS; r++) { res[r][0] = 0; on[r] = 0; }
        return;
    }
    int cur = current_result(s);
    core_get(s, "stream_status", st, sizeof st);
    core_get(s, "stream_error", err, sizeof err);
    int failed = !strcmp(st, "eof") || !strcmp(st, "unavailable");
    if (failed) {
        const char *why = stream_failure(err, !strcmp(st, "unavailable"));
        snprintf(status, 48, "%s", why[0] ? why : "TRACK ENDED");
    } else if (s->hold) snprintf(status, 48, "WAITING FOR PLAY");
    else { snprintf(status, 48, "%s", st[0] ? st : "idle"); upper(status); }
    core_get(s, "playback_time", tm, 32);
    if (cur >= 0) {
        core_get_idx(s, "search_result_title_%d", cur, a, sizeof a);
        core_get_idx(s, "search_result_channel_%d", cur, b, sizeof b);
        if (failed && err[0]) {
            char t1[40], t2[80];
            clip(t1, sizeof t1, a, 30); clip(t2, sizeof t2, tidy_error(err), 56);
            snprintf(now, TXT, "%s  |  %s", t1, t2);
        } else if (b[0]) snprintf(now, TXT, "%s - %s", a, b);
        else snprintf(now, TXT, "%s", a);
    }
    /* the DIG line: progress, the count, or the reason */
    char ss[32];
    core_get(s, "search_status", ss, sizeof ss);
    int n = core_int(s, "search_count"), pages = n > 0 ? (n + SLOTS - 1) / SLOTS : 1;
    if (s->page > pages - 1) s->page = pages - 1;
    if (!strcmp(ss, "searching") || !strcmp(ss, "queued"))
        snprintf(search, TXT, s->dig_tries ? "NOTHING THERE - DIGGING AGAIN (%d / %d)..." : "DIGGING...", s->dig_tries + 1, DIG_TRIES);
    else if (!strcmp(ss, "done") && n > 0) snprintf(search, TXT, "%d RESULTS   -   PAGE %d / %d", n, s->page + 1, pages);
    else if (!strcmp(ss, "error") || !strcmp(ss, "no_results") || (!strcmp(ss, "done") && !n)) {
        core_get(s, "search_error", err, sizeof err);
        if (err[0]) { clip(search, TXT, err, 90); upper(search); }
        else snprintf(search, TXT, "%s", !strcmp(ss, "error") ? "SEARCH ERROR" : "NOTHING FOUND - TRY ROLL OR ANOTHER CRATE");
    } else snprintf(search, TXT, "TAP SEARCH TO DIG DISCOGS, OR PICK A CRATE");
    for (int r = 0; r < SLOTS; r++) {
        int idx = s->page * SLOTS + r;
        res[r][0] = 0;
        on[r] = idx == cur;
        if (idx >= n) continue;
        char t[TXT], c[96], y[16], d[16];
        core_get_idx(s, "search_result_title_%d", idx, t, sizeof t);
        core_get_idx(s, "search_result_channel_%d", idx, c, sizeof c);
        core_get_idx(s, "search_result_year_%d", idx, y, sizeof y);
        core_get_idx(s, "search_result_duration_%d", idx, d, sizeof d);
        snprintf(res[r], TXT, "%02d  %s%s%s%s%s%s%s", idx + 1, t[0] ? t : "(untitled)", c[0] ? " - " : "", c,
                 y[0] ? "  " : "", y, d[0] ? "  " : "", d);
    }
}

/* the worker, every 200 ms: new text? Tell MPC (display_rev). */
static void refresh_text(pc_t *s) {
    char now[TXT], status[48], tm[32], search[TXT], res[SLOTS][TXT], dims[NDIMS][64], chan[96], crate[96];
    int on[SLOTS];
    pthread_mutex_lock(&s->lk);
    load_channels(s);
    make_text(s, now, status, tm, search, res, on);
    for (int d = 0; d < NDIMS; d++) {
        const char *v = dim_value(s, d);
        snprintf(dims[d], 64, "%s", v[0] ? v : "ANY");
        upper(dims[d]);
    }
    if (!s->nch) snprintf(chan, sizeof chan, "NO CHANNELS YET - ADD SOME TO CHANNELS.TXT");
    else { clip(chan, sizeof chan, s->ch[s->chan].name, 60); upper(chan); }
    snprintf(crate, sizeof crate, "%s", s->crate_info[0] ? s->crate_info : "TAP A CRATE");
    pthread_mutex_unlock(&s->lk);

    pthread_mutex_lock(&s->tx);
    int changed = strcmp(now, s->t_now) || strcmp(status, s->t_status) || strcmp(tm, s->t_time) || strcmp(search, s->t_search) ||
                  strcmp(chan, s->t_chan) || strcmp(crate, s->t_crate) || memcmp(on, s->res_on, sizeof on);
    for (int d = 0; d < NDIMS && !changed; d++) changed = strcmp(dims[d], s->t_dim[d]) != 0;
    for (int r = 0; r < SLOTS && !changed; r++) changed = strcmp(res[r], s->t_res[r]) != 0;
    if (changed) {
        memcpy(s->t_now, now, TXT); memcpy(s->t_status, status, 48); memcpy(s->t_time, tm, 32); memcpy(s->t_search, search, TXT);
        memcpy(s->t_dim, dims, sizeof dims); memcpy(s->t_chan, chan, 96); memcpy(s->t_crate, crate, 96);
        memcpy(s->t_res, res, sizeof res); memcpy(s->res_on, on, sizeof on);
        s->display_rev++;
    }
    pthread_mutex_unlock(&s->tx);
}

static void start_engine(pc_t *s);

static void *worker(void *arg) {
    pc_t *s = arg;
    int16_t block[BLOCK * 2];
    long long next_ui = 0;
    ensure_channels(s);
    start_engine(s);
    while (__atomic_load_n(&s->run, __ATOMIC_ACQUIRE)) {
        long long t = now_ms();
        if (t >= next_ui) {
            next_ui = t + 200;
            pthread_mutex_lock(&s->lk);
            dig_step(s);
            pthread_mutex_unlock(&s->lk);
            refresh_text(s);
        }
        int edge = __atomic_exchange_n(&s->edge, 0, __ATOMIC_ACQ_REL);
        if (edge) {
            pthread_mutex_lock(&s->lk);
            if (s->sync) transport_edge(s, edge > 0);
            pthread_mutex_unlock(&s->lk);
        }
        uint32_t w = __atomic_load_n(&s->wpos, __ATOMIC_ACQUIRE), r = __atomic_load_n(&s->rpos, __ATOMIC_ACQUIRE);
        if (!s->core || w - r + BLOCK > TARGET_FILL) { usleep(2000); continue; }
        pthread_mutex_lock(&s->lk);
        s->api->render_block(s->core, block, BLOCK);
        pthread_mutex_unlock(&s->lk);
        for (int i = 0; i < BLOCK; i++) {
            uint32_t k = (w + (uint32_t)i) & (RING - 1);
            s->ring[k * 2] = block[i * 2];
            s->ring[k * 2 + 1] = block[i * 2 + 1];
        }
        __atomic_store_n(&s->wpos, w + BLOCK, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* ---------------- engine interface ---------------- */
static int has_bin(const char *d) {
    char p[800];
    struct stat sb;
    snprintf(p, sizeof p, "%s/bin/yt-dlp", d);
    return d[0] && stat(p, &sb) == 0;
}

static void apply_volume(pc_t *s);

/* ---------------- the engine bundle: where bin/yt-dlp, bin/python3, bin/ffmpeg live ---------------- */
static int can_run(const char *d) {
    char a[800], b[800];
    snprintf(a, sizeof a, "%s/bin/yt-dlp", d);
    snprintf(b, sizeof b, "%s/bin/python3", d);
    return d[0] && access(a, X_OK) == 0 && access(b, X_OK) == 0;
}
/* runs a program without fork() (posix_spawnp, as the core does inside MPC) and waits for it */
static int run(const char *const argv[]) {
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0) return 0;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
/* A copy that lost its "executable" marks (unzipped on Windows, copied file by file) gets them back. */
static void fix_modes(const char *d) {
    char bin[800];
    snprintf(bin, sizeof bin, "%s/bin", d);
    const char *argv[] = {"chmod", "-R", "a+rx", bin, NULL};
    run(argv);
}
/* The folder holding bin/, or 0 with s->engine_error set. On the worker: a copy can take a minute. Stamped with the engine
 * bundle's version, so a new bundle replaces an old copy. */
#define ENGINE_STAMP "engine-cd2.0.0"
static int locate_engine(pc_t *s, char *out, size_t n) {
    static const char *const SUB[] = {"Content/engine", "engine"};
    char src[800] = "";
    for (int i = 0; i < 2 && s->data_dir[0] && !src[0]; i++) {
        char d[800];
        snprintf(d, sizeof d, "%s/%s", s->data_dir, SUB[i]);
        if (has_bin(d)) snprintf(src, sizeof src, "%s", d);
    }
    if (!src[0]) { snprintf(s->engine_error, sizeof s->engine_error, "ENGINE MISSING"); return 0; }
    if (!can_run(src)) fix_modes(src);
    if (can_run(src)) { snprintf(out, n, "%s", src); return 1; }
    static const char *const BASE[] = {"/sdcard/vst/.potentialcrates", "/data/potentialcrates", "/tmp/potentialcrates"};
    for (int i = 0; i < 3; i++) {
        char dest[800], from[820];
        snprintf(dest, sizeof dest, "%s/%s", BASE[i], ENGINE_STAMP);
        if (can_run(dest)) { snprintf(out, n, "%s", dest); return 1; }
        const char *mk[] = {"mkdir", "-p", dest, NULL};
        if (!run(mk)) continue;
        snprintf(from, sizeof from, "%s/.", src);
        const char *cp[] = {"cp", "-a", from, dest, NULL};
        if (run(cp)) { fix_modes(dest); if (can_run(dest)) { snprintf(out, n, "%s", dest); return 1; } }
        const char *rm[] = {"rm", "-rf", dest, NULL};
        run(rm);
    }
    snprintf(s->engine_error, sizeof s->engine_error, "ENGINE NOT RUNNABLE");
    return 0;
}

/* the worker, once: find the engine (copying it if need be) and start the core */
static void start_engine(pc_t *s) {
    char dir[800];
    int ok = s->api && s->data_dir[0] && locate_engine(s, dir, sizeof dir);
    void *core = ok ? s->api->create_instance(dir, NULL) : NULL;
    pthread_mutex_lock(&s->lk);
    if (core) {
        snprintf(s->module_dir, sizeof s->module_dir, "%s", dir);
        s->core = core;
        core_set(s, "search_provider", "cratedig");
        core_set(s, "cratedig_auto_advance", s->auto_next ? "1" : "0");
        apply_volume(s);
    }
    s->engine_ready = 1;
    pthread_mutex_unlock(&s->lk);
}

static void apply_volume(pc_t *s) {
    char g[32];
    snprintf(g, sizeof g, "%.3f", pow(10.0, s->volume / 20.0));
    core_set(s, "gain", g);
}

static void *create(const char *data_dir) {
    pc_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pt_theme_init(&s->theme, data_dir);
    snprintf(s->data_dir, sizeof s->data_dir, "%s", data_dir ? data_dir : "");
    s->wild = 1;
    s->auto_next = 1;
    s->seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)s;
    s->quiet_until = time(NULL) + 3;
    s->ch_mtime = -1;
    pthread_mutex_init(&s->lk, NULL);
    pthread_mutex_init(&s->tx, NULL);
    /* A write to a dead child's pipe must not take MPC down (as Crate Digger): only if the host left SIGPIPE at the default. */
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
    s->api = move_plugin_init_v2(NULL);   /* the core itself starts on the worker (start_engine): finding the engine can take a while */
    snprintf(s->crate_info, sizeof s->crate_info, "TAP A CRATE");
    s->run = 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    s->th_ok = pthread_create(&s->th, &at, worker, s) == 0;
    pthread_attr_destroy(&at);
    return s;
}

static void destroy(void *inst) {
    pc_t *s = inst;
    __atomic_store_n(&s->run, 0, __ATOMIC_RELEASE);
    if (s->th_ok) pthread_join(s->th, NULL);
    if (s->core) s->api->destroy_instance(s->core);
    pthread_mutex_destroy(&s->lk);
    pthread_mutex_destroy(&s->tx);
    free(s);
}

static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }

static void render(void *inst, int16_t *out, int frames) {
    pc_t *s = inst;
    uint32_t r = __atomic_load_n(&s->rpos, __ATOMIC_ACQUIRE), avail = __atomic_load_n(&s->wpos, __ATOMIC_ACQUIRE) - r;
    for (int i = 0; i < frames; i++) {
        if ((uint32_t)i < avail) {
            uint32_t k = (r + (uint32_t)i) & (RING - 1);
            out[i * 2] = s->ring[k * 2];
            out[i * 2 + 1] = s->ring[k * 2 + 1];
        } else out[i * 2] = out[i * 2 + 1] = 0;   /* underrun: silence, never wait */
    }
    __atomic_store_n(&s->rpos, r + ((uint32_t)frames < avail ? (uint32_t)frames : avail), __ATOMIC_RELEASE);
}

static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    pc_t *s = inst;
    (void)ppq; (void)bpm; (void)frames;
    playing = playing != 0;
    if (playing != s->host_playing) {
        s->host_playing = playing;
        __atomic_store_n(&s->edge, playing ? 1 : -1, __ATOMIC_RELEASE);
    }
}

static void step_dim(pc_t *s, int d, int dir) {
    int n = dim_count(s, d);
    set_dim(s, d, (s->dim[d] + dir + n) % n);   /* arrows wrap: ANY <-> the last entry */
    s->query[0] = 0;                             /* your own filters now, not a crate's search text */
}

static int clampi(float v, int lo, int hi) { int i = (int)lroundf(v); return i < lo ? lo : i > hi ? hi : i; }

static void set_param(void *inst, const char *key, const char *val) {
    pc_t *s = inst;
    if (pt_theme_set(&s->theme, key, val)) return;
    if (pm_set(&s->manual, key, val)) return;
    if (!strncmp(key, "res_", 4)) {   /* a result row: any tap (the row toggles either way) plays it */
        int row = atoi(key + 4) - 1;
        if (row < 0 || row >= SLOTS || time(NULL) < s->quiet_until) return;
        long long t = now_ms();
        if (t - s->row_ms[row] < 400) return;   /* one tap can arrive twice */
        s->row_ms[row] = t;
        pthread_mutex_lock(&s->lk);
        int idx = s->page * SLOTS + row;
        if (idx < core_int(s, "search_count")) { s->dig = DIG_NONE; s->await_search = 0; play_result(s, idx); }
        pthread_mutex_unlock(&s->lk);
        return;
    }
    if (!strcmp(key, "state")) {   /* "g=3;s=5;d=3;r=0;c=0;src=0;ch=0;wild=1;sync=0;auto=1;vol=0" */
        s->quiet_until = time(NULL) + 3;
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", val);
        int v[5] = {0, 0, 0, 0, 0};
        pthread_mutex_lock(&s->lk);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            const char *x = eq + 1;
            static const char *const DK[NDIMS] = {"g", "s", "d", "r", "c"};
            for (int d = 0; d < NDIMS; d++) if (!strcmp(tok, DK[d])) v[d] = atoi(x);
            if (!strcmp(tok, "src")) s->source = atoi(x) == SRC_CHANNEL ? SRC_CHANNEL : SRC_DISCOGS;
            else if (!strcmp(tok, "ch")) s->chan = atoi(x) < 0 ? 0 : atoi(x);
            else if (!strcmp(tok, "wild")) s->wild = clampi((float)atof(x), 0, 2);
            else if (!strcmp(tok, "sync")) s->sync = atoi(x) == 1;
            else if (!strcmp(tok, "auto")) s->auto_next = atoi(x) != 0;
            else if (!strcmp(tok, "vol")) { float f = (float)atof(x); s->volume = f < -24 ? -24 : f > 12 ? 12 : f; }
        }
        set_dim(s, DIM_GENRE, v[0]); set_dim(s, DIM_STYLE, v[1]); set_dim(s, DIM_DECADE, v[2]);
        set_dim(s, DIM_REGION, v[3]); set_dim(s, DIM_COUNTRY, v[4]);
        core_set(s, "cratedig_auto_advance", s->auto_next ? "1" : "0");
        apply_volume(s);
        pthread_mutex_unlock(&s->lk);
        return;
    }
    int i = find(key);
    if (i < 0) return;
    float v = (float)atof(val);
    int press = v > 0.5f;
    pthread_mutex_lock(&s->lk);
    switch (i) {
    case P_PLAY:
        if (!press) break;
        if (s->hold) { s->hold = 0; core_set(s, "hold_start", "0"); break; }   /* start now, without waiting for MPC's Play */
        { char u[16]; core_get(s, "stream_url", u, sizeof u);
          if (!u[0]) play_result(s, s->page * SLOTS); else core_set(s, "play_pause_step", "trigger"); }
        break;
    case P_STOP: if (press) core_set(s, "stop_step", "trigger"); break;
    case P_BACK: if (press) core_set(s, "rewind_15_step", "trigger"); break;
    case P_FWD: if (press) core_set(s, "forward_15_step", "trigger"); break;
    case P_NEXT: if (press) core_set(s, "next_track_step", "trigger"); break;
    case P_SEARCH: if (press) { s->dig = DIG_NONE; s->await_search = 0; snprintf(s->crate_info, sizeof s->crate_info, "YOUR FILTERS"); do_search(s); } break;
    case P_PAGE_UP: case P_PAGE_DOWN:
        if (press) {
            int n = core_int(s, "search_count"), pages = n > 0 ? (n + SLOTS - 1) / SLOTS : 1;
            s->page = (s->page + (i == P_PAGE_DOWN ? 1 : -1) + pages) % pages;
        }
        break;
    case P_GENRE_PREV: case P_GENRE_NEXT: case P_STYLE_PREV: case P_STYLE_NEXT: case P_DECADE_PREV: case P_DECADE_NEXT:
    case P_REGION_PREV: case P_REGION_NEXT: case P_COUNTRY_PREV: case P_COUNTRY_NEXT:
        if (press) step_dim(s, (i - P_GENRE_PREV) / 2, (i - P_GENRE_PREV) % 2 ? 1 : -1);
        break;
    case P_CHANNEL_PREV: case P_CHANNEL_NEXT:
        if (press && s->nch) s->chan = (s->chan + (i == P_CHANNEL_NEXT ? 1 : -1) + s->nch) % s->nch;
        break;
    case P_ANY:
        if (press) { for (int d = 0; d < NDIMS; d++) s->dim[d] = 0; s->query[0] = 0; }
        break;
    case P_DICE: if (press) start_dig(s, DIG_ROLL, 0); break;
    case P_SOURCE: s->source = clampi(v, 0, 1); break;
    case P_WILD: s->wild = clampi(v, 0, 2); break;
    case P_SYNC:
        s->sync = clampi(v, 0, 1);
        if (!s->sync && s->hold) { s->hold = 0; core_set(s, "hold_start", "0"); }
        break;
    case P_AUTO: s->auto_next = clampi(v, 0, 1); core_set(s, "cratedig_auto_advance", s->auto_next ? "1" : "0"); break;
    case P_VOLUME: s->volume = v < -24 ? -24 : v > 12 ? 12 : v; apply_volume(s); break;
    default:
        if (i >= P_CRATE1 && i <= P_CRATE8 && press) start_dig(s, DIG_CRATE, i - P_CRATE1);
        break;
    }
    pthread_mutex_unlock(&s->lk);
    /* the screen follows a tap at once, not at the next 200 ms tick */
    pthread_mutex_lock(&s->tx); s->display_rev++; pthread_mutex_unlock(&s->tx);
}

int mpc_engine_changed(void *inst, char *key, int len) {
    pc_t *s = inst;
    for (int i = 0; i < NP; i++)
        if (s->pending & (1ull << i)) { s->pending &= ~(1ull << i); snprintf(key, len, "%s", PKEY[i]); return 1; }
    return 0;
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    pc_t *s = inst;
    { int n = pt_theme_get(&s->theme, key, buf, buf_len); if (n) return n; }
    { int pm_n = pm_get(&s->manual, key, buf, buf_len); if (pm_n) return pm_n; }
    if (!strcmp(key, "display_rev")) {
        pthread_mutex_lock(&s->tx);
        int n = snprintf(buf, buf_len, "%u", s->display_rev);
        pthread_mutex_unlock(&s->tx);
        return n;
    }
    if (!strncmp(key, "res_", 4)) {   /* "res_3": the 3rd row of the page; "res_3_on": is it the one playing */
        int row = atoi(key + 4) - 1, on = strstr(key, "_on") != NULL;
        if (row < 0 || row >= SLOTS) return 0;
        pthread_mutex_lock(&s->tx);
        int n = on ? snprintf(buf, buf_len, "%d", s->res_on[row]) : snprintf(buf, buf_len, "%s", s->t_res[row][0] ? s->t_res[row] : " ");
        pthread_mutex_unlock(&s->tx);
        return n;
    }
    if (!strcmp(key, "state")) {
        pthread_mutex_lock(&s->lk);
        int n = snprintf(buf, buf_len, "g=%d;s=%d;d=%d;r=%d;c=%d;src=%d;ch=%d;wild=%d;sync=%d;auto=%d;vol=%g",
                         s->dim[0], s->dim[1], s->dim[2], s->dim[3], s->dim[4], s->source, s->chan, s->wild, s->sync,
                         s->auto_next, (double)s->volume);
        pthread_mutex_unlock(&s->lk);
        return n < buf_len ? n : buf_len - 1;
    }
    int i = find(key);
    if (i < 0) return 0;
    const char *txt = NULL;
    int n = 0;
    pthread_mutex_lock(&s->tx);
    switch (i) {
    case P_NOW: txt = s->t_now[0] ? s->t_now : "-"; break;
    case P_STATUS: txt = s->t_status; break;
    case P_TIME: txt = s->t_time[0] ? s->t_time : "-"; break;
    case P_SEARCH_INFO: txt = s->t_search; break;
    case P_GENRE: case P_STYLE: case P_DECADE: case P_REGION: case P_COUNTRY: txt = s->t_dim[i - P_GENRE]; break;
    case P_CHANNEL: txt = s->t_chan; break;
    case P_CRATE_INFO: txt = s->t_crate; break;
    default: break;
    }
    if (txt) n = snprintf(buf, buf_len, "%s", txt[0] ? txt : " ");
    pthread_mutex_unlock(&s->tx);
    if (txt) return n;
    switch (i) {
    case P_SOURCE: return snprintf(buf, buf_len, "%d", s->source);
    case P_WILD: return snprintf(buf, buf_len, "%d", s->wild);
    case P_SYNC: return snprintf(buf, buf_len, "%d", s->sync);
    case P_AUTO: return snprintf(buf, buf_len, "%d", s->auto_next);
    case P_VOLUME: return snprintf(buf, buf_len, "%g", (double)s->volume);
    default: return snprintf(buf, buf_len, "0");   /* the buttons */
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
