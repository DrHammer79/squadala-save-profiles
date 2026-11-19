/* SPDX-License-Identifier: GPL-3.0-or-later
   Save Profiles: an N64-style file select (30 files, 5 pages of 6) for the OoT co-op kit's Longshot game, as a co-op code mod.
   The idea and the perfil<N> folders come from zelda-oot-perfis by juvxncio (GPL-3.0); this is a new implementation
   for the co-op module's mod interface (coop_mod_api.h), not a port of its Switch code.

   How it works
   - The game's save folder (<game>\user\save\<user id>) always holds the file being played: account.sav, option.sav
     and the slot_NN folders, exactly as without this mod. The other files are parked in perfil<N> folders inside it
     (zelda-oot-perfis keeps its profiles 2 to 9 there too; file 1 is parked in perfil1 while another one is played).
   - Code mods start at the game's first frame, before the game's save thread (gmd::SaveDataMgr) reads anything. This
     mod pauses that thread while it is idle in a wait, shows the file select over the title, moves the chosen file into
     the save folder, and lets the thread go: the game then reads the chosen file as the only one there is. No restart.
   - The game, Longshot and co-op all see one ordinary save folder, so co-op needs nothing new: the host's save sent to
     guests is the chosen file, and a guest's co-op folder (<save folder>-coop) works as before.
   - profiles.txt in the save folder says which file is there (active=N). A move writes moving=A>B and step=1|2 first;
     a move the game stopped in the middle of is finished at the next start. Nothing is ever deleted: a name already
     taken at the destination is renamed aside (.conflict-<time>). The first move ever copies the save folder into
     profiles-backup\<time> first.
   - Two games on one save folder (Nucleus Coop's instances): moves are serialized by a named mutex on the folder's real
     path, and a file that another running game plays from cannot be swapped out from under it.
   - If the game stops drawing while its save thread is paused (it waits for its saves), the thread is let go after a
     few seconds and the game plays the file that is there: never a hang.
   - With co-op's mod interface 2 (the game's menu screens) the game starts to its own title as usual, and the title's
     Continue opens this file select over it (A on Continue is the mod's: co-op holds it from the game). A on a file
     moves the files if it is another one, has the game read its save list again, and opens that file's list of saves
     the game's own way. B goes back to the title; B on the save list comes back to this file select. While the file
     select is up, the game gets no input. With interface 1 the file select shows at the start, before the title, as in
     Save Profiles 1.0, and the title is the player's after a pick. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <io.h>
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_mod_api.h"

#define FILES 30        /* 5 pages of 6 (Save Profiles 1.0 and 1.1 had 7: their perfil<N> folders stay as they are) */
#define PER_PAGE 6
#define PAGES (FILES / PER_PAGE)
#define MOD_VERSION "1.2.0"
#define EXPORT_DIR "Squadala saves"   /* in the player's Documents: exported files, and where Import looks */
#define EXPORT_NOTE "save-profiles.txt"   /* in an exported file: its name, for Import */
#define SAVE_THREAD L"gmd::SaveDataMgr"
#define STATE_FILE "profiles.txt"
#define BACKUP_DIR "profiles-backup"
#define NAME_MAX 12      /* a file's name, as the name plate holds it */
#define STALL_MS 4000   /* no frame for this long while paused: the game waits for its saves, let the thread go */

typedef int (*RootFn)(char *out, size_t cap);
typedef int (*PadReadFn)(int n, uint64_t *buttons, int32_t stick[4]);
typedef HRESULT(WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);

static const CoopModApi *api;
static PadReadFn pad_read;
static int pad_choice;              /* LICHII_COOP_PAD: this game's pad (0: every pad) */
static char canon[MAX_PATH];        /* the game's save folder */
static char folder_key[17];         /* its real path's hash: the names of the mutexes */
static int active = 1;              /* the file in the save folder now */
static HANDLE live_marker;

enum { ST_OFF, ST_PICKING, ST_DONE };
static volatile LONG state = ST_OFF;
static CRITICAL_SECTION pick_lock;
static HANDLE save_thread;
static volatile LONG frames;
static volatile LONG cursor;        /* 0 .. FILES-1 */
static volatile LONG key_actions;   /* from on_key (the window's thread), taken in frame */
static char message[200];
static volatile DWORD message_until;
static char toast[96];
static volatile DWORD toast_until;
static int test_pick;               /* SAVE_PROFILES_TEST_PICK=<file>[,<ms>]: a test run picks by itself */
/* SAVE_PROFILES_TEST_CLICKS=x,y;x,y... (or / between): a test run's left clicks, as fractions of the picture, one each 1.5 s from 3 s
   after the file select comes up (the mouse's own path from where the pointer is: its hit test and its actions) */
#define TEST_CLICKS_MAX 24
static float test_click_x[TEST_CLICKS_MAX], test_click_y[TEST_CLICKS_MAX];
static int test_clicks_n, test_click_i;
static DWORD test_pick_ms = 3000, picking_since;

/* (mod interface 2) The game's own load list after a pick, and B on it back to the file select */
#define CONTINUE_BUTTON "Pa_Button_00"   /* the title's Continue (its first button, seen in a game) */
#define LIST_TRY_MS 20000   /* a pick's list not opened this long after the title takes input: the title is the player's */
#define HOLD_AFTER_MS 3000  /* the game's input stays held this long at most at the title, while a pick's list opens */
static int v2;                       /* the module serves interface 2 */
static int debug_screens;            /* SAVE_PROFILES_DEBUG=1: the screens the mod sees, in the log */
static volatile LONG title_pick;     /* the file select is up over the title (B on the load list) */
static volatile LONG want_list;      /* a file was picked: open its load list at the title */
static volatile LONG need_reload;    /* the files moved at the title: the game reads its save list first */
static volatile DWORD title_since;   /* when the title first took input after a pick (0: not yet; the game may load for
                                        longer than LIST_TRY_MS after a pick at the start) */
static volatile DWORD list_ms, question_ms;   /* the load list's and its question's last updates (0: none yet) */
static volatile DWORD list_asked;
/* The game's own menu sounds for the file select (mod interface 2's play_sound): asked on the frame's thread, started
   at the title's next update (the calls work only there) */
#define SND_CURSOR 1
#define SND_DECIDE 2
#define SND_CONTINUE 4
static volatile LONG want_sounds;   /* when the mod chose Continue for a pick's list (0: the list came, or none asked) */
static volatile LONG reprime;        /* the next frame's buttons are the base (none counts as pressed) */

typedef struct {
    int used;
    char place[72];
    char when[48];
    char day[16];   /* ("Nov 14") */
} Info;
static Info info[FILES + 1];
static char names[FILES + 1][NAME_MAX + 1];   /* the files' names (profiles.txt name<N>=) */

/* --- log ------------------------------------------------------------------------------------------------------ */

static void say(const char *fmt, ...) {
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (api) api->log(api, line);
}

/* --- paths and the state file --------------------------------------------------------------------------------- */

static void park_dir(int n, char *out, size_t cap) { snprintf(out, cap, "%s\\perfil%d", canon, n); }

/* Ours, never moved with a file: the parked files, the state file, the backups (and zelda-oot-perfis' own
   perfil_ativo.txt, which an emulator save copied here may hold) */
static int is_ours(const char *name) {
    return !_strnicmp(name, "perfil", 6) || !_strnicmp(name, "profiles.txt", 12) || !_strnicmp(name, BACKUP_DIR, 15) ||
           !_stricmp(name, EXPORT_NOTE);
}

static int has_items(const char *dir) {
    char pat[MAX_PATH + 4];
    WIN32_FIND_DATAA f;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int any = 0;
    do {
        if (strcmp(f.cFileName, ".") && strcmp(f.cFileName, "..") && !is_ours(f.cFileName)) any = 1;
    } while (!any && FindNextFileA(h, &f));
    FindClose(h);
    return any;
}

/* Every item of `from` that is not ours, into `to` (made if missing). The number moved, -1 when one could not go. */
static int move_items(const char *from, const char *to) {
    char pat[MAX_PATH + 4];
    static char names[64][MAX_PATH];
    int n = 0, moved = 0;
    WIN32_FIND_DATAA f;
    snprintf(pat, sizeof pat, "%s\\*", from);
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..") || is_ours(f.cFileName)) continue;
        if (n < 64) snprintf(names[n++], MAX_PATH, "%s", f.cFileName);
    } while (FindNextFileA(h, &f));
    FindClose(h);
    if (!n) return 0;
    CreateDirectoryA(to, NULL);
    for (int i = 0; i < n; i++) {
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof a, "%s\\%s", from, names[i]);
        snprintf(b, sizeof b, "%s\\%s", to, names[i]);
        if (GetFileAttributesA(b) != INVALID_FILE_ATTRIBUTES) {
            char aside[MAX_PATH];
            snprintf(aside, sizeof aside, "%s.conflict-%lu", b, (unsigned long)GetTickCount());
            say("%s was already there: kept as %s", b, aside);
            MoveFileExA(b, aside, MOVEFILE_WRITE_THROUGH);
        }
        if (!MoveFileExA(a, b, MOVEFILE_WRITE_THROUGH)) {
            say("could not move %s to %s (error %lu)", a, b, (unsigned long)GetLastError());
            return -1;
        }
        moved++;
    }
    return moved;
}

typedef struct {
    int active, from, to, step;
} State;

/* A name as typed: printable ASCII, NAME_MAX at most, no spaces at the ends */
static void clean_name(const char *in, char *out) {
    size_t k = 0;
    while (*in == ' ') in++;
    for (; *in && *in != '\r' && *in != '\n' && k < NAME_MAX; in++)
        if (*in >= 0x20 && *in < 0x7f) out[k++] = *in;
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

/* profiles.txt: the file in the save folder, a move in progress, and the files' names (into names[]) */
static State read_state(void) {
    State s = {1, 0, 0, 0};
    char p[MAX_PATH], line[128];
    snprintf(p, sizeof p, "%s\\" STATE_FILE, canon);
    memset(names, 0, sizeof names);
    FILE *f = fopen(p, "r");
    if (!f) return s;
    while (fgets(line, sizeof line, f)) {
        int a, b;
        const char *eq = strchr(line, '=');
        if (sscanf(line, "active=%d", &a) == 1 && a >= 1 && a <= FILES) s.active = a;
        else if (sscanf(line, "moving=%d>%d", &a, &b) == 2 && a >= 1 && a <= FILES && b >= 1 && b <= FILES) s.from = a, s.to = b;
        else if (sscanf(line, "step=%d", &a) == 1) s.step = a;
        else if (!strncmp(line, "name", 4) && eq && (a = atoi(line + 4)) >= 1 && a <= FILES) clean_name(eq + 1, names[a]);
    }
    fclose(f);
    return s;
}

/* The state on disk, with the names as they are in memory now (they are written back so) */
static State read_state_keep_names(void) {
    char keep[sizeof names];
    memcpy(keep, names, sizeof names);
    const State s = read_state();
    memcpy(names, keep, sizeof names);
    return s;
}

static int write_state(State s) {
    char p[MAX_PATH], t[MAX_PATH];
    snprintf(p, sizeof p, "%s\\" STATE_FILE, canon);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    if (!f) return 0;
    fprintf(f, "# Save Profiles (mods\\save-profiles): the file in this save folder now. The others are in perfil<N>.\n"
               "# Do not edit while the game runs.\n");
    fprintf(f, "active=%d\n", s.active);
    if (s.from) fprintf(f, "moving=%d>%d\nstep=%d\n", s.from, s.to, s.step);
    for (int n = 1; n <= FILES; n++)
        if (names[n][0]) fprintf(f, "name%d=%s\n", n, names[n]);
    fflush(f);
    _commit(_fileno(f));
    fclose(f);
    return MoveFileExA(t, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

/* --- the folder's lock and who plays from it ------------------------------------------------------------------ */

static void make_folder_key(void) {
    char real[MAX_PATH * 2] = "";
    HANDLE h = CreateFileA(canon, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {   /* (the real path: Nucleus Coop's instances reach one folder through links) */
        if (!GetFinalPathNameByHandleA(h, real, sizeof real, 0)) real[0] = 0;
        CloseHandle(h);
    }
    if (!real[0]) snprintf(real, sizeof real, "%s", canon);
    uint64_t x = 1469598103934665603ULL;
    for (const char *c = real; *c; c++) {
        char ch = *c;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        x = (x ^ (unsigned char)ch) * 1099511628211ULL;
    }
    snprintf(folder_key, sizeof folder_key, "%016llx", (unsigned long long)x);
}

static HANDLE folder_lock(void) {
    char name[96];
    snprintf(name, sizeof name, "Local\\SaveProfiles-%s", folder_key);
    HANDLE m = CreateMutexA(NULL, FALSE, name);
    if (!m) return NULL;
    const DWORD w = WaitForSingleObject(m, 5000);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        CloseHandle(m);
        return NULL;
    }
    return m;
}

static void folder_unlock(HANDLE m) {
    if (!m) return;
    ReleaseMutex(m);
    CloseHandle(m);
}

/* This game plays from the folder now: other games see it (other_live) until it ends */
static void mark_live(void) {
    if (live_marker) return;
    char name[112];
    snprintf(name, sizeof name, "Local\\SaveProfilesLive-%s-%lu", folder_key, (unsigned long)GetCurrentProcessId());
    live_marker = CreateMutexA(NULL, FALSE, name);
}

/* Another running game that plays from this folder: its process id, else 0 */
static DWORD other_live(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 p;
    p.dwSize = sizeof p;
    DWORD found = 0;
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32First(snap, &p); ok && !found; ok = Process32Next(snap, &p)) {
        if (p.th32ProcessID == self) continue;
        char name[112];
        snprintf(name, sizeof name, "Local\\SaveProfilesLive-%s-%lu", folder_key, (unsigned long)p.th32ProcessID);
        HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, name);
        if (m) {
            CloseHandle(m);
            found = p.th32ProcessID;
        }
    }
    CloseHandle(snap);
    return found;
}

/* --- moving files --------------------------------------------------------------------------------------------- */

static int copy_tree(const char *from, const char *to) {
    char pat[MAX_PATH + 4];
    WIN32_FIND_DATAA f;
    int ok = 1;
    CreateDirectoryA(to, NULL);
    snprintf(pat, sizeof pat, "%s\\*", from);
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..") || is_ours(f.cFileName)) continue;
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof a, "%s\\%s", from, f.cFileName);
        snprintf(b, sizeof b, "%s\\%s", to, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(a, b);
        else ok &= CopyFileA(a, b, TRUE) != 0;
    } while (FindNextFileA(h, &f));
    FindClose(h);
    return ok;
}

/* Once, before the first move ever: the save folder as it was, into profiles-backup\<time> */
static void first_backup(void) {
    char root[MAX_PATH], dir[MAX_PATH];
    snprintf(root, sizeof root, "%s\\" BACKUP_DIR, canon);
    if (GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES || !has_items(canon)) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    CreateDirectoryA(root, NULL);
    snprintf(dir, sizeof dir, "%s\\%04d%02d%02d-%02d%02d%02d", root, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    say(copy_tree(canon, dir) ? "the save folder as it was is kept in %s" : "the backup into %s is not whole", dir);
}

/* File a out of the save folder (into perfil<a>), file b in (out of perfil<b>). Under the folder's lock. 1 when done;
   0 when it could not, with everything put back as it was. */
static int swap_files(int a, int b) {
    if (a == b) return 1;
    char pa[MAX_PATH], pb[MAX_PATH];
    park_dir(a, pa, sizeof pa);
    park_dir(b, pb, sizeof pb);
    first_backup();
    State s = {a, a, b, 1};
    if (!write_state(s)) {
        say("could not write %s\\" STATE_FILE ": no switch", canon);
        return 0;
    }
    if (move_items(canon, pa) < 0) {
        move_items(pa, canon);
        write_state((State){a, 0, 0, 0});
        return 0;
    }
    s.step = 2;
    write_state(s);
    if (move_items(pb, canon) < 0) {
        move_items(canon, pb);
        move_items(pa, canon);
        write_state((State){a, 0, 0, 0});
        return 0;
    }
    RemoveDirectoryA(pb);   /* (only when empty) */
    write_state((State){b, 0, 0, 0});
    say("file %d parked in %s; file %d is in the save folder now", a, pa, b);
    return 1;
}

/* A move the game stopped in the middle of (its window closed, a crash): finished. Under the folder's lock. */
static void recover(void) {
    State s = read_state();
    if (!s.from || s.from == s.to) return;
    char pa[MAX_PATH], pb[MAX_PATH];
    park_dir(s.from, pa, sizeof pa);
    park_dir(s.to, pb, sizeof pb);
    say("finishing the switch from file %d to file %d that stopped at step %d", s.from, s.to, s.step);
    if (s.step <= 1 && move_items(canon, pa) < 0) return;
    write_state((State){s.from, s.from, s.to, 2});
    if (move_items(pb, canon) < 0) return;
    RemoveDirectoryA(pb);
    write_state((State){s.to, 0, 0, 0});
}

/* --- what each file holds (the file select's plates) ---------------------------------------------------------- */

static uint32_t u32le(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* caption.sav, as Longshot's host/control.c and scripts/longshot_saves.py read it: magic 04 03 02 01, the scene's
   name at 684, the date as key/value pairs before 1392 (found by key). The date as yyyymmddhhmmss, 0 if unknown. */
static uint64_t caption_read(const char *path, char *scene, size_t cap, int date[6]) {
    static const uint32_t keys[6] = {0x9811a3f7, 0xdfd840d3, 0xbd46f485, 0x23f3d75e, 0x27853bf7, 0x5ee50bed};
    unsigned char h[1400];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < sizeof h || memcmp(h, "\x04\x03\x02\x01", 4)) return 0;
    size_t k = 0;
    for (; k + 1 < cap && k < 64 && h[684 + k] >= 0x20 && h[684 + k] < 0x7f; k++) scene[k] = (char)h[684 + k];
    scene[k] = 0;
    for (int i = 0; i < 6; i++) date[i] = -1;
    for (size_t off = 4; off + 8 <= 1392; off += 4)
        for (int i = 0; i < 6; i++)
            if (u32le(h + off) == keys[i] && date[i] < 0) date[i] = (int)u32le(h + off + 4);
    if (date[0] < 2000 || date[0] > 2200 || date[1] < 1 || date[1] > 12 || date[2] < 1 || date[2] > 31) return 1;
    for (int i = 3; i < 6; i++)
        if (date[i] < 0 || date[i] > 59) date[i] = 0;
    return (uint64_t)date[0] * 10000000000ULL + (uint64_t)date[1] * 100000000ULL + (uint64_t)date[2] * 1000000ULL +
           (uint64_t)date[3] * 10000ULL + (uint64_t)date[4] * 100ULL + (uint64_t)date[5];
}

/* "ForestTemple" -> "Forest Temple" */
static void spaced(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        const char c = in[i], p = i ? in[i - 1] : 0;
        if (i && c >= 'A' && c <= 'Z' && ((p >= 'a' && p <= 'z') || (p >= '0' && p <= '9'))) out[o++] = ' ';
        out[o++] = c == '_' ? ' ' : c;
    }
    out[o] = 0;
}

static void read_info(Info *in, const char *dir) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    memset(in, 0, sizeof *in);
    char pat[MAX_PATH + 8];
    WIN32_FIND_DATAA f;
    snprintf(pat, sizeof pat, "%s\\slot_*", dir);
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t best = 0;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char p[MAX_PATH], scene[72];
        int date[6];
        snprintf(p, sizeof p, "%s\\%s\\progress.sav", dir, f.cFileName);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
        in->used = 1;
        snprintf(p, sizeof p, "%s\\%s\\caption.sav", dir, f.cFileName);
        const uint64_t when = caption_read(p, scene, sizeof scene, date);
        if (when && when >= best) {
            best = when;
            spaced(scene[0] ? scene : "Saved game", in->place, sizeof in->place);
            if (when > 1) {
                const int h12 = date[3] % 12 ? date[3] % 12 : 12;
                snprintf(in->when, sizeof in->when, "%s %d, %d  %d:%02d %s", months[date[1] - 1], date[2], date[0], h12,
                         date[4], date[3] < 12 ? "AM" : "PM");
                snprintf(in->day, sizeof in->day, "%s %d", months[date[1] - 1], date[2]);
            } else in->when[0] = 0, in->day[0] = 0;
        }
    } while (FindNextFileA(h, &f));
    FindClose(h);
    if (in->used && !in->place[0]) snprintf(in->place, sizeof in->place, "Saved game");
}

static void load_info(int n, const char *dir) { read_info(&info[n], dir); }

static void load_all_info(void) {
    for (int n = 1; n <= FILES; n++) {
        char dir[MAX_PATH];
        if (n == active) snprintf(dir, sizeof dir, "%s", canon);
        else park_dir(n, dir, sizeof dir);
        load_info(n, dir);
    }
}

/* --- the save thread ------------------------------------------------------------------------------------------ */

static HANDLE find_thread(const wchar_t *want) {
    GetThreadDescriptionFn describe =
        (GetThreadDescriptionFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!describe) return NULL;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    THREADENTRY32 t;
    t.dwSize = sizeof t;
    HANDLE found = NULL;
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &t); ok && !found; ok = Thread32Next(snap, &t)) {
        if (t.th32OwnerProcessID != self) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                              t.th32ThreadID);
        if (!h) continue;
        PWSTR d = NULL;
        if (SUCCEEDED(describe(h, &d)) && d && !wcscmp(d, want)) found = h;
        if (d) LocalFree(d);
        if (!found) CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

/* Suspended inside a system call (in ntdll, just after its syscall instruction): waiting, not in the middle of the
   game's own code or holding one of its locks */
static int in_syscall(HANDLE h) {
    static uintptr_t lo, hi;
    if (!lo) {
        const unsigned char *base = (const unsigned char *)GetModuleHandleW(L"ntdll.dll");
        if (!base) return 0;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
        lo = (uintptr_t)base + 0x1000;
        hi = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
    }
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(h, &c)) return 0;
    if (c.Rip < lo + 2 || c.Rip >= hi) return 0;
    const unsigned char *p = (const unsigned char *)c.Rip;
    return p[-2] == 0x0f && p[-1] == 0x05;
}

static int suspend_idle(HANDLE h) {
    for (int i = 0; i < 400; i++) {
        if (SuspendThread(h) == (DWORD)-1) return 0;
        if (in_syscall(h)) return 1;
        ResumeThread(h);
        Sleep(1);
    }
    return 0;
}

/* Under pick_lock: the save thread goes on (it reads the file in the folder now) */
static void release_save_thread(const char *why) {
    if (save_thread) {
        ResumeThread(save_thread);
        CloseHandle(save_thread);
        save_thread = NULL;
    }
    mark_live();
    say("the game's save thread goes on: %s", why);
}

static DWORD WINAPI watchdog(void *unused) {
    (void)unused;
    LONG last = frames;
    DWORD since = GetTickCount();
    while (state == ST_PICKING) {
        Sleep(50);
        const LONG f = frames;
        if (f != last) {
            last = f;
            since = GetTickCount();
            continue;
        }
        if (GetTickCount() - since < STALL_MS) continue;
        EnterCriticalSection(&pick_lock);
        if (state == ST_PICKING) {
            state = ST_DONE;
            release_save_thread("the game stopped drawing while it waited for its saves, so no file select this time");
            snprintf(toast, sizeof toast, "File %d", active);
            toast_until = GetTickCount() + 4000;
        }
        LeaveCriticalSection(&pick_lock);
    }
    return 0;
}

/* --- choosing, copying, erasing, naming ------------------------------------------------------------------------- */

/* The file select's pages, as on the N64: pick a file; Copy (which file, to which file); Erase (which file, sure?);
   Name (which file, then the name keyboard); Export (which file: a copy into Documents\Squadala saves); Import (which
   exported save, to which file). Copy or Import onto a file with saves asks first, and keeps the old saves. Picking an
   empty file with no name asks for its name first. The files show 6 at a time, on 5 pages (L and R, or the arrows). */
enum {
    MODE_MAIN, MODE_COPY_SRC, MODE_COPY_DST, MODE_COPY_SURE, MODE_ERASE_SEL, MODE_ERASE_SURE, MODE_NAME_SEL, MODE_NAME,
    MODE_EXPORT_SEL, MODE_IMPORT_SRC, MODE_IMPORT_DST, MODE_IMPORT_SURE
};
#define ITEM_COPY FILES
#define ITEM_ERASE (FILES + 1)
#define ITEM_NAME (FILES + 2)
#define ITEM_EXPORT (FILES + 3)
#define ITEM_IMPORT (FILES + 4)
#define ITEM_PAGE_PREV 100   /* (the mouse only: the page arrows) */
#define ITEM_PAGE_NEXT 101
#define ITEM_KEY 200         /* (the mouse only: the name keyboard's keys, ITEM_KEY + row * GRID_COLS + column) */
static volatile LONG mode = MODE_MAIN;
static volatile LONG page;   /* the page of files shown (0 .. PAGES-1; on Import's list, its pages) */
static int import_from;  /* 0..: the exported save being imported (exports[]) */
static int import_to;    /* 1..FILES: where to */
static volatile LONG active_touched;   /* a copy, an erase or an import changed the file in the save folder: its pick
                                          has the game read the save list again */
static int copy_from;    /* 1..FILES: the file being copied */
static int copy_to;      /* 1..FILES: where to */
static int erase_which;  /* 1..FILES: the file to erase */
static int name_for;     /* 1..FILES: the file being named */
static int name_then_load;   /* the name was asked for because the file was picked: load it when done */
static char name_buf[NAME_MAX + 1];
static int grid_r, grid_c;   /* the name keyboard's cursor (row 5: Space, Delete, Cancel, Save) */
static const char *const grid_rows[5] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "abcdefghijklm", "nopqrstuvwxyz", "0123456789-.'"};
#define GRID_COLS 13
/* The keyboard's last row: Space, Delete, Cancel and Save, each over a few columns. ("End", the N64's word, read as
   nothing to a player looking for how to keep the name: Save says it, and Cancel lets the mouse back out.) */
#define ACTIONS 4
enum { ACTION_SPACE, ACTION_DELETE, ACTION_CANCEL, ACTION_SAVE };
static const int action_col0[ACTIONS] = {0, 3, 6, 9}, action_col1[ACTIONS] = {2, 5, 8, 12};
static CRITICAL_SECTION input_lock;

/* The mouse on the file select: where its items were drawn last (draw space), read on the frame's thread */
typedef struct { float x0, y0, x1, y1; int item; } Hit;
#define HITS_MAX 96
static CRITICAL_SECTION hit_lock;
static Hit hits[HITS_MAX];
static int hit_n;
static float hit_w, hit_h;
/* The file select's swipe: how far right of its place it is drawn now (the night sky stays: only the panel moves) */
static float slide_dx;
static volatile LONG closed_back;   /* the file select closed by B (it swipes back out to the right; a pick, to the left) */
static char typed[64];   /* keys typed on the PC keyboard, taken in frame ('\b' deletes) */
static int typed_n;

static int has_content(int n) { return info[n].used || names[n][0]; }

/* The cursor onto an item; a file's item shows its page */
static void set_cursor(int i) {
    cursor = i;
    if (i >= 0 && i < FILES) page = i / PER_PAGE;
}

static void file_dir(int n, char *out, size_t cap) {
    if (n == active) snprintf(out, cap, "%s", canon);
    else park_dir(n, out, cap);
}

static void show(DWORD ms, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    message_until = GetTickCount() + ms;
}

/* A file's saves before something is copied over them: into profiles-backup\overwritten-file<N>-<date>, never deleted.
   1 when there were none or they moved. */
static int keep_aside(int n, const char *dir, const char *what) {
    if (!has_items(dir)) return 1;
    char keep[MAX_PATH];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(keep, sizeof keep, "%s\\" BACKUP_DIR, canon);
    CreateDirectoryA(keep, NULL);
    snprintf(keep, sizeof keep, "%s\\" BACKUP_DIR "\\overwritten-file%d-%04d%02d%02d-%02d%02d%02d", canon, n, t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (move_items(dir, keep) < 0) return show(8000, "File %d could not be cleared (see the log).", n), 0;
    say("file %d's saves moved to %s before %s over it", n, keep, what);
    return 1;
}

/* Under pick_lock and the folder's lock. Onto a file with saves: those are moved into profiles-backup first. */
static int copy_file(int from, int to) {
    char a[MAX_PATH], b[MAX_PATH];
    file_dir(from, a, sizeof a);
    file_dir(to, b, sizeof b);
    if (to == active && other_live()) return show(6000, "File %d is open in the other game window.", to), 0;
    if (!keep_aside(to, b, "the copy")) return 0;
    if (!copy_tree(a, b)) {
        say("copying file %d (%s) to file %d (%s) did not finish", from, a, to, b);
        return show(8000, "Copying File %d did not finish (see the log).", from), 0;
    }
    snprintf(names[to], sizeof names[to], "%s", names[from]);
    write_state(read_state_keep_names());
    if (to == active) active_touched = 1;
    say("file %d copied to file %d", from, to);
    return 1;
}

/* Under pick_lock and the folder's lock. Erased files are moved into profiles-backup, never deleted. */
static int erase_file(int n) {
    char from[MAX_PATH], to[MAX_PATH];
    file_dir(n, from, sizeof from);
    if (n == active && other_live()) return show(6000, "File %d is open in the other game window.", n), 0;
    if (has_items(from)) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        snprintf(to, sizeof to, "%s\\" BACKUP_DIR, canon);
        CreateDirectoryA(to, NULL);
        snprintf(to, sizeof to, "%s\\" BACKUP_DIR "\\erased-file%d-%04d%02d%02d-%02d%02d%02d", canon, n, t.wYear, t.wMonth,
                 t.wDay, t.wHour, t.wMinute, t.wSecond);
        if (move_items(from, to) < 0) return show(8000, "File %d could not be erased (see the log).", n), 0;
        if (n != active) RemoveDirectoryA(from);
        say("file %d erased: its saves are kept in %s", n, to);
    }
    names[n][0] = 0;
    write_state(read_state_keep_names());
    if (n == active) active_touched = 1;
    return 1;
}

static void choose(int n) {
    EnterCriticalSection(&pick_lock);
    if (state != ST_PICKING) {
        LeaveCriticalSection(&pick_lock);
        return;
    }
    HANDLE m = folder_lock();
    if (!m) say("the save folder's lock did not come in 5 s: going on without it");
    recover();
    State s = read_state();
    active = s.active;   /* (another game window may have switched meanwhile) */
    const int previous = active;
    if (n != active) {
        const DWORD other = other_live();
        if (other) {
            folder_unlock(m);
            load_all_info();   /* (the plates as the other window left them) */
            mode = MODE_MAIN;
            set_cursor(active - 1);
            show(6000, "File %d is open in the other game window. Pick File %d, or close that window first.", active, active);
            say("file %d not taken: process %lu plays from this save folder", n, (unsigned long)other);
            LeaveCriticalSection(&pick_lock);
            return;
        }
        if (swap_files(active, n)) active = n;
        else show(8000, "Could not switch to File %d (see the log). File %d is loaded.", n, active);
    }
    folder_unlock(m);
    const int moved = n == active && n != previous;
    state = ST_DONE;
    if (title_pick) {   /* (over the title: the game reads its list again when the files moved) */
        title_pick = 0;
        const LONG touched = InterlockedExchange(&active_touched, 0);
        need_reload = moved || touched;
        say("file %d chosen over the title%s", active, moved ? ": the game reads its save list again" : "");
    } else {
        release_save_thread(n == active ? "the chosen file is in the save folder" : "the switch failed, the file there stays");
    }
    title_since = 0;
    want_list = v2 && info[active].used;   /* (an empty file: the title's New Game is the player's) */
    if (names[active][0]) snprintf(toast, sizeof toast, "File %d: %s", active, names[active]);
    else snprintf(toast, sizeof toast, "File %d", active);
    toast_until = GetTickCount() + 4000;
    LeaveCriticalSection(&pick_lock);
}

/* --- export and import: Documents\Squadala saves ----------------------------------------------------------------- */

/* An exported file Import offers: its folder, its name when it was exported, its newest save */
typedef struct {
    char dir[MAX_PATH];
    char name[NAME_MAX + 1];
    char folder[96];   /* (the folder's own name, shown when the save has no name) */
    Info info;
    FILETIME made;
} Export;
#define EXPORTS_SEEN 64
static Export exports[EXPORTS_SEEN];
static int import_n;   /* the newest FILES of them, newest first */

static void export_root(char *out, size_t cap) {
    char docs[MAX_PATH] = "";
    if (GetEnvironmentVariableA("SAVE_PROFILES_EXPORT_DIR", out, (DWORD)cap) && out[0]) return;   /* (a test run's) */
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs)) || !docs[0]) {
        if (GetEnvironmentVariableA("USERPROFILE", docs, sizeof docs)) strncat(docs, "\\Documents", sizeof docs - strlen(docs) - 1);
        else snprintf(docs, sizeof docs, "%s", canon);
    }
    snprintf(out, cap, "%s\\" EXPORT_DIR, docs);
}

/* A name fit for a folder: none of \ / : * ? " < > |, no dot or space at its end */
static void folder_safe(const char *in, char *out, size_t cap) {
    size_t k = 0;
    for (; *in && k + 1 < cap; in++) out[k++] = strchr("\\/:*?\"<>|", *in) || *in < 0x20 ? '-' : *in;
    while (k && (out[k - 1] == '.' || out[k - 1] == ' ')) k--;
    out[k] = 0;
    if (!k) snprintf(out, cap, "Saved game");
}

/* Under pick_lock and the folder's lock: file n copied into a new folder of its own in Documents\Squadala saves (its
   saves, its settings; a note with its name for Import). Nothing of the file changes. */
static int export_file(int n) {
    char from[MAX_PATH], root[MAX_PATH], base[MAX_PATH], to[MAX_PATH], label[64];
    file_dir(n, from, sizeof from);
    export_root(root, sizeof root);
    CreateDirectoryA(root, NULL);
    folder_safe(names[n][0] ? names[n] : info[n].place, label, sizeof label);
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(base, sizeof base, "%s\\File %d - %s - %04d-%02d-%02d %02d.%02d", root, n, label, t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute);
    snprintf(to, sizeof to, "%s", base);
    for (int k = 2; GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES && k < 100; k++) snprintf(to, sizeof to, "%s (%d)", base, k);
    if (!copy_tree(from, to)) {
        say("exporting file %d (%s) to %s did not finish", n, from, to);
        return show(8000, "Exporting File %d did not finish (see the log).", n), 0;
    }
    char note[MAX_PATH + 32];
    snprintf(note, sizeof note, "%s\\" EXPORT_NOTE, to);
    FILE *f = fopen(note, "w");
    if (f) {
        fprintf(f, "# Save Profiles: File %d, exported %04d-%02d-%02d %02d:%02d. Import it on the file select (Import).\n"
                   "name=%s\n", n, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, names[n]);
        fclose(f);
    }
    say("file %d exported to %s", n, to);
    return 1;
}

static int export_newer(const void *a, const void *b) {
    return CompareFileTime(&((const Export *)b)->made, &((const Export *)a)->made);
}

/* The exported files in Documents\Squadala saves that hold a save, newest first (the newest FILES) */
static void list_exports(void) {
    char root[MAX_PATH], pat[MAX_PATH + 4];
    export_root(root, sizeof root);
    snprintf(pat, sizeof pat, "%s\\*", root);
    int n = 0;
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == '.' || n >= EXPORTS_SEEN) continue;
            Export *e = &exports[n];
            memset(e, 0, sizeof *e);
            snprintf(e->dir, sizeof e->dir, "%s\\%s", root, f.cFileName);
            snprintf(e->folder, sizeof e->folder, "%s", f.cFileName);
            e->made = f.ftCreationTime;
            read_info(&e->info, e->dir);
            if (!e->info.used) continue;   /* (no save in it) */
            char note[MAX_PATH + 32], line[128];
            snprintf(note, sizeof note, "%s\\" EXPORT_NOTE, e->dir);
            FILE *nf = fopen(note, "r");
            if (nf) {
                while (fgets(line, sizeof line, nf))
                    if (!strncmp(line, "name=", 5)) clean_name(line + 5, e->name);
                fclose(nf);
            }
            n++;
        } while (FindNextFileA(h, &f));
        FindClose(h);
    }
    qsort(exports, (size_t)n, sizeof *exports, export_newer);
    import_n = n < FILES ? n : FILES;
}

/* Under pick_lock and the folder's lock: exported save i into file `to` (its saves before kept aside) */
static int import_file(int i, int to) {
    char b[MAX_PATH];
    file_dir(to, b, sizeof b);
    if (i < 0 || i >= import_n) return 0;
    if (to == active && other_live()) return show(6000, "File %d is open in the other game window.", to), 0;
    if (!keep_aside(to, b, "the import")) return 0;
    if (!copy_tree(exports[i].dir, b)) {
        say("importing %s into file %d (%s) did not finish", exports[i].dir, to, b);
        return show(8000, "Importing into File %d did not finish (see the log).", to), 0;
    }
    snprintf(names[to], sizeof names[to], "%s", exports[i].name);
    write_state(read_state_keep_names());
    if (to == active) active_touched = 1;
    say("%s imported into file %d", exports[i].dir, to);
    return 1;
}

/* Copy, erase, export or import now (A on the last page of each) */
enum { JOB_ERASE, JOB_COPY, JOB_EXPORT, JOB_IMPORT };
static void file_job(int job, int n) {
    EnterCriticalSection(&pick_lock);
    if (state == ST_PICKING) {
        HANDLE m = folder_lock();
        recover();
        active = read_state().active;
        if (job == JOB_COPY && copy_file(copy_from, copy_to)) show(5000, "File %d copied to File %d.", copy_from, copy_to);
        if (job == JOB_ERASE && erase_file(erase_which)) show(5000, "File %d erased.", erase_which);
        if (job == JOB_EXPORT && export_file(n)) show(7000, "File %d exported to Documents\\" EXPORT_DIR ".", n);
        if (job == JOB_IMPORT && import_file(import_from, import_to)) show(5000, "Imported into File %d.", import_to);
        folder_unlock(m);
        load_all_info();
    }
    LeaveCriticalSection(&pick_lock);
}

/* --- the name keyboard ---------------------------------------------------------------------------------------- */

static void name_start(int n, int then_load) {
    name_for = n;
    name_then_load = then_load;
    snprintf(name_buf, sizeof name_buf, "%s", names[n]);
    grid_r = 0, grid_c = 0;
    mode = MODE_NAME;
}

static void name_put(char c) {
    const size_t len = strlen(name_buf);
    if (c < 0x20 || c > 0x7e || len >= NAME_MAX) return;
    if (c == ' ' && (len == 0 || name_buf[len - 1] == ' ')) return;   /* (no leading or doubled spaces) */
    name_buf[len] = c, name_buf[len + 1] = 0;
}

static void name_delete(void) {
    const size_t len = strlen(name_buf);
    if (len) name_buf[len - 1] = 0;
}

static void name_cancel(void) {
    mode = name_then_load ? MODE_MAIN : MODE_NAME_SEL;
    set_cursor(name_for - 1);
}

static void name_done(void) {
    size_t len = strlen(name_buf);
    while (len && name_buf[len - 1] == ' ') name_buf[--len] = 0;
    EnterCriticalSection(&pick_lock);
    if (state == ST_PICKING) {
        HANDLE m = folder_lock();
        State s = read_state();   /* (the names on disk, then this one over them) */
        snprintf(names[name_for], sizeof names[name_for], "%s", name_buf);
        write_state(s);
        folder_unlock(m);
        say("file %d named \"%s\"", name_for, name_buf);
    }
    LeaveCriticalSection(&pick_lock);
    if (name_then_load) {
        mode = MODE_MAIN;
        set_cursor(name_for - 1);
        choose(name_for);
    } else {
        mode = MODE_MAIN;
        set_cursor(name_for - 1);
    }
}

static int action_of(int col) {
    for (int k = 0; k < ACTIONS - 1; k++)
        if (col <= action_col1[k]) return k;
    return ACTIONS - 1;
}

/* --- input ---------------------------------------------------------------------------------------------------- */

enum {
    ACT_UP = 1, ACT_DOWN = 2, ACT_OK = 4, ACT_BACK = 8, ACT_LEFT = 16, ACT_RIGHT = 32, ACT_DONE = 64, ACT_CANCEL = 128,
    ACT_FILE = 256 /* <<(k): the k-th file of the page shown, 0 .. PER_PAGE-1 */, ACT_PAGE_L = 1 << 14, ACT_PAGE_R = 1 << 15
};

/* Switch NpadButton bits, as hos_coop_pad_read gives them */
#define PAD_A (1ULL << 0)
#define PAD_B (1ULL << 1)
#define PAD_Y (1ULL << 3)
#define PAD_L (1ULL << 6)
#define PAD_R (1ULL << 7)
#define PAD_ZL (1ULL << 8)
#define PAD_ZR (1ULL << 9)
#define PAD_PLUS (1ULL << 10)
#define PAD_LEFT (1ULL << 12)
#define PAD_UP (1ULL << 13)
#define PAD_RIGHT (1ULL << 14)
#define PAD_DOWN (1ULL << 15)
#define PAD_LSTICK_LEFT (1ULL << 16)
#define PAD_LSTICK_UP (1ULL << 17)
#define PAD_LSTICK_RIGHT (1ULL << 18)
#define PAD_LSTICK_DOWN (1ULL << 19)

static uint64_t read_pads(void) {
    if (!pad_read) return 0;
    uint64_t all = 0, b;
    int32_t st[4];
    if (pad_choice > 0 && pad_read(pad_choice, &b, st)) return b;
    for (int n = 1; n <= 8; n++)   /* (every pad: none chosen, or the chosen one is not connected: never stuck) */
        if (pad_read(n, &b, st)) all |= b;
    return all;
}

static void type_char(char c) {
    EnterCriticalSection(&input_lock);
    if (typed_n < (int)sizeof typed) typed[typed_n++] = c;
    LeaveCriticalSection(&input_lock);
}

static int key(void *user, uint32_t vk, int down, int repeat) {
    (void)user;
    if (state != ST_PICKING || !down) return 0;
    LONG a = 0;
    if (mode == MODE_NAME) {   /* the PC keyboard types the name */
        const int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        if (vk == VK_UP) a = ACT_UP;
        else if (vk == VK_DOWN) a = ACT_DOWN;
        else if (vk == VK_LEFT) a = ACT_LEFT;
        else if (vk == VK_RIGHT) a = ACT_RIGHT;
        else if (vk >= 'A' && vk <= 'Z') type_char((char)(shift ^ caps ? vk : vk + 32));
        else if (vk >= '0' && vk <= '9' && !shift) type_char((char)vk);
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) type_char((char)('0' + vk - VK_NUMPAD0));
        else if (vk == VK_SPACE) type_char(' ');
        else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) type_char('-');
        else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) type_char('.');
        else if (vk == VK_OEM_7 && !shift) type_char('\'');
        else if (vk == VK_BACK) type_char('\b');
        else if (!repeat && vk == VK_RETURN) a = ACT_DONE;
        else if (!repeat && vk == VK_ESCAPE) a = ACT_CANCEL;
    } else {
        if (vk == VK_UP || vk == 'W') a = ACT_UP;
        else if (vk == VK_DOWN || vk == 'S') a = ACT_DOWN;
        else if (vk == VK_LEFT || vk == 'A') a = ACT_LEFT;
        else if (vk == VK_RIGHT || vk == 'D') a = ACT_RIGHT;
        else if (vk == VK_PRIOR) a = ACT_PAGE_L;
        else if (vk == VK_NEXT) a = ACT_PAGE_R;
        else if (!repeat && (vk == VK_RETURN || vk == VK_SPACE)) a = ACT_OK;
        else if (!repeat && (vk == VK_ESCAPE || vk == VK_BACK)) a = ACT_BACK;
        else if (!repeat && mode == MODE_MAIN && vk >= '1' && vk < '1' + PER_PAGE) a = ACT_FILE << (vk - '1');
        else if (!repeat && mode == MODE_MAIN && vk >= VK_NUMPAD1 && vk < VK_NUMPAD1 + PER_PAGE) a = ACT_FILE << (vk - VK_NUMPAD1);
    }
    say("file select: key %u%s", vk, repeat ? " (held)" : "");
    InterlockedOr(&key_actions, a);
    return 1;   /* (the game does not see keys while the file select is up) */
}

static int sure_mode(int m) { return m == MODE_COPY_SURE || m == MODE_ERASE_SURE || m == MODE_IMPORT_SURE; }
static int is_lower(int i) { return i >= ITEM_COPY && i <= ITEM_IMPORT; }
static int right_column(int i) { return i == ITEM_EXPORT || i == ITEM_IMPORT; }

/* The items a page may stop on, whatever page shows: files (or, on Import's list, the exported saves) and the buttons */
static int valid(int m, int i) {
    switch (m) {
    case MODE_MAIN: return (i >= 0 && i < FILES) || is_lower(i);
    case MODE_COPY_SRC: return i >= 0 && i < FILES && info[i + 1].used;
    case MODE_COPY_DST: return i >= 0 && i < FILES && i != copy_from - 1;
    case MODE_COPY_SURE:
    case MODE_ERASE_SURE:
    case MODE_IMPORT_SURE: return i == 0 || i == 1;   /* (Yes, Quit) */
    case MODE_ERASE_SEL: return i >= 0 && i < FILES && has_content(i + 1);
    case MODE_NAME_SEL: return i >= 0 && i < FILES;
    case MODE_EXPORT_SEL: return i >= 0 && i < FILES && info[i + 1].used;
    case MODE_IMPORT_SRC: return i >= 0 && i < import_n;
    case MODE_IMPORT_DST: return i >= 0 && i < FILES;
    }
    return 0;
}

/* ...and on the page shown */
static int item_ok(int m, int i) {
    if (!valid(m, i)) return 0;
    return sure_mode(m) || is_lower(i) || i / PER_PAGE == page;
}

static int pages_of(int m) { return m == MODE_IMPORT_SRC ? (import_n + PER_PAGE - 1) / PER_PAGE : PAGES; }

/* Up and down on the page shown: its files (those the page takes), then the buttons' column the cursor is in */
static int column_order(int m, int *out) {
    int n = 0;
    if (sure_mode(m)) {
        out[n++] = 0, out[n++] = 1;
        return n;
    }
    for (int k = 0; k < PER_PAGE; k++)
        if (item_ok(m, (int)page * PER_PAGE + k)) out[n++] = (int)page * PER_PAGE + k;
    if (m == MODE_MAIN) {
        if (right_column((int)cursor)) out[n++] = ITEM_EXPORT, out[n++] = ITEM_IMPORT;
        else out[n++] = ITEM_COPY, out[n++] = ITEM_ERASE, out[n++] = ITEM_NAME;
    }
    return n;
}

static void step(int dir) {
    int order[16];
    const int n = column_order((int)mode, order);
    if (!n) return;
    int at = -1;
    for (int k = 0; k < n; k++)
        if (order[k] == (int)cursor) at = k;
    cursor = at < 0 ? order[dir > 0 ? 0 : n - 1] : order[((at + dir) % n + n) % n];
}

/* The next or the last page; the cursor keeps its row (a file the page does not take: its first one, or none) */
static void turn_page(int dir) {
    const int m = (int)mode, pages = pages_of(m);
    if (sure_mode(m) || pages <= 1) return;
    const int cur = (int)cursor, on_list = cur >= 0 && !is_lower(cur);
    page = ((int)page + dir + pages) % pages;
    if (!on_list) return;
    const int want = (int)page * PER_PAGE + cur % PER_PAGE;
    if (item_ok(m, want)) {
        cursor = want;
        return;
    }
    int order[16];
    const int n = column_order(m, order);
    cursor = n && !is_lower(order[0]) ? order[0] : -1;
}

/* Left and right: between the buttons' two columns (Copy, Erase, Name | Export, Import); on the files, the pages */
static void side(int dir) {
    const int m = (int)mode, cur = (int)cursor;
    if (sure_mode(m)) return;
    if (m == MODE_MAIN && is_lower(cur)) {
        if (dir > 0 && !right_column(cur)) cursor = cur == ITEM_COPY ? ITEM_EXPORT : ITEM_IMPORT;
        else if (dir < 0 && right_column(cur)) cursor = cur == ITEM_EXPORT ? ITEM_COPY : ITEM_ERASE;
        return;
    }
    turn_page(dir);
}

static int first_valid(int m) {
    for (int i = 0; i < FILES; i++)
        if (valid(m, i)) return i;
    return -1;
}
static int any_used(void) { return first_valid(MODE_COPY_SRC) >= 0; }

/* A page of the file select, the cursor on `at` (a file's page shows), or else on the first item it takes */
static void go(int m, int at) {
    mode = m;
    if (!valid(m, at)) at = first_valid(m);
    if (sure_mode(m)) {
        cursor = at < 0 ? 1 : at;
        return;
    }
    if (at >= 0 && !is_lower(at)) page = at / PER_PAGE;
    cursor = at;
}

static void handle_name(LONG a) {
    if (a & ACT_UP) grid_r = (grid_r + 5) % 6;
    if (a & ACT_DOWN) grid_r = (grid_r + 1) % 6;
    if (a & (ACT_LEFT | ACT_RIGHT)) {
        const int dir = (a & ACT_LEFT) ? -1 : 1;
        if (grid_r == 5) grid_c = action_col0[(action_of(grid_c) + ACTIONS + dir) % ACTIONS];
        else grid_c = (grid_c + GRID_COLS + dir) % GRID_COLS;
    }
    if (a & ACT_OK) {
        if (grid_r < 5) name_put(grid_rows[grid_r][grid_c]);
        else if (action_of(grid_c) == ACTION_SPACE) name_put(' ');
        else if (action_of(grid_c) == ACTION_DELETE) name_delete();
        else if (action_of(grid_c) == ACTION_CANCEL) return name_cancel();
        else return name_done();
    }
    if (a & ACT_BACK) {   /* (B: delete a letter; with none left, back out) */
        if (name_buf[0]) name_delete();
        else return name_cancel();
    }
    if (a & ACT_CANCEL) return name_cancel();
    if (a & ACT_DONE) return name_done();
}

static void close_over_title(void);   /* (below, with the menu screens) */

static void pick_file(int n) {
    set_cursor(n - 1);
    if (!has_content(n)) name_start(n, 1);   /* (a new file: its name first, as the N64) */
    else choose(n);
}

static void handle(LONG a) {
    const int m = (int)mode;
    if (m == MODE_NAME) return handle_name(a);
    if (a & ACT_UP) step(-1);
    if (a & ACT_DOWN) step(1);
    if (a & ACT_LEFT) side(-1);
    if (a & ACT_RIGHT) side(1);
    if (a & ACT_PAGE_L) turn_page(-1);
    if (a & ACT_PAGE_R) turn_page(1);
    if (m == MODE_MAIN)
        for (int k = 0; k < PER_PAGE; k++)
            if (a & (ACT_FILE << k)) return pick_file((int)page * PER_PAGE + k + 1);
    const int cur = (int)cursor;
    if ((a & ACT_OK) && (cur < 0 || !item_ok(m, cur))) return;   /* (no item under the cursor on this page) */
    if (a & ACT_OK) {
        switch (m) {
        case MODE_MAIN:
            if (cur < FILES) pick_file(cur + 1);
            else if (cur == ITEM_COPY) {
                if (!any_used()) show(4000, "There is no file to copy.");
                else go(MODE_COPY_SRC, active - 1);
            } else if (cur == ITEM_ERASE) {
                if (first_valid(MODE_ERASE_SEL) >= 0) go(MODE_ERASE_SEL, active - 1);
                else show(4000, "There is no file to erase.");
            } else if (cur == ITEM_NAME) {
                go(MODE_NAME_SEL, active - 1);
            } else if (cur == ITEM_EXPORT) {
                if (!any_used()) show(4000, "There is no file to export.");
                else go(MODE_EXPORT_SEL, active - 1);
            } else if (cur == ITEM_IMPORT) {
                list_exports();
                if (!import_n) show(7000, "No saves to import: exported files go in Documents\\" EXPORT_DIR ".");
                else go(MODE_IMPORT_SRC, 0);
            }
            return;
        case MODE_COPY_SRC: copy_from = cur + 1; go(MODE_COPY_DST, cur); return;
        case MODE_COPY_DST:
            copy_to = cur + 1;
            if (has_content(copy_to)) go(MODE_COPY_SURE, 1);   /* (Quit first) */
            else file_job(JOB_COPY, 0), go(MODE_MAIN, cur);
            return;
        case MODE_COPY_SURE:
            if (cur == 0) file_job(JOB_COPY, 0);
            go(MODE_MAIN, copy_to - 1);
            return;
        case MODE_ERASE_SEL: erase_which = cur + 1; go(MODE_ERASE_SURE, 1); return;   /* (Quit first, as the N64) */
        case MODE_ERASE_SURE:
            if (cur == 0) file_job(JOB_ERASE, 0);
            go(MODE_MAIN, erase_which - 1);
            return;
        case MODE_NAME_SEL: name_start(cur + 1, 0); return;
        case MODE_EXPORT_SEL:
            file_job(JOB_EXPORT, cur + 1);
            go(MODE_MAIN, ITEM_EXPORT);
            return;
        case MODE_IMPORT_SRC: {
            import_from = cur;
            int to = -1;   /* (the first empty file, else the one in the save folder) */
            for (int i = 0; i < FILES && to < 0; i++)
                if (!has_content(i + 1)) to = i;
            go(MODE_IMPORT_DST, to < 0 ? active - 1 : to);
            return;
        }
        case MODE_IMPORT_DST:
            import_to = cur + 1;
            if (has_content(import_to)) go(MODE_IMPORT_SURE, 1);   /* (Quit first) */
            else file_job(JOB_IMPORT, 0), go(MODE_MAIN, cur);
            return;
        case MODE_IMPORT_SURE:
            if (cur == 0) file_job(JOB_IMPORT, 0);
            go(MODE_MAIN, import_to - 1);
            return;
        }
    }
    if (a & ACT_BACK) {
        switch (m) {
        case MODE_MAIN:   /* (over the title: back to it; at the start: B keeps the file played last) */
            if (title_pick) close_over_title();
            else choose(active);
            return;
        case MODE_COPY_SRC: go(MODE_MAIN, ITEM_COPY); return;
        case MODE_COPY_DST: go(MODE_COPY_SRC, copy_from - 1); return;
        case MODE_COPY_SURE: go(MODE_COPY_DST, copy_to - 1); return;
        case MODE_ERASE_SEL: go(MODE_MAIN, ITEM_ERASE); return;
        case MODE_ERASE_SURE: go(MODE_ERASE_SEL, erase_which - 1); return;
        case MODE_NAME_SEL: go(MODE_MAIN, ITEM_NAME); return;
        case MODE_EXPORT_SEL: go(MODE_MAIN, ITEM_EXPORT); return;
        case MODE_IMPORT_SRC: go(MODE_MAIN, ITEM_IMPORT); return;
        case MODE_IMPORT_DST: go(MODE_IMPORT_SRC, import_from); return;
        case MODE_IMPORT_SURE: go(MODE_IMPORT_DST, import_to - 1); return;
        }
    }
}

static void pointed(LONG *a, int moved, int l_down, int r_down, float fx, float fy);

/* The mouse over the game's window: the pointer moves the cursor onto an item, a left click chooses it, a right
   click is B (as on the game's own menus). On the name screen the keys and its Space, Delete, Cancel and Save are items too;
   the page arrows turn the page. prime: only note the buttons (one held when the file select came up does not
   count). */
static void mouse(LONG *a, int prime) {
    static POINT last = {-1, -1};
    static int was_l, was_r;
    const int l = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, r = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const int l_down = l && !was_l, r_down = r && !was_r;
    was_l = l, was_r = r;
    static DWORD test_next;
    if (test_click_i < test_clicks_n && !prime) {   /* (a test run's clicks) */
        const DWORD now = GetTickCount();
        if (!test_next) test_next = now + 3000;
        if ((LONG)(now - test_next) >= 0) {
            const int i = test_click_i++;
            test_next = now + 1500;
            const LONG before = *a;
            pointed(a, 1, 1, 0, test_click_x[i], test_click_y[i]);
            say("test click %d at %.3f, %.3f: actions %#lx, mode %ld, cursor %ld, page %ld, keyboard row %d column %d", i + 1,
                test_click_x[i], test_click_y[i], (unsigned long)(*a & ~before), (long)mode, (long)cursor, (long)page, grid_r, grid_c);
        }
    }
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    POINT p;
    RECT c;
    if (!w || pid != GetCurrentProcessId() || !GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &c) ||
        c.right <= 0 || c.bottom <= 0 || p.x < 0 || p.y < 0 || p.x >= c.right || p.y >= c.bottom)
        return;   /* (another window in front, or the pointer outside the game's) */
    const int moved = p.x != last.x || p.y != last.y;
    last = p;
    if (prime) return;
    pointed(a, moved, l_down, r_down, (float)p.x / (float)c.right, (float)p.y / (float)c.bottom);
}

/* The pointer at (fx, fy), fractions of the picture: onto an item (moved), a click on it (l_down), B (r_down) */
static void pointed(LONG *a, int moved, int l_down, int r_down, float fx, float fy) {
    int item = -1;
    EnterCriticalSection(&hit_lock);
    const float x = fx * hit_w, y = fy * hit_h;
    for (int i = 0; i < hit_n; i++)
        if (x >= hits[i].x0 && x < hits[i].x1 && y >= hits[i].y0 && y < hits[i].y1) item = hits[i].item;
    LeaveCriticalSection(&hit_lock);
    if (r_down) *a |= ACT_BACK;
    if (item < 0) return;
    if (item == ITEM_PAGE_PREV || item == ITEM_PAGE_NEXT) {
        if (l_down) *a |= item == ITEM_PAGE_PREV ? ACT_PAGE_L : ACT_PAGE_R;
        return;
    }
    if (mode == MODE_NAME) {
        if (item < ITEM_KEY) return;
        const int k = item - ITEM_KEY, kr = k / GRID_COLS, kc = k % GRID_COLS;
        if (moved || l_down) grid_r = kr, grid_c = kc;
        if (l_down) *a |= ACT_OK;
        return;
    }
    if (item >= ITEM_KEY || !item_ok((int)mode, item)) return;
    if (moved || l_down) cursor = item;
    if (l_down) *a |= ACT_OK;
}

static void frame(void *user) {
    (void)user;
    static uint64_t prev;
    static int primed;
    static DWORD held_since, last_repeat;
    InterlockedIncrement(&frames);
    if (state != ST_PICKING) return;
    const DWORD now = GetTickCount();
    const uint64_t b = read_pads();
    if (InterlockedExchange(&reprime, 0)) primed = 0;   /* (the B that closed the load list does not count here) */
    const int priming = !primed;
    if (!primed) prev = b, primed = 1;   /* (a button already held when the file select came up does not count) */
    const uint64_t pressed = b & ~prev;
    LONG a = InterlockedExchange(&key_actions, 0);
    mouse(&a, priming);
    static const uint64_t dirs[4] = {PAD_UP | PAD_LSTICK_UP, PAD_DOWN | PAD_LSTICK_DOWN, PAD_LEFT | PAD_LSTICK_LEFT,
                                     PAD_RIGHT | PAD_LSTICK_RIGHT};
    static const LONG dir_act[4] = {ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT};
    int held = -1;
    for (int k = 0; k < 4; k++) {
        if (pressed & dirs[k]) a |= dir_act[k], held_since = now, last_repeat = now;
        else if (b & dirs[k]) held = k;
    }
    if (held >= 0 && now - held_since > 400 && now - last_repeat > 110) a |= dir_act[held], last_repeat = now;
    if (pressed & PAD_A) a |= ACT_OK;
    if (pressed & PAD_B) a |= ACT_BACK;
    if (pressed & PAD_PLUS) a |= mode == MODE_NAME ? ACT_DONE : ACT_OK;
    if ((pressed & PAD_Y) && mode == MODE_NAME) type_char(' ');
    if (pressed & (PAD_L | PAD_ZL)) a |= ACT_PAGE_L;
    if (pressed & (PAD_R | PAD_ZR)) a |= ACT_PAGE_R;
    prev = b;
    if (test_pick && now - picking_since > test_pick_ms) {   /* (its page first) */
        page = (test_pick - 1) / PER_PAGE;
        a |= ACT_FILE << ((test_pick - 1) % PER_PAGE), test_pick = 0;
    }
    if (a & (ACT_OK | ACT_DONE)) InterlockedOr(&want_sounds, SND_DECIDE);   /* (a choice: the game's own decide) */

    char q[sizeof typed];
    int qn;
    EnterCriticalSection(&input_lock);
    qn = typed_n;
    memcpy(q, typed, (size_t)qn);
    typed_n = 0;
    LeaveCriticalSection(&input_lock);
    if (mode == MODE_NAME)
        for (int i = 0; i < qn; i++) q[i] == '\b' ? name_delete() : name_put(q[i]);
    const LONG cursor_was = cursor, mode_was = mode, page_was = page;
    if (a) handle(a);
    if ((cursor != cursor_was || page != page_was) && mode == mode_was)
        InterlockedOr(&want_sounds, SND_CURSOR);   /* (a step or a page: button_on) */
}

/* --- the game's menu screens (mod interface 2) ------------------------------------------------------------------- */

/* The file select comes up over the title (its Continue, or B on the save list) */
static void open_over_title(const char *why) {
    EnterCriticalSection(&pick_lock);
    if (state != ST_PICKING) {
        HANDLE m = folder_lock();
        recover();
        active = read_state().active;
        folder_unlock(m);
        load_all_info();
        mode = MODE_MAIN;
        set_cursor(active - 1);
        message[0] = 0;
        reprime = 1;
        closed_back = 0;
        title_pick = 1;
        want_list = 0;
        picking_since = GetTickCount();
        state = ST_PICKING;
        say("%s: the file select is up over the title (file %d in the save folder)", why, active);
    }
    LeaveCriticalSection(&pick_lock);
}

/* B in the file select over the title: the title is the player's again */
static void close_over_title(void) {
    EnterCriticalSection(&pick_lock);
    if (state == ST_PICKING && title_pick) {
        title_pick = 0;
        want_list = 0;
        closed_back = 1;
        state = ST_DONE;
        say("B: the file select closed, the title is yours (file %d in the save folder)", active);
    }
    LeaveCriticalSection(&pick_lock);
}

/* The load list just closed with no save loaded, and the title takes input again: B (a pad, a key or the mouse) took
   the player back. A save loaded from the list always goes through its question (its Yes), and the title does not
   come back then; the System page's load list has no title behind it. */
static int back_from_list(DWORD now) {
    return list_ms && now - list_ms > 150 && now - list_ms < 1500 && (!question_ms || now - question_ms > 1500);
}

/* Every menu screen's update, on the game's thread (the calls below work only in here) */
static void on_screen(void *user, const CoopModScreen *sc) {
    (void)user;
    const DWORD now = GetTickCount();
    /* (the focused button takes a choice now: interface 3's field, 1 from a runtime without it) */
    const int ready = api->version >= 3 && sc->size >= offsetof(CoopModScreen, focused_ready) + sizeof sc->focused_ready ? sc->focused_ready : 1;
    if (debug_screens && sc->kind < 4) {   /* (SAVE_PROFILES_DEBUG=1: each change of what a kind of screen says, once) */
        static char last[4][96];
        char now_says[96];
        snprintf(now_says, sizeof now_says, "focus '%s'%s, save list %s", sc->focused, ready ? " (ready)" : "",
                 sc->save_list_ready ? "read" : "not read");
        if (strcmp(now_says, last[sc->kind])) {
            memcpy(last[sc->kind], now_says, sizeof now_says);
            say("screen: kind %u, %s", sc->kind, now_says);
        }
    }
    if (sc->kind == COOP_MOD_SCREEN_LOAD_QUESTION) {
        question_ms = now;
        return;
    }
    if (sc->kind == COOP_MOD_SCREEN_LOAD_LIST) {
        list_ms = now;
        list_asked = 0;
        return;
    }
    if (sc->kind != COOP_MOD_SCREEN_TITLE) return;
    /* the sounds the file select asked for, on the title it is over (the game's own, as its screens start them) */
    if (api->version >= 3 && api->size >= offsetof(CoopModApi, play_sound) + sizeof api->play_sound && api->play_sound) {
        const LONG want = InterlockedExchange(&want_sounds, 0);
        if (want & SND_CONTINUE) api->play_sound(api, "Pa_Button_00_decide");
        if (want & (SND_CONTINUE | SND_DECIDE)) api->play_sound(api, "button_decide");
        else if (want & SND_CURSOR) api->play_sound(api, "button_on");
    }
    static uint64_t title_pad;   /* (the buttons at the title's last update: a press is a new one) */
    const uint64_t pressed = sc->buttons & ~title_pad;
    title_pad = sc->buttons;
    if (state == ST_PICKING) {
        if (title_pick) api->hold_input(api, COOP_MOD_HOLD_ALL);   /* (the file select over the title: the title gets nothing) */
        return;
    }
    /* The game's save list (or its question) over the title, or about to come: the title keeps Continue focused a
       moment under it, and the player's A there is the list's. */
    if (now - list_ms < 150 || now - question_ms < 150 || (list_asked && now - list_asked < 3000)) {
        api->hold_input(api, 0);
        return;
    }
    list_asked = 0;
    if (!sc->focused[0]) return;   /* (the title behind another screen, or not taking input yet) */
    if (back_from_list(now)) {
        list_ms = 0;
        open_over_title("B on the save list");
        api->hold_input(api, COOP_MOD_HOLD_ALL);
        return;
    }
    if (need_reload) {
        api->hold_input(api, COOP_MOD_HOLD_ALL);
        if (api->reload_save_list(api)) need_reload = 0;
        return;
    }
    if (want_list) {
        if (!title_since) title_since = now ? now : 1;
        api->hold_input(api, now - title_since < HOLD_AFTER_MS ? COOP_MOD_HOLD_ALL : 0);
        if (!sc->save_list_ready) return;   /* (the game still reads the files' captions) */
        if (strcmp(sc->focused, CONTINUE_BUTTON) != 0) {
            want_list = 0;
            api->hold_input(api, 0);
            say("the title's focus is on %s, not Continue: the title is yours", sc->focused);
            return;
        }
        if (api->choose_focused(api)) {
            want_list = 0;
            list_asked = now ? now : 1;
            api->hold_input(api, 0);
            say("file %d: the game's save list opens", active);
        } else if (now - title_since > LIST_TRY_MS) {
            want_list = 0;
            api->hold_input(api, 0);
            say("file %d: the save list did not open in %d s (co-op busy at the title?): the title is yours", active,
                LIST_TRY_MS / 1000);
        }
        return;
    }
    /* The title's Continue opens the file select: its A (and +) are the mod's while Continue is focused, the rest of
       the title stays the player's (New Game, Options, co-op's rows). */
    /* (not while the title's menu still comes in: its opening scene has Continue focused, and the first A brings the
       menu in, as the game has it) */
    if (!strcmp(sc->focused, CONTINUE_BUTTON) && ready) {
        api->hold_input(api, PAD_A | PAD_PLUS);
        if (pressed & (PAD_A | PAD_PLUS)) {
            InterlockedOr(&want_sounds, SND_CONTINUE);   /* (Continue's own decide: the game starts none, A was ours) */
            open_over_title("Continue");
            api->hold_input(api, COOP_MOD_HOLD_ALL);
        }
        return;
    }
    api->hold_input(api, 0);
}

/* --- drawing: the N64 file select's night sky, panel and buttons, from rectangles and text ------------------------ */

#define RGBA(r, g, b, a) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))

/* The N64's letters: PT Serif Bold on the buttons, Lato Bold Italic for the footer (both SIL OFL, in fonts\ beside this
   mod). The mod interface draws text in the overlay's one font; Longshot's own draw table (hos_coop_draw.h, the table
   co-op's DrawUser points at, which co-op hands Longshot's hos_coop_draw2 table through) can load a TTF and draw with
   it. That table is reached only when it checks out (the same picture size, and its calls inside loaded code), else
   every letter stays in the overlay's font: never a crash, only the plainer look. */
typedef struct LongshotDraw {
    float width, height;
    void *user;
    void (*rect)(void *user, float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);
    void (*circle)(void *user, float x, float y, float radius, uint32_t rgba, float thickness);
    void (*triangle)(void *user, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba);
    void (*text)(void *user, float x, float y, float px, uint32_t rgba, const char *utf8);
    float (*text_width)(void *user, float px, const char *utf8);
    int (*font)(void *user, const char *utf8_path);
    void (*use_font)(void *user, int id);
} LongshotDraw;
static const LongshotDraw *ld;   /* this frame's, or NULL */
static int serif_font, italic_font, fonts_tried;

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (const char *)p + n <= (const char *)mi.BaseAddress + mi.RegionSize;
}

static int is_code(const void *f) {
    MEMORY_BASIC_INFORMATION mi;
    if (!f || !VirtualQuery(f, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static const LongshotDraw *longshot_draw(const CoopModDraw *d) {
    if (!readable(d->user, sizeof(void *))) return NULL;
    const LongshotDraw *x = *(const LongshotDraw *const *)d->user;
    if (!readable(x, sizeof *x) || x->width != d->width || x->height != d->height) return NULL;
    if (!is_code((const void *)(uintptr_t)x->text) || !is_code((const void *)(uintptr_t)x->text_width) ||
        !is_code((const void *)(uintptr_t)x->font) || !is_code((const void *)(uintptr_t)x->use_font))
        return NULL;
    return x;
}

static void load_fonts(void) {
    static const char *const files[2] = {"PTSerif-Bold.ttf", "Lato-BoldItalic.ttf"};
    int *const ids[2] = {&serif_font, &italic_font};
    fonts_tried = 1;
    for (int i = 0; i < 2; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\fonts\\%s", api->mod_folder, files[i]);
        *ids[i] = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES ? ld->font(ld->user, p) : 0;
    }
    say("letters: %s, %s", serif_font ? "PT Serif Bold on the buttons" : "no serif font (the overlay's)",
        italic_font ? "Lato Bold Italic on the footer" : "no italic font (the overlay's)");
}

static void font_on(int id) {
    if (ld && id) ld->use_font(ld->user, id);
}
static void font_off(void) {
    if (ld) ld->use_font(ld->user, 0);
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static uint32_t mix(int r0, int g0, int b0, int r1, int g1, int b1, float t, int a) {
    t = clamp01(t);
    return RGBA((int)(r0 + (r1 - r0) * t), (int)(g0 + (g1 - g0) * t), (int)(b0 + (b1 - b0) * t), a);
}
static uint32_t lcg(uint32_t *x) { return *x = *x * 1664525u + 1013904223u; }
static float unit(uint32_t *x) { return (float)(lcg(x) >> 8) / 16777216.0f; }

/* Text with a dark edge all round (the N64's button letters) */
static void text_outline(const CoopModDraw *d, float x, float y, float px, uint32_t rgba, uint32_t edge, const char *s) {
    const float o = px * 0.05f > 1.0f ? px * 0.05f : 1.0f;
    d->text(d->user, x - o, y, px, edge, s);
    d->text(d->user, x + o, y, px, edge, s);
    d->text(d->user, x, y - o, px, edge, s);
    d->text(d->user, x, y + o, px, edge, s);
    d->text(d->user, x, y, px, rgba, s);
}

/* Text with a black drop shadow (the N64's titles and names) */
static void text_drop(const CoopModDraw *d, float x, float y, float px, uint32_t rgba, const char *s) {
    const float o = px * 0.06f > 1.5f ? px * 0.06f : 1.5f;
    d->text(d->user, x + o, y + o, px, RGBA(0, 0, 0, 210), s);
    d->text(d->user, x, y, px, rgba, s);
}

/* Button letters cut into the button, centred: the groove's shadow on their upper left, light catching its lower lip
   on their lower right, and a near-black fill tinted by the button's blue (lit: by its light cyan) */
static void text_engraved(const CoopModDraw *d, float cx, float y, float px, int lit, const char *s) {
    font_on(serif_font);
    if (serif_font) y -= px * 0.16f, px *= 1.32f;   /* (PT Serif's letters fill less of their box, and sit lower in it) */
    const float x = cx - d->text_width(d->user, px, s) * 0.5f, o = px * 0.045f > 1.0f ? px * 0.045f : 1.0f;
    d->text(d->user, x + o, y + o, px, lit ? RGBA(236, 246, 255, 235) : RGBA(150, 182, 255, 210), s);
    d->text(d->user, x - o * 0.6f, y - o * 0.6f, px, lit ? RGBA(18, 34, 100, 210) : RGBA(6, 12, 52, 220), s);
    d->text(d->user, x, y, px, lit ? RGBA(14, 22, 62, 255) : RGBA(14, 20, 52, 255), s);
    font_off();
}

/* Left-aligned text shrunk until it fits `room` */
static void text_fit(const CoopModDraw *d, float x, float y, float px, float room, uint32_t rgba, const char *s) {
    const float w = d->text_width(d->user, px, s);
    if (w > room && w > 0) px *= room / w;
    d->text(d->user, x, y, px, rgba, s);
}

/* The letters one by one, spread out as the N64 writes a file's name. The x after the last letter. */
static float text_spaced(const CoopModDraw *d, float x, float y, float px, float room, uint32_t rgba, const char *s) {
    char c[2] = {0, 0};
    float plain = 0;
    int n = 0;
    for (const char *p = s; *p; p++, n++) c[0] = *p, plain += d->text_width(d->user, px, c);
    float gap = n > 1 ? (room - plain) / (float)(n - 1) : 0;
    if (gap > px * 0.42f) gap = px * 0.42f;
    if (gap < 0) gap = 0, px *= room / (plain > 1 ? plain : 1);
    for (const char *p = s; *p; p++) {
        c[0] = *p;
        text_drop(d, x, y, px, rgba, c);
        x += d->text_width(d->user, px, c) + gap;
    }
    return x - gap;
}

/* The night sky, as if looking up: deep navy high up, a grey-blue haze low down, stars twinkling, and soft clouds
   drifting slowly to the right. A cloud is a loose cluster of round puffs, each three faint nested ellipses (no edge
   anywhere), thinning out at its ends: nearer clouds lower, bigger, brighter and a little faster. */
static void draw_sky(const CoopModDraw *d, float t) {
    const float W = d->width, H = d->height, k = H / 1080.0f;
    const int bands = 90;
    for (int i = 0; i < bands; i++) {
        const float y0 = H * i / bands, y1 = H * (i + 1) / bands, v = (float)i / (bands - 1);
        const uint32_t c = v < 0.55f ? mix(8, 13, 32, 20, 30, 62, v / 0.55f, 255) : mix(20, 30, 62, 48, 62, 104, (v - 0.55f) / 0.45f, 255);
        d->rect(d->user, 0, y0, W, y1, c, 0);
    }
    uint32_t seed = 0x5eed1234u;
    for (int i = 0; i < 260; i++) {   /* stars, thicker high up, twinkling */
        const float x = unit(&seed) * W, y = (float)pow(unit(&seed), 1.6) * H * 0.92f;
        const float big = unit(&seed), phase = unit(&seed) * 6.283f, speed = 0.6f + unit(&seed) * 2.2f;
        const float r = (big > 0.93f ? 2.6f : big > 0.7f ? 1.8f : 1.2f) * k;
        const float tw = 0.55f + 0.45f * (float)sin(t * speed + phase);
        const int a = (int)((big > 0.7f ? 230 : 150) * tw * (1.0f - 0.45f * y / H));
        if (big > 0.93f) d->rect(d->user, x - r * 2.2f, y - r * 2.2f, x + r * 2.2f, y + r * 2.2f, RGBA(150, 180, 255, a / 6), r * 2.2f);
        d->rect(d->user, x - r, y - r, x + r, y + r, RGBA(225, 235, 255, a), big > 0.93f ? r : 0);
    }
    uint32_t cs = 0xc10d5eedu;
    const float span = W * 1.8f;
    for (int c = 0; c < 16; c++) {
        const float depth = unit(&cs), bx = unit(&cs) * span;
        const float cy = H * (0.08f + 0.84f * (0.30f * unit(&cs) + 0.70f * depth));
        const float puff = (46 + 96 * depth) * k, reach = W * (0.07f + 0.10f * unit(&cs) + 0.05f * depth);
        const float speed = (4.0f + 10.0f * depth) * k, bright = 0.40f + 0.60f * depth;
        const float cx = (float)fmod(bx + t * speed, span) - W * 0.4f;
        const float tilt = (unit(&cs) - 0.5f) * 0.22f, bend = (unit(&cs) - 0.5f) * 0.5f;   /* (off level, a little curved) */
        for (int p = 0; p < 22; p++) {
            const float along = unit(&cs) + unit(&cs) - 1.0f;   /* (-1 .. 1, most near the middle) */
            const float across = (unit(&cs) + unit(&cs) - 1.0f) * 0.6f;
            const float thin = 1.0f - 0.65f * (along < 0 ? -along : along);
            const float r = puff * (0.40f + 0.75f * unit(&cs)) * thin;
            const float x = cx + along * reach;
            const float y = cy + along * reach * tilt + along * along * reach * bend * 0.25f + across * puff * thin - r * 0.1f;
            if (x + r * 1.7f < 0 || x - r * 1.7f > W) continue;
            for (int ring = 0; ring < 5; ring++) {   /* (faint outside, a little brighter and paler inside) */
                const float rx = r * (1.7f - 0.3f * ring), ry = rx * 0.42f;
                const int a = (int)(5.0f * bright + 0.5f);
                d->rect(d->user, x - rx, y - ry, x + rx, y + ry, mix(70, 82, 128, 176, 186, 218, ring / 4.0f, a), ry);
            }
        }
    }
}

/* The N64 panel: a blue window with its top left corner cut, lighter low down, see-through over the files (the night
   sky's stars faint behind them) and fading out to the right over its last third. Its inner window (fx, fy: under the title, behind the buttons and plates) is a deeper
   blue. Drawn in columns that meet edge to edge (never overlapping, or seams show); fine ones along the cut corner. */
#define PANEL_BANDS 12
/* (of 255: 250 hid the stars; 205, with the inner window's 140 over it, still let only about a tenth through where the
   files are. 150 and the inner window's 60 let about a third through there, the stars faint behind the files) */
#define PANEL_ALPHA 150
#define INNER_ALPHA 60
static void draw_panel(const CoopModDraw *d, float x0, float y0, float x1, float y1, float s, float fx, float fy, float fy1) {
    const float cut = 46 * s, rim = 4 * s, solid = 0.62f;
    for (float x = x0, next; x < x1; x = next) {
        const float off = x - x0;
        next = off < cut ? x + 2.0f * s : off < (x1 - x0) * solid ? x0 + (x1 - x0) * solid : x + 6.0f * s;
        if (next <= x) next = x + 6.0f * s;
        if (next > x1) next = x1;
        const float mid = (x + next) * 0.5f - x0, u = mid / (x1 - x0);
        float f = u < solid ? 1.0f : 1.0f - clamp01((u - solid) / (1.0f - solid));
        f = f * f * (3 - 2 * f);
        const int a = (int)(PANEL_ALPHA * f);
        if (a < 3) break;
        const float top = y0 + (mid < cut ? cut - mid : 0);
        d->rect(d->user, x, top, next, top + rim, mix(128, 166, 240, 112, 150, 232, u, a), 0);
        for (int b = 0; b < PANEL_BANDS; b++) {   /* (the body: lighter toward the bottom) */
            float b0 = y0 + (y1 - y0) * b / PANEL_BANDS, b1 = y0 + (y1 - y0) * (b + 1) / PANEL_BANDS;
            if (b1 <= top + rim) continue;
            if (b0 < top + rim) b0 = top + rim;
            if (b1 > y1 - rim) b1 = y1 - rim;
            if (b1 <= b0) continue;
            const float v = (b + 0.5f) / PANEL_BANDS;
            d->rect(d->user, x, b0, next, b1, mix(62, 92, 160, 86, 128, 220, v, a), 0);
        }
        d->rect(d->user, x, y1 - rim, next, y1, RGBA(24, 36, 110, a), 0);
        if (next > fx + 9 * s) {   /* (the inner window, deeper) */
            const float ix = x < fx + 9 * s ? fx + 9 * s : x;
            d->rect(d->user, ix, fy + 9 * s, next, fy1, RGBA(30, 48, 104, (int)(INNER_ALPHA * f)), 0);
        }
    }
    d->rect(d->user, x0, y0 + cut, x0 + rim, y1 - rim, RGBA(150, 180, 250, PANEL_ALPHA + 20), 0);
}

/* lit: the cursor is on it (the light blue fill; only ever one). halo: an outline in that colour (0: none), for the
   cursor and for files a page marks (the copy's source, a file about to be erased or overwritten). dot: 0 none; 1 the
   empty file's dot; 2 the dot's room only, so every file button's label sits in the same place. */
static void draw_button(const CoopModDraw *d, float x0, float y0, float x1, float y1, float s, const char *label, int lit,
                        uint32_t halo, float pulse, int dot) {
    const float h = y1 - y0, r = 13 * s;
    d->rect(d->user, x0 + 3 * s, y0 + 6 * s, x1 + 3 * s, y1 + 6 * s, RGBA(0, 0, 30, 110), r);
    if (lit)   /* the N64's glow: soft white-blue light spreading out from the button, breathing */
        for (int k = 4; k >= 1; k--) {
            const float g = 4.5f * k * s;
            d->rect(d->user, x0 - g, y0 - g, x1 + g, y1 + g, RGBA(196, 226, 255, (int)((34 - 6 * k) * (0.65f + 0.35f * pulse))), r + g);
        }
    if (halo && !lit) d->rect(d->user, x0 - 4 * s, y0 - 4 * s, x1 + 4 * s, y1 + 4 * s, (halo & 0x00ffffffu) | 200u << 24, r + 4 * s);
    if (halo && lit && halo != RGBA(120, 248, 255, 255))   /* (the cursor on a file about to be overwritten or erased) */
        d->rect(d->user, x0 - 4 * s, y0 - 4 * s, x1 + 4 * s, y1 + 4 * s, (halo & 0x00ffffffu) | 220u << 24, r + 4 * s);
    d->rect(d->user, x0, y0, x1, y1, lit ? RGBA(40, 62, 150, 255) : RGBA(14, 24, 78, 255), r);
    d->rect(d->user, x0 + 3 * s, y0 + 3 * s, x1 - 3 * s, y1 - 3 * s, lit ? RGBA(116, 158, 236, 255) : RGBA(72, 108, 198, 255), r - 2 * s);
    if (lit) {   /* (lit from inside) */
        d->rect(d->user, x0 + 5 * s, y0 + 5 * s, x1 - 5 * s, y1 - 5 * s, RGBA(230, 242, 255, (int)(34 + 26 * pulse)), r - 3 * s);
        d->rect(d->user, x0 + 10 * s, y0 + 8 * s, x1 - 10 * s, y1 - 8 * s, RGBA(240, 248, 255, (int)(20 + 18 * pulse)), r - 5 * s);
    }
    d->rect(d->user, x0 + 6 * s, y0 + 5 * s, x1 - 6 * s, y0 + h * 0.46f, RGBA(255, 255, 255, lit ? 48 : 30), r - 4 * s);
    if (dot == 1) {
        const float cx = x1 - 26 * s, cy = (y0 + y1) * 0.5f;
        d->rect(d->user, cx - 6 * s, cy - 6 * s, cx + 6 * s, cy + 6 * s, RGBA(10, 18, 60, 220), 6 * s);
        d->rect(d->user, cx - 4 * s, cy - 4 * s, cx + 4 * s, cy + 4 * s, RGBA(235, 242, 255, 255), 4 * s);
    }
    const float px = h * 0.72f;
    const float cx = dot ? (x0 + x1 - 26 * s) * 0.5f : (x0 + x1) * 0.5f;
    text_engraved(d, cx, y0 + (h - px) * 0.42f, px, lit, label);
}

/* The link between a button and its plate: a short bar with two rivets */
static void draw_link(const CoopModDraw *d, float x0, float x1, float cy, float s) {
    d->rect(d->user, x0, cy - 17 * s, x1, cy + 17 * s, RGBA(14, 24, 78, 255), 10 * s);
    d->rect(d->user, x0 + 3 * s, cy - 14 * s, x1 - 3 * s, cy + 14 * s, RGBA(84, 118, 210, 255), 8 * s);
    d->rect(d->user, x0 + 5 * s, cy - 12 * s, x1 - 5 * s, cy - 2 * s, RGBA(255, 255, 255, 40), 6 * s);
    for (int k = 0; k < 2; k++) {
        const float rx = x0 + (x1 - x0) * (k ? 0.68f : 0.32f);
        d->rect(d->user, rx - 6 * s, cy - 6 * s, rx + 6 * s, cy + 6 * s, RGBA(12, 20, 64, 255), 6 * s);
        d->rect(d->user, rx - 3 * s, cy - 4 * s, rx + 2 * s, cy + 0 * s, RGBA(150, 170, 240, 180), 3 * s);
    }
}

/* lit: the name plate of the chosen file; light: the small side plate (dark letters on a pale blue, as the N64's) */
static void draw_plate(const CoopModDraw *d, float x0, float y0, float x1, float y1, float s, int lit, int light) {
    const float r = 10 * s;
    d->rect(d->user, x0 + 2 * s, y0 + 5 * s, x1 + 2 * s, y1 + 5 * s, RGBA(0, 0, 30, 90), r);
    d->rect(d->user, x0, y0, x1, y1, RGBA(14, 24, 78, 255), r);   /* (solid: its shadow must not show through) */
    d->rect(d->user, x0 + 3 * s, y0 + 3 * s, x1 - 3 * s, y1 - 3 * s,
            light ? RGBA(132, 164, 232, 255) : lit ? RGBA(86, 124, 214, 255) : RGBA(62, 94, 190, 255), r - 2 * s);
    d->rect(d->user, x0 + 5 * s, y0 + 5 * s, x1 - 5 * s, y0 + (y1 - y0) * 0.45f, RGBA(255, 255, 255, light ? 40 : 28), r - 3 * s);
}

/* A small arrow (a triangle of thin strips) pointing left (dir -1) or right (1), its tip at x */
static void draw_arrow(const CoopModDraw *d, float x, float cy, float s, int dir, uint32_t rgba) {
    const int n = 8;
    const float len = 13 * s, half = 10 * s, w = len / n;
    for (int j = 0; j < n; j++) {   /* (strip j: from the tip back; taller toward the back) */
        const float h = half * (j + 1) / n, a = x - dir * w * j, b = x - dir * w * (j + 1);
        d->rect(d->user, a < b ? a : b, cy - h, a < b ? b : a, cy + h, rgba, 0);
    }
}

static void draw_footer(const CoopModDraw *d, float W, float y, float s, const char *l, const char *r, const char *hint) {
    const float fpx = (italic_font && ld ? 60 : 54) * s;
    font_on(italic_font);   /* (the N64's footer is an italic) */
    const float lw = d->text_width(d->user, fpx, l), rw = d->text_width(d->user, fpx, r), gap = 70 * s;
    const float fx0 = W * 0.5f - (lw + gap + rw) * 0.5f;
    const uint32_t cyan = RGBA(64, 226, 232, 255), edge = RGBA(0, 52, 72, 190);
    text_outline(d, fx0, y, fpx, cyan, edge, l);
    text_outline(d, fx0 + lw + gap, y, fpx, cyan, edge, r);
    font_off();
    const float dcx = fx0 + lw + gap * 0.5f, dcy = y + fpx * 0.55f;
    d->rect(d->user, dcx - 10 * s, dcy - 10 * s, dcx + 10 * s, dcy + 10 * s, edge, 10 * s);
    d->rect(d->user, dcx - 8 * s, dcy - 8 * s, dcx + 8 * s, dcy + 8 * s, cyan, 8 * s);
    const float hw = d->text_width(d->user, 21 * s, hint);
    text_drop(d, W * 0.5f - hw * 0.5f, y + 74 * s, 21 * s, RGBA(176, 190, 228, 210), hint);
}

static void draw_naming(const CoopModDraw *d, float x0, float fy, float s, float pulse, float t, Hit *hit, int *hit_count) {
    char label[16];
    snprintf(label, sizeof label, "File %d", name_for);
    /* the file's button and its name plate, the name being typed in it */
    const float bx0 = x0 + 92 * s, bx1 = bx0 + 290 * s, r0 = fy + 34 * s, r1 = r0 + 56 * s, cy = (r0 + r1) * 0.5f;
    draw_button(d, bx0, r0, bx1, r1, s, label, 0, 0, 0, 2);
    const float nx0 = bx1 + 46 * s, nx1 = x0 + 900 * s;
    draw_link(d, bx1 - 10 * s, nx0 + 8 * s, cy, s);
    draw_plate(d, nx0, r0 + 3 * s, nx1, r1 - 3 * s, s, 1, 0);
    const float room = (nx1 - nx0 - 48 * s) * (float)strlen(name_buf) / NAME_MAX;
    float end = name_buf[0] ? text_spaced(d, nx0 + 24 * s, r0 + 9 * s, 34 * s, room > 40 * s ? room : 40 * s,
                                          RGBA(242, 246, 255, 255), name_buf)
                            : nx0 + 24 * s;
    if (fmod(t, 1.0) < 0.6 && strlen(name_buf) < NAME_MAX)   /* (the cursor) */
        d->rect(d->user, end + 6 * s, r1 - 16 * s, end + 30 * s, r1 - 11 * s, RGBA(120, 248, 255, 255), 0);

    /* the keyboard: each key is the mouse's too */
    const float cell = 58 * s, gx0 = x0 + 120 * s, gy0 = r1 + 46 * s;
    d->rect(d->user, gx0 - 22 * s, gy0 - 18 * s, gx0 + GRID_COLS * cell + 22 * s, gy0 + 5 * cell + 18 * s, RGBA(10, 18, 64, 190), 12 * s);
    d->rect(d->user, gx0 - 19 * s, gy0 - 15 * s, gx0 + GRID_COLS * cell + 19 * s, gy0 + 5 * cell + 15 * s, RGBA(76, 110, 204, 255), 10 * s);
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < GRID_COLS; c++) {
            const float cx0 = gx0 + c * cell, cy0 = gy0 + r * cell;
            const char ch[2] = {grid_rows[r][c], 0};
            if (r == grid_r && c == grid_c) {   /* (the same glow as a button's) */
                for (int k = 3; k >= 1; k--) {
                    const float g = 4.0f * k * s;
                    d->rect(d->user, cx0 - g, cy0 - g, cx0 + cell + g, cy0 + cell + g,
                            RGBA(196, 226, 255, (int)((34 - 7 * k) * (0.65f + 0.35f * pulse))), 10 * s + g);
                }
                d->rect(d->user, cx0 + 3 * s, cy0 + 3 * s, cx0 + cell - 3 * s, cy0 + cell - 3 * s, RGBA(116, 158, 236, 255), 9 * s);
                d->rect(d->user, cx0 + 6 * s, cy0 + 6 * s, cx0 + cell - 6 * s, cy0 + cell - 6 * s,
                        RGBA(230, 242, 255, (int)(40 + 26 * pulse)), 7 * s);
            }
            const float px = (serif_font && ld ? 33 : 40) * s;
            text_engraved(d, cx0 + cell * 0.5f, cy0 + (cell - px) * 0.45f, px, r == grid_r && c == grid_c, ch);
            if (*hit_count < HITS_MAX) hit[(*hit_count)++] = (Hit){cx0, cy0, cx0 + cell, cy0 + cell, ITEM_KEY + r * GRID_COLS + c};
        }
    static const char *const actions[ACTIONS] = {"Space", "Delete", "Cancel", "Save"};
    const float ay0 = gy0 + 5 * cell + 36 * s, ay1 = ay0 + 56 * s;
    for (int k = 0; k < ACTIONS; k++) {
        const float ax0 = gx0 + action_col0[k] * cell + 4 * s, ax1 = gx0 + (action_col1[k] + 1) * cell - 4 * s;
        const int on = grid_r == 5 && action_of(grid_c) == k;
        draw_button(d, ax0, ay0, ax1, ay1, s, actions[k], on, on ? RGBA(120, 248, 255, 255) : 0, pulse, 0);
        if (*hit_count < HITS_MAX) hit[(*hit_count)++] = (Hit){ax0, ay0, ax1, ay1, ITEM_KEY + 5 * GRID_COLS + action_col0[k]};
    }
}

static void draw_picker(const CoopModDraw *d) {
    const float W = d->width, H = d->height;
    const float s = (W / 1920.0f < H / 1080.0f ? W / 1920.0f : H / 1080.0f);
    const float t = (float)(GetTickCount64() % 3600000ULL) / 1000.0f;
    const float pulse = 0.5f + 0.5f * (float)sin(t * 4.2);
    const int m = (int)mode, cur = (int)cursor, pg = (int)page;
    const float keep_dx = slide_dx;
    slide_dx = 0;
    draw_sky(d, t);
    slide_dx = keep_dx;

    const float x0 = W * 0.5f - 532 * s, y0 = H * 0.5f - 500 * s, x1 = x0 + 1090 * s, y1 = y0 + 880 * s;
    /* the inner window's corner, as the N64 draws it */
    const float fx = x0 + 56 * s, fy = y0 + 136 * s;
    draw_panel(d, x0, y0, x1, y1, s, fx, fy, y1 - 34 * s);
    char overwrite_title[48];
    snprintf(overwrite_title, sizeof overwrite_title, "Overwrite File %d?", m == MODE_IMPORT_SURE ? import_to : copy_to);
    const int sure = sure_mode(m);
    const char *title = m == MODE_COPY_SRC      ? "Which file do you want to copy?"
                        : m == MODE_COPY_DST    ? "Copy to which file?"
                        : m == MODE_COPY_SURE   ? overwrite_title
                        : m == MODE_ERASE_SEL   ? "Which file do you want to erase?"
                        : m == MODE_ERASE_SURE  ? "Are you sure?"
                        : m == MODE_NAME_SEL    ? "Which file do you want to name?"
                        : m == MODE_NAME        ? "Please enter a name."
                        : m == MODE_EXPORT_SEL  ? "Which file do you want to export?"
                        : m == MODE_IMPORT_SRC  ? "Which save do you want to import?"
                        : m == MODE_IMPORT_DST  ? "Import it to which file?"
                        : m == MODE_IMPORT_SURE ? overwrite_title
                                                : "Please select a file.";
    text_drop(d, x0 + 126 * s, y0 + 44 * s, 50 * s, RGBA(255, 255, 255, 255), title);
    if (message[0] && GetTickCount() < message_until)
        text_drop(d, x0 + 130 * s, y0 + 102 * s, 26 * s, RGBA(255, 232, 130, 255), message);

    /* (its top line solid over the buttons, then fading out to the right in strips that meet edge to edge) */
    const float fade0 = x0 + 470 * s, fade1 = x0 + 1010 * s;
    d->rect(d->user, fx, fy, fade0, fy + 9 * s, RGBA(0, 0, 8, 225), 0);
    d->rect(d->user, fx + 9 * s, fy + 9 * s, fade0, fy + 14 * s, RGBA(0, 0, 8, 70), 0);
    for (float x = fade0, next; x < fade1; x = next) {
        next = x + 4 * s;
        if (next > fade1) next = fade1;
        const float u = ((x + next) * 0.5f - fade0) / (fade1 - fade0), f = (1 - u) * (1 - u);
        d->rect(d->user, x, fy, next, fy + 9 * s, RGBA(0, 0, 8, (int)(225 * f)), 0);
        d->rect(d->user, x, fy + 9 * s, next, fy + 14 * s, RGBA(0, 0, 8, (int)(70 * f)), 0);
    }
    d->rect(d->user, fx, fy, fx + 9 * s, y1 - 34 * s, RGBA(0, 0, 8, 225), 0);
    d->rect(d->user, fx + 9 * s, fy + 9 * s, fx + 14 * s, y1 - 34 * s, RGBA(0, 0, 8, 70), 0);

    Hit drawn[HITS_MAX];
    int drawn_n = 0;
    if (m == MODE_NAME) {
        draw_naming(d, x0, fy, s, pulse, t, drawn, &drawn_n);
        EnterCriticalSection(&hit_lock);
        memcpy(hits, drawn, sizeof(Hit) * (size_t)drawn_n);
        hit_n = drawn_n, hit_w = W, hit_h = H;
        LeaveCriticalSection(&hit_lock);
        draw_footer(d, W, y1 + 26 * s, s, "A - Enter", "B - Delete",
                    "+ or Enter: done    Y: space    Type on the keyboard too: letters, digits, Backspace, Esc to cancel");
        return;
    }

    /* the page's six files (on Import's list, six exported saves), a little gap after the third */
    const float bx0 = x0 + 92 * s, bx1 = bx0 + 290 * s, bh = 56 * s, rowh = 64 * s, by = fy + 30 * s, split = 18 * s;
    const int list = m == MODE_IMPORT_SRC;
    for (int k = 0; k < PER_PAGE; k++) {
        const int i = pg * PER_PAGE + k, n = i + 1;
        const float r0 = by + k * rowh + (k >= PER_PAGE / 2 ? split : 0), r1 = r0 + bh, cy = (r0 + r1) * 0.5f;
        char label[16];
        const Info *in;
        const char *nm;
        int content;
        if (list) {
            if (i >= import_n) continue;
            in = &exports[i].info, nm = exports[i].name, content = 1;
            snprintf(label, sizeof label, "Save %d", n);
        } else {
            in = &info[n], nm = names[n], content = has_content(n);
            snprintf(label, sizeof label, "File %d", n);
        }
        const int pickable = !sure && item_ok(m, i);
        const int at = !sure && i == cur;   /* (the cursor: the only light blue button) */
        const uint32_t cyan = RGBA(120, 248, 255, 255), gold = RGBA(255, 226, 110, 255), red = RGBA(255, 120, 110, 255);
        uint32_t halo = 0;
        if (at) halo = m == MODE_ERASE_SEL || ((m == MODE_COPY_DST || m == MODE_IMPORT_DST) && content) ? red : cyan;
        else if ((m == MODE_COPY_DST || m == MODE_COPY_SURE) && n == copy_from) halo = gold;
        else if ((m == MODE_ERASE_SURE && n == erase_which) || (m == MODE_COPY_SURE && n == copy_to) ||
                 (m == MODE_IMPORT_SURE && n == import_to))
            halo = red;
        draw_button(d, bx0, r0, bx1, r1, s, label, at, halo, at ? pulse : 0.6f, content ? 2 : 1);
        if (!sure && drawn_n < HITS_MAX) drawn[drawn_n++] = (Hit){bx0, r0, x0 + 1010 * s, r1, i};   /* (its button and plates) */
        if (m != MODE_MAIN && !pickable && !halo) d->rect(d->user, bx0, r0, bx1, r1, RGBA(10, 16, 50, 110), 13 * s);
        if (!content) continue;
        /* the name plate (the file's name, or where it was saved), then a small plate: where and when */
        const float lx0 = bx1 - 10 * s, lx1 = bx1 + 54 * s, nx0 = lx1 - 8 * s, nx1 = x0 + 760 * s;
        draw_link(d, lx0, lx1, cy, s);
        draw_plate(d, nx0, r0 + 3 * s, nx1, r1 - 3 * s, s, at, 0);
        const char *big = nm[0] ? nm : in->place;
        text_spaced(d, nx0 + 24 * s, r0 + 9 * s, 34 * s, nx1 - nx0 - 48 * s, RGBA(242, 246, 255, 255), big);
        const float dx0 = nx1 + 40 * s, dx1 = dx0 + 250 * s;
        draw_link(d, nx1 - 10 * s, dx0 + 10 * s, cy, s);
        draw_plate(d, dx0, r0 + 4 * s, dx1, r1 - 4 * s, s, 0, 1);
        const uint32_t ink = RGBA(14, 18, 40, 255);
        if (!in->used) {
            d->text(d->user, dx0 + 20 * s, r0 + 14 * s, 26 * s, ink, "New");
        } else if (nm[0]) {   /* (named: the place and the date both here) */
            text_fit(d, dx0 + 16 * s, r0 + 9 * s, 20 * s, dx1 - dx0 - 30 * s, ink, in->place);
            text_fit(d, dx0 + 16 * s, r0 + 30 * s, 17 * s, dx1 - dx0 - 30 * s, RGBA(30, 40, 84, 255), in->when);
        } else if (in->when[0]) {
            text_fit(d, dx0 + 16 * s, r0 + 15 * s, 24 * s, dx1 - dx0 - 30 * s, ink, in->when);
        }
    }
    const float gy = by + PER_PAGE * rowh + split + 14 * s;

    /* the page: "2 / 5" between two arrows, on a small plate at the right under the list (a click on either half turns) */
    const int pages = pages_of(m);
    if (!sure && pages > 1) {
        const float px0 = x0 + 800 * s, px1 = x0 + 1050 * s, pr0 = gy + 8 * s, pr1 = gy + bh - 8 * s, pcy = (pr0 + pr1) * 0.5f;
        draw_plate(d, px0, pr0, px1, pr1, s, 0, 1);
        const uint32_t ink = RGBA(14, 18, 40, 255);
        draw_arrow(d, px0 + 18 * s, pcy, s, -1, ink);
        draw_arrow(d, px1 - 18 * s, pcy, s, 1, ink);
        char pg_text[24];
        snprintf(pg_text, sizeof pg_text, "%d / %d", pg + 1, pages);
        const float tpx = 26 * s, tw = d->text_width(d->user, tpx, pg_text);
        d->text(d->user, (px0 + px1) * 0.5f - tw * 0.5f, pcy - tpx * 0.58f, tpx, ink, pg_text);
        if (drawn_n + 2 <= HITS_MAX) {
            drawn[drawn_n++] = (Hit){px0 - 6 * s, pr0 - 6 * s, (px0 + px1) * 0.5f, pr1 + 6 * s, ITEM_PAGE_PREV};
            drawn[drawn_n++] = (Hit){(px0 + px1) * 0.5f, pr0 - 6 * s, px1 + 6 * s, pr1 + 6 * s, ITEM_PAGE_NEXT};
        }
    }

    /* the buttons: Copy, Erase, Name, and beside them Export, Import (on a question: Yes, Quit) */
    const char *lower[5] = {"Copy", "Erase", "Name", "Export", "Import"};
    const int count = sure ? 2 : 5;
    if (sure) lower[0] = "Yes", lower[1] = "Quit";
    for (int k = 0; k < count; k++) {
        const int col = k >= 3, row = col ? k - 3 : k;
        const float c0 = col ? bx1 + 34 * s : bx0, c1 = c0 + 290 * s;
        const float r0 = gy + row * rowh, r1 = r0 + bh;
        const int item = sure ? k : ITEM_COPY + k;
        const int at = sure ? cur == k : m == MODE_MAIN && cur == item;
        /* (the page's own button keeps an outline while its files are picked: which job this is) */
        const int job = !sure && (((m == MODE_COPY_SRC || m == MODE_COPY_DST) && k == 0) || (m == MODE_ERASE_SEL && k == 1) ||
                                  (m == MODE_NAME_SEL && k == 2) || (m == MODE_EXPORT_SEL && k == 3) ||
                                  ((m == MODE_IMPORT_SRC || m == MODE_IMPORT_DST) && k == 4));
        const uint32_t halo = at ? (sure && k == 0 ? RGBA(255, 120, 110, 255) : RGBA(120, 248, 255, 255))
                                 : job ? RGBA(120, 248, 255, 255) : 0;
        draw_button(d, c0, r0, c1, r1, s, lower[k], at, halo, at ? pulse : 0.1f, 0);
        if ((sure || m == MODE_MAIN) && drawn_n < HITS_MAX) drawn[drawn_n++] = (Hit){c0, r0, c1, r1, item};
    }
    EnterCriticalSection(&hit_lock);
    memcpy(hits, drawn, sizeof(Hit) * (size_t)drawn_n);
    hit_n = drawn_n, hit_w = W, hit_h = H;
    LeaveCriticalSection(&hit_lock);
    draw_footer(d, W, y1 + 26 * s, s, "A - Decide", title_pick ? "B - Back" : "B - Cancel",
                "Keyboard: arrows to move, Enter to decide, Esc to go back, 1-6 for a file    L / R or Page Up / Down: pages");
}

/* The file select comes in and goes as the game's own screens do: it swipes in from the right while it fades in, and
   swipes out (to the left for a pick, back to the right for B) while it fades out. Every colour's alpha times the fade
   and every x moved by the swipe, through a drawing table of the mod's own over the overlay's (the letters' fonts stay
   the overlay's: longshot_draw reads the overlay's own table) */
#define FADE_IN_MS 300
#define FADE_OUT_MS 220
#define SWIPE 0.18f   /* the swipe's length, in screen widths */
typedef struct { const CoopModDraw *d; float a; } Fade;
static uint32_t faded(uint32_t rgba, float a) { return (rgba & 0x00ffffffu) | (uint32_t)((float)(rgba >> 24) * a + 0.5f) << 24; }
static float ease_out(float t) { t = 1 - clamp01(t); return 1 - t * t * t; }
static void fade_rect(void *u, float x0, float y0, float x1, float y1, uint32_t rgba, float rounding) {
    const Fade *f = (const Fade *)u;
    f->d->rect(f->d->user, x0 + slide_dx, y0, x1 + slide_dx, y1, faded(rgba, f->a), rounding);
}
static void fade_text(void *u, float x, float y, float px, uint32_t rgba, const char *utf8) {
    const Fade *f = (const Fade *)u;
    f->d->text(f->d->user, x + slide_dx, y, px, faded(rgba, f->a), utf8);
}
static float fade_width(void *u, float px, const char *utf8) {
    const Fade *f = (const Fade *)u;
    return f->d->text_width(f->d->user, px, utf8);
}

static void overlay(void *user, const CoopModDraw *d) {
    (void)user;
    static int was_picking;
    static DWORD shown_since, hidden_since;
    const DWORD now = GetTickCount();
    const int picking = state == ST_PICKING;
    if (picking && !was_picking) shown_since = now;
    if (!picking && was_picking) hidden_since = now ? now : 1;
    was_picking = picking;
    const int fading_out = !picking && hidden_since && now - hidden_since < FADE_OUT_MS;
    if (picking || fading_out) {
        const float in = picking ? ease_out((float)(now - shown_since) / FADE_IN_MS) : 1.0f;
        const float out = picking ? 0.0f : ease_out((float)(now - hidden_since) / FADE_OUT_MS);
        Fade f = {d, in * (1 - out)};
        slide_dx = d->width * SWIPE * ((1 - in) + (closed_back ? out : -out));
        CoopModDraw view = *d;
        view.user = &f, view.rect = fade_rect, view.text = fade_text, view.text_width = fade_width;
        ld = longshot_draw(d);
        if (ld && !fonts_tried) load_fonts();
        draw_picker(&view);
        slide_dx = 0;
        font_off();
        ld = NULL;
        return;
    }
    if (!picking) hidden_since = 0;
    ld = NULL;
    if (toast[0] && GetTickCount() < toast_until) {
        const float s = d->height / 1080.0f;
        const float tw = d->text_width(d->user, 30 * s, toast);
        d->rect(d->user, 24 * s, d->height - 92 * s, 60 * s + tw, d->height - 36 * s, RGBA(28, 44, 120, 220), 12 * s);
        d->text(d->user, 42 * s, d->height - 82 * s, 30 * s, RGBA(255, 255, 255, 255), toast);
        if (message[0] && GetTickCount() < message_until)
            text_drop(d, 24 * s, d->height - 134 * s, 24 * s, RGBA(255, 232, 130, 255), message);
    }
}

/* --- start ---------------------------------------------------------------------------------------------------- */

static int setting_on(const char *k) {
    char v[16] = "";
    return api->setting(api, k, v, sizeof v) > 0 && v[0] == '1';
}

COOP_MOD_EXPORT int coop_mod_start(const CoopModApi *a) {
    if (!a || a->version < 1 || a->size < sizeof(CoopModApi)) return 0;
    api = a;
    InitializeCriticalSection(&pick_lock);
    InitializeCriticalSection(&input_lock);
    InitializeCriticalSection(&hit_lock);
    HMODULE exe = GetModuleHandleW(NULL);
    RootFn root = (RootFn)(void *)GetProcAddress(exe, "hos_coop_fs_save_root");
    pad_read = (PadReadFn)(void *)GetProcAddress(exe, "hos_coop_pad_read");
    if (!root || !root(canon, sizeof canon)) {
        say("save profiles " MOD_VERSION ": this game did not say where its save folder is (co-op patch 0008): off, "
            "the game plays its save folder as it is");
        return 0;
    }
    size_t len = strlen(canon);
    while (len && (canon[len - 1] == '\\' || canon[len - 1] == '/')) canon[--len] = 0;
    if (len > 5 && !_stricmp(canon + len - 5, "-coop")) canon[len -= 5] = 0;   /* (never at the start; to be sure) */
    make_folder_key();

    char env[32];
    if (GetEnvironmentVariableA("LICHII_COOP_PAD", env, sizeof env) && env[0]) pad_choice = atoi(env);
    else {   /* (co-op's own switch file beside the game, as Nucleus Coop's instances have it) */
        char ini[MAX_PATH], line[256];
        const DWORD n = GetModuleFileNameA(NULL, ini, sizeof ini);
        char *slash = n ? strrchr(ini, '\\') : NULL;
        if (slash) {
            snprintf(slash + 1, sizeof ini - (size_t)(slash + 1 - ini), "coop-options.ini");
            FILE *f = fopen(ini, "r");
            if (f) {
                while (fgets(line, sizeof line, f)) {
                    const char *p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    if (!_strnicmp(p, "LICHII_COOP_PAD", 15)) {
                        p += 15;
                        while (*p == ' ' || *p == '\t' || *p == '=') p++;
                        pad_choice = atoi(p);
                    }
                }
                fclose(f);
            }
        }
    }
    if (GetEnvironmentVariableA("SAVE_PROFILES_DEBUG", env, sizeof env) && env[0] == '1') debug_screens = 1;
    char clicks[512];
    if (GetEnvironmentVariableA("SAVE_PROFILES_TEST_CLICKS", clicks, sizeof clicks) && clicks[0]) {
        for (const char *c = clicks; *c && test_clicks_n < TEST_CLICKS_MAX;) {
            float x, y;
            if (sscanf(c, "%f,%f", &x, &y) == 2) test_click_x[test_clicks_n] = x, test_click_y[test_clicks_n++] = y;
            c = strpbrk(c, ";/");
            if (!c) break;
            c++;
        }
        say("test run: %d clicks", test_clicks_n);
    }
    if (GetEnvironmentVariableA("SAVE_PROFILES_TEST_PICK", env, sizeof env) && env[0]) {
        test_pick = atoi(env);
        const char *c = strchr(env, ',');
        if (c) test_pick_ms = (DWORD)atoi(c + 1);
        if (test_pick < 1 || test_pick > FILES) test_pick = 0;
    }

    HANDLE m = folder_lock();
    recover();
    active = read_state().active;
    folder_unlock(m);
    load_all_info();

    api->on_frame(api, frame, 0);
    api->on_overlay(api, overlay, 0);
    api->on_key(api, key, 0);
    v2 = a->version >= 2 && a->size >= offsetof(CoopModApi, hold_input) + sizeof(void *) && a->on_screen &&
         api->on_screen(api, on_screen, 0);
    if (!v2)
        say("this co-op's mod interface is version %u: the file select shows at the start (version 2 opens it from the "
            "title's Continue)", a->version);
    if (v2) {   /* (the title's Continue opens it: nothing to pause at the start) */
        mark_live();
        say("save profiles " MOD_VERSION ": file %d of %d in %s; the title's Continue opens the file select", active,
            FILES, canon);
        return 1;
    }

    const char *why = NULL;
    DWORD other = 0;
    if (setting_on("skip")) why = "the Mods tab skips the file select";
    else if ((other = other_live()) != 0) why = "another game window already plays from this save folder";
    else if (!(save_thread = find_thread(SAVE_THREAD))) why = "the game's save thread was not found";
    else if (!suspend_idle(save_thread)) {
        CloseHandle(save_thread);
        save_thread = NULL;
        why = "the game's save thread was busy";
    }
    if (why) {
        mark_live();
        say("save profiles " MOD_VERSION ": file %d of %d in %s; no file select this time: %s", active, FILES, canon, why);
        return 1;
    }
    picking_since = GetTickCount();
    set_cursor(active - 1);
    state = ST_PICKING;
    HANDLE w = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (w) CloseHandle(w);
    say("save profiles " MOD_VERSION ": file select up (file %d of %d in %s, pad %d); the save thread waits for it",
        active, FILES, canon, pad_choice);
    return 1;
}

COOP_MOD_EXPORT void coop_mod_stop(void) {
    if (state == ST_PICKING && save_thread) ResumeThread(save_thread);
    if (save_thread) CloseHandle(save_thread), save_thread = NULL;
}
