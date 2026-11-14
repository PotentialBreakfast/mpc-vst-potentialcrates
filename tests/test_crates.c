/* The one-tap digs on the computer (no engine, no network): every crate names a real Discogs genre and style from
 * cratedig_tables.h, and ROLL at every wildness gives valid filters. Build: see .github/workflows/release.yml. */
#include "../src/crates.c"
#include <assert.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void valid(pc_t *s, const char *what) {
    for (int d = 0; d < NDIMS; d++) CHECK(s->dim[d] >= 0 && s->dim[d] < dim_count(s, d), "%s: dim %d = %d of %d", what, d, s->dim[d], dim_count(s, d));
}

int main(void) {
    pc_t *s = create(NULL);
    assert(s && !s->core);
    for (int c = 0; c < 8; c++)
        for (int k = 0; k < CRATES[c].n; k++) {
            const dig_t *d = &CRATES[c].digs[k];
            CHECK(genre_index(d->genre) > 0, "%s: no genre \"%s\"", CRATES[c].name, d->genre);
            CHECK(!d->style[0] || style_index(d->genre, d->style) > 0, "%s: no style \"%s\" in \"%s\"", CRATES[c].name, d->style, d->genre);
            CHECK(d->d0 >= 1 && d->d1 >= d->d0 && d->d1 < N_CRATEDIG_DECADES, "%s: decades %d..%d", CRATES[c].name, d->d0, d->d1);
        }
    for (int n = 0; n < (int)(sizeof DIG_GENRES / sizeof *DIG_GENRES); n++) CHECK(genre_index(DIG_GENRES[n]) > 0, "no genre \"%s\"", DIG_GENRES[n]);
    pthread_mutex_lock(&s->lk);
    for (int w = 0; w < 3; w++) {
        s->wild = w;
        for (int i = 0; i < 2000; i++) {
            roll_filters(s); valid(s, "roll");
            if (w < 2) CHECK(s->dim[DIM_GENRE] > 0 && s->dim[DIM_DECADE] > 0, "roll %d left genre or decade at ANY", w);
        }
    }
    for (int c = 0; c < 8; c++)
        for (int i = 0; i < 200; i++) {
            crate_filters(s, c); valid(s, "crate");
            CHECK(s->dim[DIM_GENRE] > 0, "crate %d: genre ANY", c);
        }
    start_dig(s, DIG_CRATE, 0);   /* no engine: must not crash */
    pthread_mutex_unlock(&s->lk);
    char buf[256];
    set_param(s, "crate_3", "1");
    set_param(s, "dice", "1");
    set_param(s, "res_1", "1");
    set_param(s, "state", "g=99;s=-4;d=3;r=9;c=200;src=1;ch=5;wild=7;sync=1;auto=0;vol=99");
    get_param(s, "state", buf, sizeof buf);
    printf("state after a bad restore: %s\n", buf);
    CHECK(strstr(buf, "wild=2") && strstr(buf, "vol=12"), "state not clamped: %s", buf);
    usleep(300000);
    get_param(s, "search_info", buf, sizeof buf);
    CHECK(strstr(buf, "ENGINE") != NULL, "no engine should say so: %s", buf);
    destroy(s);

    /* the engine finder: a plugin folder whose engine lost its exec marks (unzipped on Windows) gets them back */
    char tmpl[] = "/tmp/pc_test_XXXXXX", cmd[600], dir[800];
    assert(mkdtemp(tmpl));
    snprintf(cmd, sizeof cmd, "mkdir -p %s/Content/engine/bin/python3/bin && touch %s/Content/engine/bin/yt-dlp %s/Content/engine/bin/python3/bin/python3 "
             "&& chmod 644 %s/Content/engine/bin/yt-dlp && chmod 644 %s/Content/engine/bin/python3/bin/python3 && chmod 644 %s/Content/engine/bin/python3",
             tmpl, tmpl, tmpl, tmpl, tmpl, tmpl);
    assert(system(cmd) == 0);
    pc_t t = {0};
    snprintf(t.data_dir, sizeof t.data_dir, "%s", tmpl);
    char want[800];
    snprintf(want, sizeof want, "%s/Content/engine", tmpl);
    CHECK(!can_run(want), "the test engine should start without exec marks");
    CHECK(locate_engine(&t, dir, sizeof dir) && !strcmp(dir, want), "engine not found or not fixed: %s (%s)", dir, t.engine_error);
    pc_t u = {0};
    snprintf(u.data_dir, sizeof u.data_dir, "/nonexistent");
    CHECK(!locate_engine(&u, dir, sizeof dir) && !strcmp(u.engine_error, "ENGINE MISSING"), "missing engine: %s", u.engine_error);
    /* channels.txt: the first start copies an old Crate Digger's list from the same drive, else writes the how-to */
    {
        char drv[] = "/tmp/pc_drive_XXXXXX", me[700], ch[800], line[256];
        assert(mkdtemp(drv));
        snprintf(cmd, sizeof cmd, "mkdir -p '%s/VST2 Plugins/PotentialBreaks - VST - PotentialCrates' '%s/Synths-old/sd88me - VST - Crate Digger/cratedigger' "
                 "&& printf '# mine\\n@somechannel | Some Channel\\n' > '%s/Synths-old/sd88me - VST - Crate Digger/cratedigger/channels.txt'", drv, drv, drv);
        assert(system(cmd) == 0);
        pc_t c = {0};
        snprintf(me, sizeof me, "%s/VST2 Plugins/PotentialBreaks - VST - PotentialCrates", drv);
        snprintf(c.data_dir, sizeof c.data_dir, "%s/.", me);   /* as the wrapper gives it (MODULE_SUBDIR ".") */
        ensure_channels(&c);
        snprintf(ch, sizeof ch, "%s/channels.txt", me);
        CHECK(channels_in(ch) == 1, "the old Crate Digger's list was not copied");
        c.ch_mtime = -1; load_channels(&c);
        CHECK(c.nch == 1 && !strcmp(c.ch[0].name, "Some Channel"), "copied list: %d channels", c.nch);
        /* no old list anywhere: the how-to, with no channels yet */
        snprintf(cmd, sizeof cmd, "rm -rf '%s/Synths-old' '%s'", drv, ch);
        assert(system(cmd) == 0);
        ensure_channels(&c);
        FILE *f = fopen(ch, "r");
        CHECK(f && fgets(line, sizeof line, f) && strstr(line, "PotentialCrates"), "no how-to channels.txt written");
        if (f) fclose(f);
        CHECK(channels_in(ch) == 0, "the how-to should hold no live channels");
        /* a how-to with no channels yet: an old list found later still replaces it (1.1.2) */
        snprintf(cmd, sizeof cmd, "mkdir -p '%s/Synths-old/sd88me - VST - Crate Digger/cratedigger' && printf '@later | Later\\n' > '%s/Synths-old/sd88me - VST - Crate Digger/cratedigger/channels.txt'", drv, drv);
        assert(system(cmd) == 0);
        ensure_channels(&c);
        CHECK(channels_in(ch) == 1, "the how-to was not replaced by the old list");
        /* an existing list is never touched */
        snprintf(cmd, sizeof cmd, "printf '@kept\\n' > '%s'", ch);
        assert(system(cmd) == 0);
        ensure_channels(&c);
        CHECK(channels_in(ch) == 1, "an existing channels.txt was changed");
        snprintf(cmd, sizeof cmd, "rm -rf %s", drv);
        system(cmd);
    }
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
    system(cmd);
    printf(fails ? "FAILED (%d)\n" : "OK: 8 crates, 3 wildness levels, state, no-engine, engine finder\n", fails);
    return fails != 0;
}
