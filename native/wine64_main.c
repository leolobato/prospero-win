/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Native entry for running Wine's own Unix side inside the title.
 *
 * The title runs one game per process (src/pw_wine_launch.h). Started with
 * no game it shows the launcher, without loading Wine; choosing a game
 * restarts the title with sceSystemServiceLoadExec and the game's
 * arguments. Holding Options+Create for 3 s in a game sends it Alt+F4. The
 * generic title restarts its launcher after Wine exits, or when the game does
 * not close in time; a descriptor-backed game title records the session and
 * returns to Home.
 *
 * The launcher and a game request the /data mount, where the library is.
 * A game then loads ntdll.prx, prepares Wine's
 * environment and enters __wine_main on a dedicated thread
 * (src/pw_wine_start.c). Wine's stderr reaches ps5log/1 through the sink
 * ntdll exports (Wine patch 0560), so its debug channels become telemetry;
 * the main thread shows the frames Wine's user driver presents (patch 0400)
 * on VideoOut, turns DualSense buttons into Wine key events, and reports
 * Wine's address-space counters until Wine exits the process or the run
 * deadline passes. Wine's audio driver plays its mix on the console's main
 * audio port through the title's audio sink.
 *
 * A single-program title (tools/build_native.sh, PW_FIXED_PROFILE) has no
 * launcher: started with no game it starts its one profile, and where the
 * title would go back to the launcher it closes, to the console's home.
 */
#include "ps5log/ps5log.h"
#include "pw_diagnostics.h"
#undef PS5LOG_LOG
#define PS5LOG_LOG(...) pw_diagnostics_log(__VA_ARGS__)
#ifndef PW_BUILD_ID
#define PW_BUILD_ID "unknown"
#endif
#include "../src/pw_wine_start.h"
#include "../src/pw_launcher_render.h"
#include "../src/pw_script_input.h"
#include "../src/pw_title_launch.h"
#include "../src/pw_title_id.h"
#include "../src/pw_title_session.h"
#include "../src/pw_wine_launch.h"
#include "../src/pw_game_menu.h"
#include "pw_data_mount.h"
#include "pw_audio_ps5.h"
#include "pw_pad_ps5.h"
#include "pw_hid_ps5.h"
#include "pw_osk_ps5.h"
#include "pw_videoout_ps5.h"
#include "pw_wine_library.h"
#include "pw_wine_prefix.h"
#include "pw_title_publication_native.h"
#include "pw_title_publication_ps5_lock.h"
#include "../src/pw_present.h"
#include "../src/pw_spinner.h"
#include "../src/pw_tsc_calibrate.h"
#include "pw_wine_display.h"
#include "../include/prospero_win.h"

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PW_TITLE_ID
#define PW_TITLE_ID "PPSA99995"
#endif
#define PW_APP_NAME "prospero-win-wine64"
#ifndef PW_FIXED_PROFILE
/* The id of the one profile a single-program title runs; "" for the launcher. */
#define PW_FIXED_PROFILE ""
#endif
static const char fixed_profile[] = PW_FIXED_PROFILE;
static int single_program(void) { return fixed_profile[0] != '\0'; }
#ifndef PW_TITLE_PROBE
#define PW_TITLE_PROBE 0
#endif
#ifndef PW_MANAGED_DESCRIPTOR_REQUIRED
#define PW_MANAGED_DESCRIPTOR_REQUIRED 0
#endif

/* The ID this process runs as, resolved at startup (src/pw_title_id.h):
 * one shell binary serves every title a launcher makes on the console, and
 * PW_TITLE_ID is only the fallback. */
static char title_id[PW_TITLE_ID_SIZE];

/* As PS5-Lapy-JB-Daemon's helper reads it; the kernel may write more. */
struct pw_app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c + 0x400];
};
int sceKernelGetAppInfo(pid_t pid, struct pw_app_info *info);

static int kernel_title_id(char *title, size_t size)
{
    struct pw_app_info info;

    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(getpid(), &info) != 0) return -1;
    info.title_id[sizeof(info.title_id) - 1] = '\0';
    snprintf(title, size, "%s", info.title_id);
    return 0;
}

/* Where the title's Wine runtime can be seen: /app0 inside the sandbox;
 * once /data is granted the process sees the real root, where /app0 does
 * not exist (measured), and the sandbox's view of app0 is used instead. */
#define PW_WINE64_RUNTIME "/win/wine/lib/wine/x86_64-unix"
/* Its 64-bit PE modules, among them the WoW64 CPU backends. */
#define PW_WINE64_PE_RUNTIME "/win/wine/lib/wine/x86_64-windows"
static char sandbox_app0[sizeof("/mnt/sandbox/_000/app0") + PW_TITLE_ID_SIZE];
static const char *const runtime_roots[] = { "/app0", sandbox_app0 };
/* The title's data: profiles/, input/, prefix/ and prefixes/<name>. */
#ifndef PW_WINE64_ROOT_DATA
#define PW_WINE64_ROOT_DATA "/data/prospero-win"            /* when /data is granted */
#endif
#ifndef PW_WINE64_ROOT_SANDBOX
#define PW_WINE64_ROOT_SANDBOX "/download0/prospero-win"    /* sandbox fallback */
#endif
#ifndef PW_WINE64_DEBUG
/* No +seh: WoW64 callback returns unwind with 80000026 many times a second
 * and would flood the sink. */
#define PW_WINE64_DEBUG "err+all,+loaddll,+process"
#endif
#ifndef PW_WINE64_SECONDS
/* A game closes itself this long after it started; 0 lets it run until it
 * is closed. Pinball presents its first frame about 31 s in (run 13). */
#define PW_WINE64_SECONDS 0
#endif
#ifndef PW_WINE64_WAIT_WATCHDOG
/* 1 turns on Wine's wait watchdog (patch 0680): a snapshot of what every
 * thread waits on every two seconds, for stalls that logging hides. */
#define PW_WINE64_WAIT_WATCHDOG 0
#endif
#ifndef PW_WINE64_SCRIPT
/* 1 drives the launcher unattended for validation: it opens
 * PW_WINE64_SCRIPT_CYCLES games, one per cycle in the library's order
 * (starting again after the last), each closed by the deadline above. */
#define PW_WINE64_SCRIPT 0
#endif
#ifndef PW_WINE64_SCRIPT_CYCLES
#define PW_WINE64_SCRIPT_CYCLES 2
#endif

#if PW_WINE64_SCRIPT
/* Unattended runs may also press keys in the game, from <root>/pw_script_keys:
 * one "<milliseconds after the game starts> <virtual-key code>" per line, for
 * instance "45000 0x0d" for ENTER. Each is posted as a press and release. */
#define PW_WINE64_SCRIPT_KEYS_MAX 64
static struct { uint64_t at_ns; uint32_t vk; } script_keys[PW_WINE64_SCRIPT_KEYS_MAX];
static size_t script_key_count, script_key_next;

static void script_keys_load(const char *root)
{
    char path[512];
    FILE *file;
    unsigned long ms, vk;

    script_key_count = script_key_next = 0;
    snprintf(path, sizeof(path), "%s/pw_script_keys", root);
    if (!(file = fopen(path, "r"))) return;
    while (script_key_count < PW_WINE64_SCRIPT_KEYS_MAX && fscanf(file, "%lu %li", &ms, (long *)&vk) == 2) {
        script_keys[script_key_count].at_ns = (uint64_t)ms * 1000000u;
        script_keys[script_key_count].vk = (uint32_t)vk;
        script_key_count++;
    }
    fclose(file);
    PS5LOG_LOG("PW_WINE64 script keys=%u", (unsigned)script_key_count);
}

/* A recorded macro (src/pw_script_input.h) from <root>/pw_script_input,
 * replayed through the path a USB keyboard and mouse take. */
static PwScriptInput script_input;
static size_t script_input_next;
/* What its pad and stick events hold: the game's XInput controller 0 adds
 * it to the DualSense's state every frame (xinput mode only). */
static PwScriptPad script_pad;
static uint64_t script_input_start_ns; /* 0 until its sync text appeared */
static PwScriptInputSync script_input_sync;
static long script_input_sync_offset;
static char script_input_sync_file[768];

static void script_input_load(const char *root)
{
    char path[512];
    FILE *file;
    long length = -1;
    char *text = MAP_FAILED;
    size_t bad = 0, capacity = 0, text_bytes = 0, storage_bytes = 0;
    PwScriptInputEvent *storage = MAP_FAILED;
    int status = PW_ERR_TRUNCATED; /* read short */

    memset(&script_input, 0, sizeof(script_input));
    memset(&script_pad, 0, sizeof(script_pad));
    script_input_next = 0;
    script_input_start_ns = 0;
    snprintf(path, sizeof(path), "%s/pw_script_input", root);
    if (!(file = fopen(path, "rb"))) return;
    /* The title's heap is small: the text and the events get pages of their own. */
    if (!fseek(file, 0, SEEK_END) && (length = ftell(file)) > 0 && !fseek(file, 0, SEEK_SET)) {
        text_bytes = (size_t)length;
        text = mmap(NULL, text_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (text != MAP_FAILED && fread(text, 1, text_bytes, file) == text_bytes) {
            capacity = pw_script_input_lines(text, text_bytes);
            storage_bytes = capacity * sizeof(*storage);
            storage = mmap(NULL, storage_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            status = storage == MAP_FAILED ? PW_ERR_VM :
                     pw_script_input_parse(text, text_bytes, storage, capacity, &script_input, &bad);
        }
    }
    fclose(file);
    if (text != MAP_FAILED) munmap(text, text_bytes);
    if (status != PW_OK && storage != MAP_FAILED) {
        munmap(storage, storage_bytes);
        memset(&script_input, 0, sizeof(script_input));
    }
    if (script_input.sync_path[0]) {
        snprintf(script_input_sync_file, sizeof(script_input_sync_file), "%s/%s", root, script_input.sync_path);
        script_input_sync = (PwScriptInputSync){ script_input.sync_text, 0 };
        /* Only what the file gains from now on: a game's log may keep
         * earlier sessions (Source's -condebug appends). */
        script_input_sync_offset = 0;
        if ((file = fopen(script_input_sync_file, "rb"))) {
            if (!fseek(file, 0, SEEK_END)) script_input_sync_offset = ftell(file);
            if (script_input_sync_offset < 0) script_input_sync_offset = 0;
            fclose(file);
        }
    }
    PS5LOG_LOG("PW_WINE64 script input status=%s events=%u bad_line=%u sync=%s pad_events=%u",
               pw_result_name(status), (unsigned)script_input.count, (unsigned)bad,
               script_input.sync_path[0] ? script_input.sync_path : "-", (unsigned)script_input.pad_events);
}

/* With a sync line, reads what the file gained since the last look; the
 * replay's clock starts once the text appeared. */
static void script_input_poll_sync(uint64_t now)
{
    char chunk[4096];
    size_t got;
    FILE *file;

    if (script_input_start_ns || !script_input.count) return;
    if (!script_input.sync_path[0]) {
        script_input_start_ns = now;
        return;
    }
    if (!(file = fopen(script_input_sync_file, "rb"))) return;
    if (!fseek(file, script_input_sync_offset, SEEK_SET)) {
        while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
            script_input_sync_offset += (long)got;
            if (pw_script_input_sync_feed(&script_input_sync, chunk, got)) {
                script_input_start_ns = now;
                PS5LOG_LOG("PW_WINE64 script input synced at offset=%ld", script_input_sync_offset);
                break;
            }
        }
    }
    fclose(file);
}
#endif

/* The games, from <root>/profiles (native/pw_wine_library.h); nothing is
 * built in. catalog holds the valid ones for pw_wine_launch. */
static PwWineLibrary library;
static PwWineApp catalog[PW_WINE_LIBRARY_MAX];
static char catalog_detail[PW_WINE_LIBRARY_MAX][64];
static size_t catalog_count;
static const PwGameProfile *per_game_profile;
static const char *library_root = PW_WINE64_ROOT_SANDBOX;
static int diagnostics_opened;
static char per_game_library_root[sizeof(((PwTitleLaunch *)0)->data_root) + sizeof("/library")];
static PwTitleLaunch title_launch;
static PwTitlePublicationDescriptor title_descriptor;
static PwTitlePublicationLease title_publication = { .lock_fd = -1 };
static PwTitleSession title_session;
static const char *title_session_argument;
static int per_game_mode;
static volatile int per_game_running;

/* The largest desktop shown: Wine's PS5 driver defaults to 800x600
 * (WINE_PS5_DESKTOP overrides, 1920x1080 for Warcraft III); bigger frames
 * are counted and dropped. */
enum { PW_WINE64_MAX_FRAME = 1920 * 1200 * 4, PW_WINE64_TICK_US = 16000,
       PW_WINE64_TICKS_PER_S = 60, PW_WINE64_CLOSE_WAIT_S = 5, PW_WINE64_LEAVE_WAIT_S = 5,
       /* Options+Create are also Start+Select, which games use together:
        * closing takes a hold no game asks for. */
       PW_WINE64_CLOSE_HOLD_S = 3 };

enum { PAD_CREATE = 0x1u, PAD_OPTIONS = 0x8u, PAD_UP = 0x10u, PAD_RIGHT = 0x20u,
       PAD_DOWN = 0x40u, PAD_LEFT = 0x80u, PAD_L1 = 0x400u, PAD_R1 = 0x800u,
       PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u, PAD_CLOSE = PAD_OPTIONS | PAD_CREATE };
/* The pad core needs a key map to open; the title reads raw edges and maps
 * them with the game's bindings (pw_game_profile). */
static const PwPadKeyMap pad_open_map[] = { { PAD_CROSS, 0x20, 0, 0, "cross" } };


int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                                 uint32_t flags, const void *option, int *result);
int sceKernelGetModuleInfo(int32_t handle, void *info);
int sceKernelAvailableFlexibleMemorySize(size_t *bytes);

/* libkernel's description of one mapping of the title's own address space
 * (the layout every PS4 and PS5 SDK documents). */
typedef struct {
    uintptr_t start, end;
    int64_t offset;
    int32_t protection, memory_type;
    uint8_t flags;     /* bit 0 flexible, 1 direct, 2 stack, 3 pooled, 4 committed */
    char name[32];
} PwVirtualQueryInfo;
int sceKernelVirtualQuery(const void *address, int flags, void *info, size_t size);

/* Who holds the flexible memory: the title's mappings of it, summed by name
 * and protection, largest first. Logged once when less than 64 MiB is left
 * and once below 4 MiB, the levels at which a browser's views and then a
 * new thread's translator state were refused. */
static void log_flexible_holders(size_t available)
{
    enum { BUCKETS = 64, TOP = 16 };
    struct { char name[33]; int32_t protection; uint64_t bytes; uint32_t count; } bucket[BUCKETS];
    unsigned used = 0, other = 0;
    uint64_t total = 0, unnamed = 0;
    uintptr_t address = 0;
    PwVirtualQueryInfo info;

    memset(bucket, 0, sizeof(bucket));
    for (unsigned steps = 0; steps < 200000; steps++) {
        uint64_t bytes;
        unsigned i;

        memset(&info, 0, sizeof(info));
        if (sceKernelVirtualQuery((const void *)address, 1, &info, sizeof(info)) || info.end <= address) break;
        address = info.end;
        if (!(info.flags & 1)) continue;
        bytes = info.end - info.start;
        total += bytes;
        info.name[sizeof(info.name) - 1] = 0;
        if (!info.name[0]) unnamed += bytes;
        for (i = 0; i < used; i++)
            if (bucket[i].protection == info.protection && !strcmp(bucket[i].name, info.name)) break;
        if (i == used) {
            if (used == BUCKETS) { other++; continue; }
            used++;
            memcpy(bucket[i].name, info.name, sizeof(info.name));
            bucket[i].protection = info.protection;
        }
        bucket[i].bytes += bytes;
        bucket[i].count++;
    }
    PS5LOG_LOG("PW_WINE64 flexible_holders available=%zuK mapped=%lluK unnamed=%lluK buckets=%u overflow=%u",
               available >> 10, (unsigned long long)(total >> 10), (unsigned long long)(unnamed >> 10), used,
               other);
    for (unsigned rank = 0; rank < TOP && rank < used; rank++) {
        unsigned best = rank;
        for (unsigned i = rank + 1; i < used; i++)
            if (bucket[i].bytes > bucket[best].bytes) best = i;
        if (best != rank) {
            __typeof__(bucket[0]) swap = bucket[rank];
            bucket[rank] = bucket[best];
            bucket[best] = swap;
        }
        PS5LOG_LOG("PW_WINE64 flexible_holder rank=%u bytes=%lluK count=%u prot=%#x name=%s", rank + 1,
                   (unsigned long long)(bucket[rank].bytes >> 10), bucket[rank].count,
                   (unsigned)bucket[rank].protection, bucket[rank].name[0] ? bucket[rank].name : "-");
    }
}

static uint64_t now_ns(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0u;
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

/* [runtime] fast_clock: the readers pw_tsc_measure takes. */
static uint64_t calibration_clock(void *context) { (void)context; return now_ns(); }
static uint64_t calibration_tsc(void *context) { (void)context; return __builtin_ia32_rdtsc(); }

/* Read the immutable per-game descriptor while this title's /app0 is still
 * visible. 0 means the generic title has no descriptor, 1 means per-game
 * mode, and -1 means a present descriptor was refused. */
static int load_title_descriptor(char *error, size_t error_size)
{
    static char bytes[PW_TITLE_LAUNCH_MAX_BYTES + 1];
    static const char path[] = "/app0/game-launch.json";
    size_t used = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        if (errno == ENOENT && !PW_MANAGED_DESCRIPTOR_REQUIRED) return 0;
        snprintf(error, error_size, "cannot open descriptor: %s", strerror(errno));
        return -1;
    }
    for (;;) {
        ssize_t got = read(fd, bytes + used, sizeof(bytes) - used);
        if (got < 0) {
            snprintf(error, error_size, "cannot read descriptor: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (!got) break;
        used += (size_t)got;
        if (used == sizeof(bytes)) {
            snprintf(error, error_size, "descriptor exceeds %u bytes",
                     (unsigned)PW_TITLE_LAUNCH_MAX_BYTES);
            close(fd);
            return -1;
        }
    }
    close(fd);
    if (pw_title_publication_descriptor_parse((const uint8_t *)bytes, used,
            title_id, &title_descriptor, error, error_size) != 0)
        return -1;
    title_launch = title_descriptor.launch;
    return 1;
}

/* Registered before admission: every normal exit retains the reader lease
 * until session reporting has finished. Kernel process teardown closes it on
 * abnormal termination. Never release it on primary-program return. */
static void close_title_publication(void)
{
    pw_title_publication_close(&title_publication);
}

static int per_game_argument(int argc, char **argv, char *error, size_t error_size)
{
    const char *argument = NULL;

    for (int i = 0; i < argc; i++) {
        if (!argv || !argv[i] || !argv[i][0]) continue;
        if (argument) {
            snprintf(error, error_size, "per-game launch accepts one session argument");
            return -1;
        }
        argument = argv[i];
    }
    if (argument && (strncmp(argument, "session=", 8) ||
                     !pw_title_session_id_valid(argument + 8))) {
        snprintf(error, error_size, "per-game launch rejects argument %.32s", argument);
        return -1;
    }
    title_session_argument = argument;
    return 0;
}

/* ---- Wine stderr -> ps5log, one line per text line -------------------- */

static char sink_line[1024];
static size_t sink_used;
static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long sink_calls;

static void wine_output_sink(const char *str, size_t len)
{
    sink_calls++;
    pthread_mutex_lock(&sink_lock);
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n' || sink_used == sizeof(sink_line) - 1) {
            sink_line[sink_used] = 0;
            PS5LOG_LOG("WINE %s", sink_line);
            sink_used = 0;
            if (str[i] == '\n') continue;
        }
        sink_line[sink_used++] = str[i];
    }
    pthread_mutex_unlock(&sink_lock);
}

/* ---- frames and input -------------------------------------------------- */

/* Wine presents on its own threads: keep a copy for the main thread. The
 * two frame buffers are mapped, not static: the title image must stay clear
 * of the low range PE32 executables occupy, and libc's heap is too small. */
static PwWineFrameBox frames;
static uint8_t *frame_shown;

/* A created Wine thread is still bootstrap: a missing executable or DLL can
 * fail there. Only validated game presentation (a nonblank frame actually
 * shown, or sustained Vulkan presentation) advances the durable record. */
static void observe_game_start(void)
{
    int expected = 0;

    if (!per_game_mode || !__atomic_compare_exchange_n(&per_game_running, &expected, 1, 0,
                                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    int session_status = pw_title_session_running(&title_session);
    PS5LOG_LOG("PW_WINE64 session id=%s phase=running evidence=graphics status=%d",
               title_session.session_id, session_status);
}

static int wine_present(void *context, const void *bgra, uint32_t width, uint32_t height,
                        uint32_t stride)
{
    (void)context;
    return pw_wine_frame_box_put(&frames, bgra, width, height, stride);
}

/* A game presenting with Vulkan scans out through the GPU driver's own video
 * output (patch 0460), which the title must close first. The main thread
 * owns the title's, so the driver's request waits for it to be closed. */
static int display_request, display_closed;
enum { PW_WINE64_QUIESCE_MS = 500 };

/* Wine's dlsym, for the exports of the modules it loaded (libvulkan.prx). */
static void *(*wine_dlsym_fn)(void *, const char *);

/* The console's on-screen keyboard (native/pw_osk_ps5.h): a dialog still up
 * when the title leaves the game is ended with it. */
static PwOskPs5 osk_ps5;

/* Once the display is Vulkan's, the game's last flips are still on their
 * way when Wine exits: RADV's own flip thread submits them after the game's
 * threads are gone. A process replaced with such a flip queued stayed at
 * the system's "Closing" screen (Project Eagle, 2026-10-12). libvulkan.prx
 * drains them (pw_videoout_quiesce); an older driver has no such export. */
static void display_quiesce(const char *reason)
{
    int (*quiesce)(unsigned, unsigned *, int *);
    unsigned waited = 0;
    int pending = 0, status;

    pw_osk_ps5_close(&osk_ps5);
    if (!__atomic_load_n(&display_closed, __ATOMIC_ACQUIRE) || !wine_dlsym_fn) return;
    quiesce = (int (*)(unsigned, unsigned *, int *))(uintptr_t)wine_dlsym_fn(NULL, "pw_videoout_quiesce");
    if (!quiesce) {
        PS5LOG_LOG("PW_WINE64 display quiesce reason=%s status=unavailable", reason);
        return;
    }
    status = quiesce(PW_WINE64_QUIESCE_MS, &waited, &pending);
    PS5LOG_LOG("PW_WINE64 display quiesce reason=%s status=%d waited_ms=%u pending=%d", reason, status,
               waited, pending);
}

static int wine_release_display(void *context)
{
    (void)context;
    __atomic_store_n(&display_request, 1, __ATOMIC_RELEASE);
    for (int waited = 0; waited < 5000; waited++) {
        if (__atomic_load_n(&display_closed, __ATOMIC_ACQUIRE)) return 0;
        usleep(1000);
    }
    return -1;
}

/* ---- sound -------------------------------------------------------------- */

/* Wine's audio driver (wine/wineps5) mixes every stream a game plays into
 * one grain at a time and hands it to this sink, which plays it on the
 * console's main port. sceAudioOutOutput returns once the port has taken
 * the grain, so the port clocks the driver. */
_Static_assert(PW_WINE_AUDIO_GRAIN == PW_AUDIO_PS5_GRAIN && PW_WINE_AUDIO_RATE == PW_AUDIO_PS5_RATE,
               "Wine's audio grain is the port's");
static PwAudioPs5Ops audio_ops;
static int audio_port = -1;
/* Grains played, and those with any sound in them. */
static volatile unsigned long audio_grains, audio_audible;

static int wine_audio(void *context, const int16_t *frames)
{
    (void)context;
    audio_grains++;
    for (size_t i = 0; i < 2u * PW_WINE_AUDIO_GRAIN; i++)
        if (frames[i]) { audio_audible++; break; }
    return audio_ops.output(audio_port, frames) < 0 ? -1 : 0;
}

/* Scale a frame onto the whole screen as the profile asks (fit keeps the
 * aspect ratio: 800x600 becomes 1440x1080), straight into the scanout, and
 * flip it. */
/*
 * Once a game's Vulkan swapchain owns the video output, its GDI frames (a
 * DirectShow movie drawn outside Direct3D) are shown through RADV's VideoOut
 * WSI while no swapchain presents: libvulkan.prx's pw_videoout_idle and
 * pw_videoout_show_tiled, found with Wine's dlsym among the modules it has
 * loaded (a NULL handle), which never loads a second driver. The frame is
 * drawn as the title's own presenter draws it.
 */
typedef struct VulkanFrames {
    int resolved;
    int (*idle)(void);
    int (*show_tiled)(const void *, uint64_t, uint32_t, uint32_t);
    uint32_t *tiled;
    uint64_t shown, busy, failed;
} VulkanFrames;

static void vulkan_frames_resolve(VulkanFrames *vf, const PwPrxDescriptor *ntdll)
{
    void *(*wine_dlsym)(void *, const char *) = (void *(*)(void *, const char *))
        (uintptr_t)pw_prx_lookup(ntdll, "dlsym");

    vf->resolved = 1;
    if (wine_dlsym) {
        vf->idle = (int (*)(void))(uintptr_t)wine_dlsym(NULL, "pw_videoout_idle");
        vf->show_tiled = (int (*)(const void *, uint64_t, uint32_t, uint32_t))
            (uintptr_t)wine_dlsym(NULL, "pw_videoout_show_tiled");
    }
    if (vf->idle && vf->show_tiled) {
        void *tiled = mmap(NULL, PW_VIDEOOUT_PS5_FRAME_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
        vf->tiled = tiled == MAP_FAILED ? NULL : tiled;
    }
    PS5LOG_LOG("PW_WINE64 vulkan frames idle=%d show=%d buffer=%d", vf->idle != NULL,
               vf->show_tiled != NULL, vf->tiled != NULL);
}

/*
 * While a game loads, Wine and the game show nothing for seconds: the
 * title shows a spinner (src/pw_spinner.h) until the game's first frame
 * with content, or until it has presented with Vulkan for a second on end.
 * One present is not enough: a Direct3D game presents once when it creates
 * its device, then loads for seconds (Warcraft III: 25 s, measured). Blank
 * frames until then are not shown. Half a second passes first, so a game
 * that is quick to show something never flashes it.
 */
enum { PW_WINE64_SPINNER_DELAY_MS = 500, PW_WINE64_SPINNER_STEP_MS = 83,
       PW_WINE64_PRESENTING_MS = 1000 };
typedef struct Loading {
    int content;               /* the game has shown something */
    uint32_t *pixels;          /* PW_SPINNER_WIDTH x PW_SPINNER_HEIGHT */
    uint32_t step;
    uint64_t next_ns;
    uint64_t presenting_since;  /* the Vulkan driver busy without a pause, or 0 */
} Loading;

/* The game has shown something: no more spinner. */
static void loading_done(Loading *loading, const char *by)
{
    if (loading->content) return;
    loading->content = 1;
    PS5LOG_LOG("PW_WINE64 loading done by=%s spinner_steps=%u", by, loading->step);
}

/* The spinner's next image, or NULL when none is due. */
static const PwPresentFrame *loading_frame(Loading *loading, uint64_t now, uint64_t started,
                                           PwPresentFrame *frame)
{
    if (loading->content || now - started < PW_WINE64_SPINNER_DELAY_MS * 1000000ull ||
        now < loading->next_ns)
        return NULL;
    if (!loading->pixels) {
        void *pixels = mmap(NULL, (size_t)PW_SPINNER_WIDTH * PW_SPINNER_HEIGHT * 4,
                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pixels == MAP_FAILED) {
            loading_done(loading, "no-memory");
            return NULL;
        }
        loading->pixels = pixels;
    }
    pw_spinner_draw(loading->pixels, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH,
                    loading->step++);
    loading->next_ns = now + PW_WINE64_SPINNER_STEP_MS * 1000000ull;
    *frame = (PwPresentFrame){ (const uint8_t *)loading->pixels, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT,
                               PW_SPINNER_WIDTH * 4, PW_PRESENT_BGRX8 };
    return frame;
}

/* Whether a frame the game drew is shown: always once it has shown content. */
static int loading_admits(Loading *loading, const PwPresentView *view)
{
    if (!loading->content &&
        !pw_spinner_frame_blank(view->pixels, view->width, view->height, view->stride))
        loading_done(loading, "frame");
    return loading->content;
}

/* Draws frame for the Vulkan driver and shows it: 1 when shown. */
static int vulkan_frames_flip(VulkanFrames *vf, const PwPresentFrame *frame, int scaling)
{
    if (pw_videoout_ps5_draw_scaled(frame, scaling, 0x000000u, vf->tiled) != PW_OK) {
        vf->failed++;
        return 0;
    }
    int status = vf->show_tiled(vf->tiled, PW_VIDEOOUT_PS5_FRAME_BYTES, PW_VIDEOOUT_PS5_WIDTH,
                                PW_VIDEOOUT_PS5_HEIGHT);
    if (status == 0) vf->shown++;
    else if (status == 1) vf->busy++;
    else if (!vf->failed++) PS5LOG_LOG("PW_WINE64 vulkan frames show failed=%d", status);
    return status == 0;
}

/* 1 when the newest GDI frame, or the spinner while the game loads, was
 * shown through the Vulkan driver. A game presenting has shown content. */
static int vulkan_frames_show(VulkanFrames *vf, uint64_t *sequence, int scaling, Loading *loading,
                              uint64_t now, uint64_t started)
{
    PwPresentView view;
    PwPresentFrame spinner;
    const PwPresentFrame *next;

    if (!vf->tiled) return 0;
    if (!vf->idle()) {
        if (!loading->presenting_since) loading->presenting_since = now;
        else if (now - loading->presenting_since >= PW_WINE64_PRESENTING_MS * 1000000ull) {
            loading_done(loading, "vulkan");
            observe_game_start();
        }
        return 0;
    }
    loading->presenting_since = 0;
    if (pw_wine_frame_box_take(&frames, sequence, frame_shown, PW_WINE64_MAX_FRAME, &view) == 1 &&
        loading_admits(loading, &view)) {
        const PwPresentFrame frame = { view.pixels, view.width, view.height, view.stride,
                                       PW_PRESENT_BGRX8 };
        return vulkan_frames_flip(vf, &frame, scaling);
    }
    if ((next = loading_frame(loading, now, started, &spinner)))
        return vulkan_frames_flip(vf, next, PW_PRESENT_SCALE_FIT);
    return 0;
}

static int show_scaled(PwVideoOutPs5 *video, const PwPresentView *view, int scaling)
{
    const PwPresentFrame frame = { view->pixels, view->width, view->height, view->stride,
                                   PW_PRESENT_BGRX8 };

    return pw_videoout_ps5_present_scaled(video, &frame, scaling, 0x000000u);
}

/* ---- fd 1 and fd 2 -> ps5log ------------------------------------------- */

/* The in-process wineserver writes its errors with fprintf(stderr), and a
 * console program's output reaches Wine's standard output handle, fd 1,
 * neither of which a title shows: each becomes one end of a socket pair
 * (pipe() is not available and dup2 onto fd 2 is refused, measured) whose
 * lines a thread forwards as "WINESERVER ..." and "STDOUT ..." records. */
typedef struct PwFdForward { int reader; const char *tag; } PwFdForward;

static void *forward_fd(void *arg)
{
    const PwFdForward *forward = arg;
    char buffer[1024];
    size_t used = 0;

    for (;;) {
        ssize_t got = read(forward->reader, buffer + used, sizeof(buffer) - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
        for (;;) {
            char *newline = memchr(buffer, '\n', used);
            if (!newline && used < sizeof(buffer) - 1) break;
            size_t line = newline ? (size_t)(newline - buffer) : used;
            buffer[line] = 0;
            if (line && buffer[line - 1] == '\r') buffer[line - 1] = 0;
            PS5LOG_LOG("%s %s", forward->tag, buffer);
            line += newline ? 1u : 0u;
            memmove(buffer, buffer + line, used - line);
            used -= line;
        }
    }
    return NULL;
}

/* Close fd first: descriptors are allocated lowest first, so one end of the
 * new pair takes its number. Both ends of a socket pair are alike, so that
 * end is the writer and the other the reader, with no dup2. */
static int capture_fd(int fd, PwFdForward *forward)
{
    int pair[2];
    pthread_t thread;

    close(fd);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
    if (pair[0] == fd || pair[1] == fd) {
        forward->reader = pair[0] == fd ? pair[1] : pair[0];
    } else {
        /* lower numbers were free too: move the writer onto fd */
        forward->reader = pair[0];
        if (dup2(pair[1], fd) != fd) return -3;
    }
    return pthread_create(&thread, NULL, forward_fd, forward) ? -4 : 0;
}

/* ---- one game per process -------------------------------------------- */

static PwWineLaunch launch;

/* End a single-program title for the console's home. A process that ends
 * by itself, by exit() or _exit(0) alike, is reported by the console as
 * "Something went wrong with this game or app"; one the system closes is
 * not. So the title asks the system to close it, as the PS button's Close
 * does (sceSystemServiceKillApp, as homebrew_ps5-optional-shsrv's
 * bigapp-control closes a title), after its last flips are drained; if the
 * system refuses, or has not closed it in PW_WINE64_LEAVE_WAIT_S, it ends
 * itself. Does not return. */
static void leave_to_home(const char *reason)
{
    int app_id, rc = -1;

    display_quiesce(reason);
    app_id = sceSystemServiceGetAppIdOfRunningBigApp();
    PS5LOG_LOG("PW_WINE64 leave close app_id=0x%x", (unsigned)app_id);
    pw_diagnostics_close(reason);
    if (app_id > 0 && (rc = sceSystemServiceKillApp(app_id, -1, 0, 0)) == 0)
        for (unsigned i = 0; i < PW_WINE64_LEAVE_WAIT_S * 10u; i++) usleep(100000);
    pw_diagnostics_resume();
    PS5LOG_LOG("PW_WINE64 leave close rc=0x%08x: ending the process", (unsigned)rc);
    pw_diagnostics_close(reason);
    _exit(0);
}

/* Replace this process with the title again: app's game, or the launcher
 * when app is NULL. /app0 is not visible once /data is granted, so the
 * sandbox's view of the same eboot is tried next. Returns only on failure.
 * A single-program title has no launcher to go to: it closes itself for the
 * console's home instead (leave_to_home). */
static void restart_title(const PwWineApp *app, uint32_t cycle, const char *reason)
{
    static char sandbox_eboot[sizeof(sandbox_app0) + sizeof("/eboot.bin")];
    const char *const eboots[] = { "/app0/eboot.bin", sandbox_eboot };
    static char storage[2 * PW_WINE_LAUNCH_PATH_MAX + 64];
    char *next[PW_WINE_LAUNCH_ARGS];

    if (per_game_mode) {
        PS5LOG_LOG("PW_WINE64 per-game title will exit reason=%s", reason);
        return;
    }
    if (!app && single_program()) {
        PS5LOG_LOG("PW_WINE64 leave to=home cycle=%u reason=%s", (unsigned)cycle, reason);
        leave_to_home(reason);
    }

    if (!pw_wine_launch_argv(app, cycle, storage, sizeof(storage), next, PW_WINE_LAUNCH_ARGS)) {
        PS5LOG_LOG("PW_WINE64 restart refused: arguments do not fit");
        return;
    }
    display_quiesce(reason);
    snprintf(sandbox_eboot, sizeof(sandbox_eboot), "%s/eboot.bin", sandbox_app0);
    for (size_t i = 0; i < sizeof(eboots) / sizeof(eboots[0]); i++) {
        PS5LOG_LOG("PW_WINE64 restart to=%s cycle=%u reason=%s eboot=%s", app ? app->id : "launcher",
                   (unsigned)cycle, reason, eboots[i]);
        pw_diagnostics_close(reason);
        int rc = sceSystemServiceLoadExec(eboots[i], next);
        pw_diagnostics_resume();
        PS5LOG_LOG("PW_WINE64 restart failed rc=0x%08x", (unsigned)rc);
    }
}

/* A title that ends its process directly is reported by the system as a
 * crash (CE-108255-1, measured after HL2's own Quit). Ask the system to close
 * the per-game title instead, as other titles do, and wait here for it.
 * Returns only if the request is refused. */
static void return_to_home(void)
{
    close_title_publication();
    if (sceSystemServiceLoadExec("exit", NULL) != 0) return;
    for (;;) usleep(100000);
}

/* Wine ends the process with exit() when its last program ends. A generic
 * title returns to its catalog; a per-game title records the observation and
 * returns to Home. The callback does not carry Wine's exit status. */
static void on_exit_report(void)
{
    PS5LOG_LOG("PW_WINE64 exit sink_calls=%lu", sink_calls);
    if (per_game_mode) {
        if (__atomic_load_n(&per_game_running, __ATOMIC_ACQUIRE))
            (void)pw_title_session_exited(&title_session, 0, 0);
        else (void)pw_title_session_failed(&title_session, 0, 0);
        pw_title_session_close(&title_session);
    } else {
        restart_title(NULL, launch.cycle + 1u, "wine-exit");
    }
    pw_diagnostics_close("wine64-exit");
    if (per_game_mode) return_to_home();
}

/* ---- library ------------------------------------------------------------ */

/* Complete the one-shot helper exchange before loading profiles or starting
 * Wine. A failed or incomplete request closes this process to Home. */
static int open_library(void)
{
    PwDataMountResult mount;
    char session_error[160];
    int profile_path_refused = 0;
    int status;

    status = pw_data_mount_request(&mount);
    if (status == 0 && !per_game_mode) library_root = PW_WINE64_ROOT_DATA;
    PS5LOG_LOG("PW_WINE64 data_mount data_before=%d elevated_before=%d helper_completed=%d "
               "helper_errno=%d credential_errno=%d data_after=%d elevated_after=%d waited_ms=%d settled_ms=%d root=%s",
               mount.data_before, mount.elevated_before, mount.helper_completed,
               mount.helper_errno, mount.credential_errno, mount.data_after, mount.elevated_after, mount.waited_ms,
               mount.settled_ms, status == 0 ? library_root : "unavailable");
    if (status != 0) return -1;
    if (per_game_mode) {
        /* Open the session report as soon as the private library is reachable,
         * before publication admission, so a refused start leaves its reason
         * (buffered earlier records are flushed into it as well). */
        if (snprintf(per_game_library_root, sizeof(per_game_library_root), "%s/library",
                     title_launch.data_root) >= (int)sizeof(per_game_library_root))
            return -1;
        diagnostics_opened = pw_diagnostics_open(per_game_library_root, PW_BUILD_ID,
                                                 title_launch.default_profile_id, 0) == 0;
        /* The console starts titles with umask 0, so Wine would save the
         * private bottle's registry as 0666, which publication admission then
         * refuses on the next start. Everything this title creates is the app
         * user's. The libkernel gateway resolves only after the grant. */
        const int previous_mask = pw_title_ps5_umask(077);
        PS5LOG_LOG("PW_WINE64 umask private=%d previous=%o errno=%d", previous_mask >= 0,
                   previous_mask >= 0 ? (unsigned)previous_mask : 0u, previous_mask >= 0 ? 0 : errno);
        if (previous_mask < 0) return -1;
        const PwTitlePublicationPlatform platform = {
            .host_root = NULL, .owner_uid = (uint32_t)geteuid(),
            .try_lock_shared = pw_title_publication_ps5_lock, .context = NULL
        };
        if (pw_title_publication_open(&title_descriptor, &platform,
                &title_publication, session_error, sizeof(session_error)) != 0) {
            PS5LOG_LOG("PW_WINE64 publication refused reason=%s", session_error);
            return -1;
        }
        library_root = per_game_library_root;
        if (pw_title_session_start(&title_session, &title_launch, title_session_argument,
                                   session_error, sizeof(session_error)) != 0) {
            PS5LOG_LOG("PW_WINE64 session start refused reason=%s", session_error);
            return -1;
        }
        PS5LOG_LOG("PW_WINE64 session id=%s origin=%s phase=starting",
                   title_session.session_id, title_session.origin);
        /* The guard admitted and cached these bytes under the lease. Avoid
         * reopening profile/input files after their validation. */
        memset(&library, 0, sizeof(library));
        library.count = 1;
        library.entries[0].status = PW_OK;
        library.entries[0].profile = title_publication.profile;
    }
    status = per_game_mode ? PW_OK : pw_wine_library_load(&library, library_root);
    if (status != PW_OK && per_game_mode) {
        (void)pw_title_session_failed(&title_session, 0, 0);
        PS5LOG_LOG("PW_WINE64 library refused status=%s", pw_result_name(status));
        return -1;
    }
    catalog_count = 0;
    for (uint32_t i = 0; i < library.count; i++) {
        const PwWineLibraryEntry *entry = &library.entries[i];
        if (entry->status != PW_OK) {
            PS5LOG_LOG("PW_WINE64 profile refused file=%s status=%s", entry->file,
                       pw_result_name(entry->status));
            continue;
        }
        const PwGameProfile *game = &entry->profile;
        if (per_game_mode && strcmp(game->app.id, title_launch.default_profile_id)) continue;
        if (per_game_mode) {
            per_game_profile = game;
        }
        if (game->display.width)
            snprintf(catalog_detail[catalog_count], sizeof(catalog_detail[0]), "%s  %s  %ux%u",
                     game->app.architecture == PW_APP_ARCH_PE64 ? "pe64" : "pe32",
                     game->app.graphics == PW_APP_GRAPHICS_DXVK ? "dxvk" :
                     game->app.graphics == PW_APP_GRAPHICS_ZINK ? "opengl" : "gdi",
                     (unsigned)game->display.width, (unsigned)game->display.height);
        else
            snprintf(catalog_detail[catalog_count], sizeof(catalog_detail[0]), "%s  %s",
                     game->app.architecture == PW_APP_ARCH_PE64 ? "pe64" : "pe32",
                     game->app.graphics == PW_APP_GRAPHICS_DXVK ? "dxvk" :
                     game->app.graphics == PW_APP_GRAPHICS_ZINK ? "opengl" : "gdi");
        catalog[catalog_count] = (PwWineApp){ game->app.id, game->app.name,
                                              catalog_detail[catalog_count], game->app.executable };
        catalog_count++;
    }
    if (per_game_mode && (catalog_count != 1 || profile_path_refused)) {
        PS5LOG_LOG("PW_WINE64 default profile refused id=%s matches=%u",
                   title_launch.default_profile_id, (unsigned)catalog_count);
        (void)pw_title_session_failed(&title_session, 0, 0);
        return -1;
    }
    PS5LOG_LOG("PW_WINE64 library status=%s listed_by=%d scan_errno=%d entries=%u games=%u "
                   "root=%s", pw_result_name(status), library.listed_by, library.scan_error,
               (unsigned)library.count, (unsigned)catalog_count, library_root);
    return 0;
}

/* The USB keyboard and mouse's log lines. */
static void hid_log(const char *line)
{
    PS5LOG_LOG("PW_WINE64 hid %s", line);
}

/* The console's on-screen keyboard's. */
static void osk_log(const char *line)
{
    PS5LOG_LOG("PW_WINE64 osk %s", line);
}

/* The process's CPU time, user and system, in milliseconds: a stall that
 * waits shows no growth, one that spins grows by a core a second. */
static unsigned long long cpu_ms(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage)) return 0;
    return (unsigned long long)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000u +
           (unsigned long long)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000u;
}

/* ---- launcher ----------------------------------------------------------- */

/* Shown until a game is chosen; Wine is not loaded. Does not return. */
static void run_launcher(void)
{
    static PwVideoOutPs5 video;
    static PwPadPs5 pad;
    PwPadPs5Ops pad_ops;
    PwLauncherItem items[PW_WINE_LIBRARY_MAX];
    const size_t frame_bytes = (size_t)PW_LAUNCHER_RENDER_WIDTH * PW_LAUNCHER_RENDER_HEIGHT * 4u;
    uint8_t *frame = mmap(NULL, frame_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    PwLauncherScene scene = { items, 0, PW_LAUNCHER_RENDER_NONE,
        launch.refused ? "THAT GAME IS NOT IN THE LIBRARY" :
        !catalog_count ? "ADD PROFILES TO /DATA/PROSPERO-WIN/PROFILES" :
        launch.cycle ? "WELCOME BACK" : "CHOOSE A GAME" };
    static PwHidPs5 hid;
    int video_status = pw_videoout_ps5_open(&video), pad_status = pw_pad_ps5_platform_ops(&pad_ops);
    int hid_status, dirty = 1, chosen = -1;

    if (pad_status == PW_OK)
        pad_status = pw_pad_ps5_open(&pad, &pad_ops, pad_open_map,
                                     sizeof(pad_open_map) / sizeof(pad_open_map[0]));
    /* A USB keyboard works the launcher too, the pad's user's or else the
     * foreground user's, as in a game. */
    {
        int32_t user = pad_status == PW_OK ? pad.user_id : -1;
        if (user < 0 && pw_pad_ps5_platform_ops(&pad_ops) == PW_OK) {
            (void)pad_ops.user_initialize(NULL);
            (void)pad_ops.foreground_user(&user);
        }
        hid_status = pw_hid_ps5_open(&hid, pw_hid_ps5_system_ops(hid_log), user);
    }
    /* The games, then the refused profiles, listed by file as not available. */
    for (size_t i = 0; i < catalog_count; i++)
        items[scene.count++] = (PwLauncherItem){ catalog[i].name, catalog[i].detail, 1 };
    for (uint32_t i = 0; i < library.count; i++)
        if (library.entries[i].status != PW_OK)
            items[scene.count++] = (PwLauncherItem){ library.entries[i].file, "profile refused", 0 };
    if (scene.count) scene.selected = 0;
    PS5LOG_LOG("PW_WINE64 launcher video=%s pad=%s hid=%s frame=%d apps=%u cycle=%u refused=%u script=%d",
               pw_result_name(video_status), pw_result_name(pad_status), pw_result_name(hid_status),
               frame != MAP_FAILED,
               (unsigned)catalog_count, (unsigned)launch.cycle, launch.refused, PW_WINE64_SCRIPT);
    char log_status[80], report_path[96];
    snprintf(report_path, sizeof(report_path), "GET LOGS OVER FTP: %s/LOGS", library_root);
    int report_help = 0;
    pw_diagnostics_status(log_status, sizeof(log_status));
    scene.status = log_status;
    for (uint64_t tick = 1; chosen < 0; tick++) {
        pw_diagnostics_tick(now_ns());
        if (tick % PW_WINE64_TICKS_PER_S == 0 && !report_help) {
            pw_diagnostics_status(log_status, sizeof(log_status)); dirty = 1;
        }
        uint32_t before = scene.selected;
        if (pad_status == PW_OK && pw_pad_ps5_read(&pad) == PW_OK) {
            static const struct { uint32_t button; PwLauncherAction action; } pad_actions[] = {
                { PAD_RIGHT, PW_LAUNCHER_ACTION_RIGHT }, { PAD_LEFT, PW_LAUNCHER_ACTION_LEFT },
                { PAD_DOWN, PW_LAUNCHER_ACTION_DOWN }, { PAD_UP, PW_LAUNCHER_ACTION_UP },
                { PAD_CROSS, PW_LAUNCHER_ACTION_CHOOSE },
            };
            if (pad.core.pressed_edges & PAD_OPTIONS) {
                report_help = !report_help;
                scene.status = report_help ? report_path : log_status;
                dirty = 1;
            }
            for (size_t i = 0; i < sizeof(pad_actions) / sizeof(pad_actions[0]); i++)
                if ((pad.core.pressed_edges & pad_actions[i].button) &&
                    pw_launcher_navigate(&scene, pad_actions[i].action, (uint32_t)catalog_count))
                    chosen = (int)scene.selected;
        }
        /* Key presses; a keyboard plugged in later is found every few seconds. */
        if (hid.ops.load_module) {
            PwHidPs5Poll poll;

            if (hid_status != PW_OK && tick % (5 * PW_WINE64_TICKS_PER_S) == 0)
                hid_status = pw_hid_ps5_retry(&hid);
            pw_hid_ps5_poll(&hid, &poll);
            for (size_t i = 0; i < poll.count && chosen < 0; i++)
                if (poll.events[i].kind == PW_HID_EVENT_KEY && poll.events[i].down &&
                    pw_launcher_navigate(&scene, pw_launcher_key_action(poll.events[i].code),
                                         (uint32_t)catalog_count))
                    chosen = (int)scene.selected;
        }
        dirty |= scene.selected != before;
        if (PW_WINE64_SCRIPT && tick == 3 * PW_WINE64_TICKS_PER_S) {
            if (launch.cycle >= PW_WINE64_SCRIPT_CYCLES) {
                PS5LOG_LOG("PW_WINE64 launcher script done cycles=%u", (unsigned)launch.cycle);
                pw_diagnostics_close("wine64-script-done");
                _exit(0);
            }
            /* One game per cycle, in the library's order. */
            if (catalog_count) chosen = (int)(launch.cycle % catalog_count);
        }
        if (dirty && video_status == PW_OK && frame != MAP_FAILED) {
            const PwPresentTarget target = { frame, PW_LAUNCHER_RENDER_WIDTH, PW_LAUNCHER_RENDER_HEIGHT,
                                             PW_LAUNCHER_RENDER_WIDTH * 4u, frame_bytes };
            const PwPresentView view = { frame, target.width, target.height, target.stride,
                                           (uint32_t)frame_bytes };
            int status = pw_launcher_render(&scene, &target);
            if (status == PW_OK) status = pw_videoout_ps5_present(&video, &view);
            if (status != PW_OK) PS5LOG_LOG("PW_WINE64 launcher present=%s", pw_result_name(status));
            dirty = 0;
        } else {
            usleep(PW_WINE64_TICK_US);
        }
    }
    PS5LOG_LOG("PW_WINE64 launcher chose=%s", catalog[chosen].id);
    if (pad_status == PW_OK) (void)pw_pad_ps5_close(&pad);
    if (video_status == PW_OK) (void)pw_videoout_ps5_close(&video);
    restart_title(&catalog[chosen], launch.cycle, "launcher");
    pw_diagnostics_close("wine64-launch-failed");
    _exit(1);
}

/* ---- early fault report ------------------------------------------------ */

/* Until Wine installs its own handlers, a fault inside ntdll would end the
 * process silently; report where it happened relative to ntdll's segments.
 * RIP is read at the measured ucontext offset (224). */
static PwWineStart *fault_start;

static void early_fault(int sig, siginfo_t *info, void *opaque)
{
    uint64_t rip = 0, offset = 0;
    int segment = -1;

    memcpy(&rip, (const uint8_t *)opaque + 224, sizeof(rip));
    for (uint32_t i = 0; fault_start && i < fault_start->segment_count; i++) {
        uint64_t base = (uint64_t)(uintptr_t)fault_start->segments[i].address;
        if (rip >= base && rip < base + fault_start->segments[i].size) {
            segment = (int)i;
            offset = rip - base;
            break;
        }
    }
    pw_diagnostics_emergency(sig, (uintptr_t)(info ? info->si_addr : NULL), rip, segment, offset);
    _exit(3);
}

static void install_early_fault_report(PwWineStart *start)
{
    struct sigaction action;

    fault_start = start;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = early_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
}

/* ---- PS5 bindings for the start sequence ------------------------------ */

static int set_env(const char *name, const char *value)
{
    return setenv(name, value, 1);
}

struct thread_start { void (*entry)(void *); void *arg; };

static void *thread_trampoline(void *opaque)
{
    struct thread_start start = *(struct thread_start *)opaque;

    free(opaque);
    PS5LOG_LOG("PW_WINE64 thread entered, calling __wine_main");
    start.entry(start.arg);
    PS5LOG_LOG("PW_WINE64 __wine_main returned");
    return NULL;
}

static int start_thread(void (*entry)(void *), void *arg, size_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_t thread;
    struct thread_start *start = malloc(sizeof(*start));
    int status;

    if (!start) return -1;
    start->entry = entry;
    start->arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    status = pthread_create(&thread, &attr, thread_trampoline, start);
    pthread_attr_destroy(&attr);
    if (status) free(start);
    return status;
}

#if PW_TITLE_PROBE
/* Step-0 probe for one shell serving many titles: record every way this
 * process can learn its own title ID, before and after the /data grant, in
 * /app0/title-probe.txt (read back over FTP), then exit. */
#include <dirent.h>

int sysctl(const int *name, unsigned length, void *old, size_t *old_length,
           const void *new_value, size_t new_length);

static void probe_list(FILE *out, const char *label, const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    unsigned shown = 0;

    fprintf(out, "%s path=%s open=%d errno=%d\n", label, path, dir != NULL, dir ? 0 : errno);
    if (!dir) return;
    while ((entry = readdir(dir)) && shown < 64) {
        fprintf(out, "%s entry=%s\n", label, entry->d_name);
        shown++;
    }
    closedir(dir);
}

static void probe_identity(FILE *out, const char *phase)
{
    struct pw_app_info info;
    struct stat st;
    char text[1024];
    int mib[4] = { 1, 14, 12, -1 };   /* kern.proc.pathname of this process */
    size_t size = sizeof(text) - 1;
    int rc, fd;

    memset(&info, 0, sizeof(info));
    fprintf(out, "%s step=appinfo\n", phase);
    rc = sceKernelGetAppInfo(getpid(), &info);
    fprintf(out, "%s pid=%d uid=%d euid=%d\n", phase, (int)getpid(), (int)getuid(), (int)geteuid());
    fprintf(out, "%s appinfo rc=0x%x app_id=0x%x title_id=%.14s\n", phase, (unsigned)rc,
            (unsigned)info.app_id, info.title_id);
    fprintf(out, "%s app0 stat=%d errno=%d\n", phase, stat("/app0", &st), errno);
    memset(text, 0, sizeof(text));
    fprintf(out, "%s step=sysctl\n", phase);
    rc = sysctl(mib, 4, text, &size, NULL, 0);
    fprintf(out, "%s pathname rc=%d errno=%d value=%s\n", phase, rc, rc ? errno : 0, text);
    memset(text, 0, sizeof(text));
    /* getcwd terminates the title (CE-108255-1, measured) */
    fd = open("/app0/sce_sys/param.json", O_RDONLY);
    fprintf(out, "%s param open=%d errno=%d\n", phase, fd >= 0, fd >= 0 ? 0 : errno);
    if (fd >= 0) {
        char param[4096];
        ssize_t got = read(fd, param, sizeof(param) - 1);
        const char *key;
        close(fd);
        param[got > 0 ? got : 0] = 0;
        key = strstr(param, "\"titleId\"");
        fprintf(out, "%s param bytes=%zd titleId=%.32s\n", phase, got, key ? key : "(absent)");
    }
    probe_list(out, phase, "/mnt/sandbox");
}

/* With the title's probe-allow-list present, install it as a root-owned allow-list
 * for the rule-mode helper probe, the way a launcher would. */
#define PW_PROBE_ALLOW_DIR "/data/title-shell-probe/rule"
static void probe_allow_list(FILE *out)
{
    char list[256];
    struct stat st;
    char source[64];
    int in, fd;
    ssize_t got;

    snprintf(source, sizeof(source), "/data/homebrew/%s/probe-allow-list", title_id);
    in = open(source, O_RDONLY);
    if (in < 0) { fprintf(out, "allow-list source absent\n"); return; }
    got = read(in, list, sizeof(list));
    close(in);
    (void)mkdir("/data/title-shell-probe", 0755);
    (void)mkdir(PW_PROBE_ALLOW_DIR, 0755);
    (void)chmod(PW_PROBE_ALLOW_DIR, 0755);
    (void)unlink(PW_PROBE_ALLOW_DIR "/allow-list");
    fd = open(PW_PROBE_ALLOW_DIR "/allow-list", O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        if (got > 0 && write(fd, list, (size_t)got) != got) got = -1;
        (void)fchmod(fd, 0644);
        close(fd);
    }
    fprintf(out, "allow-list write fd=%d bytes=%zd errno=%d\n", fd, got, fd >= 0 ? 0 : errno);
    if (!lstat(PW_PROBE_ALLOW_DIR, &st))
        fprintf(out, "allow-list dir uid=%d mode=%o\n", (int)st.st_uid, (unsigned)(st.st_mode & 07777));
    if (!lstat(PW_PROBE_ALLOW_DIR "/allow-list", &st))
        fprintf(out, "allow-list file uid=%d mode=%o nlink=%d\n", (int)st.st_uid,
                (unsigned)(st.st_mode & 07777), (int)st.st_nlink);
}

static int title_probe(void)
{
    PwDataMountResult grant;
    FILE *out = fopen("/app0/title-probe.txt", "w");
    int rc;

    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);   /* every line lands even if a step crashes */
    fprintf(out, "build=%s compiled_title=%s resolved_title=%s\n", PW_BUILD_ID, PW_TITLE_ID, title_id);
    probe_identity(out, "before");
    fflush(out);
    fprintf(out, "step=grant\n");
    rc = pw_data_mount_request(&grant);
    fprintf(out, "grant rc=%d completed=%d helper_errno=%d credential_errno=%d data_after=%d\n", rc,
            grant.helper_completed, grant.helper_errno, grant.credential_errno, grant.data_after);
    probe_identity(out, "after");
    probe_list(out, "after", "/data/homebrew");
    {   /* can a granted title write a root-owned file (a launcher's allow-list)? */
        uid_t effective = geteuid();
        struct stat st;
        int raised = seteuid(0), raise_errno = raised ? errno : 0, fd = -1, stat_rc = -1;
        if (raised == 0) {
            char path[64];
            snprintf(path, sizeof(path), "/data/homebrew/%s/root-probe.txt", title_id);
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { stat_rc = fstat(fd, &st); close(fd); }
            probe_allow_list(out);
            (void)seteuid(effective);
        }
        fprintf(out, "root-write seteuid=%d errno=%d open=%d file_uid=%d mode=%o restored_euid=%d\n",
                raised, raise_errno, fd >= 0, stat_rc ? -1 : (int)st.st_uid,
                stat_rc ? 0 : (unsigned)(st.st_mode & 07777), (int)geteuid());
    }
    fclose(out);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    static char prefix[PW_WINE_LIBRARY_PATH + PW_APP_ID_CAPACITY], desktop[24], view[8] = "window";
    static char radv_cache[PW_WINE_LIBRARY_PATH + 16];
    enum { WINE64_FIXED_ENV_COUNT = 7,
           WINE64_PROFILE_ENV_CAPACITY = 4 + PW_GAME_GRAPHICS_ENV_MAX + PW_GAME_DISPLAY_ENV_MAX +
                                         PW_GAME_RUNTIME_ENV_MAX + PW_GAME_CPU_ENV_MAX + PW_GAME_CLOCK_ENV_MAX +
                                         PW_GAME_DEBUG_ENV_MAX + PW_GAME_ENVIRONMENT_MAX };
    static PwWineStartEnv extra[WINE64_FIXED_ENV_COUNT + WINE64_PROFILE_ENV_CAPACITY] = {
        { "WINEDEBUG", PW_WINE64_DEBUG },
        /* the i386 exe runs in this process through WoW64; otherwise Wine
         * starts it from start.exe in a new process, which a title cannot */
        { "WINEARCH", "wow64" },
        { "HOME", prefix },
        { "USER", "prospero" },
        { "WINE_PS5_TRACE_STARTUP", "1" },  /* patch 0560: name startup steps */
        { "WINE_PS5_VIEW", view },          /* patch 0430: the game's windows, or the desktop */
        /* RADV's shader cache. The driver's PS5 layer would put it under
         * /app0, which this title cannot write, and then run without one:
         * every launch compiled every pipeline again (2 s of compiles in
         * the first 100 s of GTA San Andreas, in 25-50 ms frames). The
         * driver keeps the first value, so this must be set before it
         * starts; a profile's [debug] env line still overrides it. */
        { "MESA_SHADER_CACHE_DIR", radv_cache },
        { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, { NULL, NULL }, /* profile-specific environment */
    };
    _Static_assert(sizeof(extra) / sizeof(extra[0]) ==
                   WINE64_FIXED_ENV_COUNT + WINE64_PROFILE_ENV_CAPACITY,
                   "profile environment capacity changed");
    _Static_assert(WINE64_FIXED_ENV_COUNT + WINE64_PROFILE_ENV_CAPACITY <= PW_WINE_START_MAX_ENV,
                   "more environment than pw_wine_start sets");
    /* wine, the executable, the profile's argument words, NULL */
    static const char *wine_argv[2 + PW_WINE_LAUNCH_WORDS + 1] = { "wine" };
    static char argument_words[PW_APP_ARGUMENTS_CAPACITY];
    static char effective_dll_overrides[PW_APP_DLL_OVERRIDES_CAPACITY + sizeof(";opengl32=n")];
    static char ntdll_dir[256], ntdll_path[288];
    static const PwWineStartOps ops = {
        sceKernelLoadStartModule, sceKernelGetModuleInfo, set_env, start_thread };
    static PwWineStartConfig config = {
        .ntdll_path = ntdll_path,
        .ntdll_dir = ntdll_dir,
        .prefix = prefix,
        .extra_env = extra, .extra_env_count = WINE64_FIXED_ENV_COUNT,
        .argc = 2, .argv = wine_argv, .stack_bytes = 16u << 20,
    };
    static PwWineStart start;
    static PwVideoOutPs5 video;
    static PwPadPs5 pad;
    static PwHidPs5 hid;
    static PwGameInput game_input;
    /* [input] player2: a second signed-in user's DualSense as more keys. */
    static PwPadPs5 pad2;
    static PwGameInput game_input2;
    int player2 = 0, pad2_status = PW_ERR_NOT_FOUND;
    uint64_t pad2_seen = 0;   /* the tick pad 2 last reported a controller */
    const PwGameProfile *game = NULL;
    int scaling = PW_PRESENT_SCALE_FIT;
    PwWinePointer pointer = { 0, 0 };
    int (*post_input)(const PwWineInput *) = NULL;
    void (*set_pad)(const PwWinePad *) = NULL;
    int (*rumble)(uint32_t *, uint32_t *) = NULL;
    /* [display] menu: L3+R3 opens the menu, DXVK's HUD draws it (patch 0973). */
    void (*set_menu)(const struct pw_title_menu *) = NULL;
    static PwGameMenu game_menu;
    static PwPadPs5 game_pad;   /* the pad as the game sees it while the menu has it */
    /* A two-finger touchpad click opens the console's own keyboard over
     * the game, and the menu's "Keyboard input" does too; the accepted
     * line is typed into the game (src/pw_osk.h). */
    static PwOsk osk;
    int osk_status = PW_ERR_STATE;
    uint64_t vibrations = 0;
    int pad_seen = -1;
    uint64_t shown_sequence = 0, shown = 0, posted = 0, refused = 0;
    VulkanFrames vulkan_frames = { 0 };
    Loading loading = { 0 };
    ps5log_config log_config;
    PwPadPs5Ops pad_ops;
    int status, video_status = PW_ERR_STATE, pad_status = PW_ERR_STATE, hid_status = PW_ERR_STATE;
    uint64_t close_requested = 0, combo_ticks = 0, started = now_ns();
    char title_error[192];
    int title_from_kernel = pw_title_id_resolve(kernel_title_id, PW_TITLE_ID, title_id);

    if (title_from_kernel < 0) return 1;
    snprintf(sandbox_app0, sizeof(sandbox_app0), "/mnt/sandbox/%s_000/app0", title_id);
#if PW_TITLE_PROBE
    return title_probe();
#endif
    ps5log_config_defaults(&log_config);
    if (atexit(close_title_publication) != 0) return 1;
    if (ps5log_load_config(ps5log_default_conf_paths, ps5log_default_conf_path_count,
                           &log_config, NULL) == 0)
        ps5log_init(&log_config, title_id, PW_APP_NAME, now_ns());
    PS5LOG_LOG("PW_WINE64 title id=%s source=%s compiled=%s", title_id,
               title_from_kernel ? "kernel" : "compiled", PW_TITLE_ID);
    status = load_title_descriptor(title_error, sizeof(title_error));
    if (status < 0) {
        PS5LOG_LOG("PW_WINE64 startup blocked reason=invalid-descriptor detail=%s", title_error);
        ps5log_close("invalid-descriptor");
        return 1;
    }
    per_game_mode = status == 1;
    /* The private umask is set after the data grant (see open_library): the
     * libkernel gateway it uses is not resolvable before it. */
    if (per_game_mode && per_game_argument(argc, argv, title_error, sizeof(title_error)) != 0) {
        PS5LOG_LOG("PW_WINE64 startup blocked reason=invalid-per-game-arguments detail=%s",
                   title_error);
        ps5log_close("invalid-per-game-arguments");
        return 1;
    }
    pw_hid_ps5_preload(hid_log);   /* before the /data grant changes the title's credentials */
    pw_osk_ps5_preload(osk_log);   /* the dialog's module is a system module too */
    if (open_library() != 0) {
        PS5LOG_LOG("PW_WINE64 startup blocked reason=%s",
                   per_game_mode ? "per-game-bootstrap" : "data-helper-incomplete");
        ps5log_close(per_game_mode ? "per-game-bootstrap" : "data-helper-incomplete");
        return 1;
    }
    if (per_game_mode) {
        memset(&launch, 0, sizeof(launch));
        launch.mode = PW_WINE_LAUNCH_GAME;
        launch.app = &catalog[0];
        snprintf(launch.executable, sizeof(launch.executable), "%s", catalog[0].executable);
    } else {
        (void)pw_wine_launch_parse(argc, argv, catalog, catalog_count, &launch);
    }
    if (!diagnostics_opened)
        (void)pw_diagnostics_open(library_root, PW_BUILD_ID,
                                  launch.app ? launch.app->id : "launcher", launch.cycle);
    PS5LOG_LOG("PW_WINE64 args argc=%d mode=%s profile=%s cycle=%u refused=%u", argc,
               launch.mode == PW_WINE_LAUNCH_GAME ? "game" : "launcher",
               launch.app ? launch.app->id : "-", (unsigned)launch.cycle, launch.refused);
    if (launch.mode != PW_WINE_LAUNCH_GAME && single_program()) {
        /* A single-program title starts its profile as the launcher would. */
        for (uint32_t i = 0; i < catalog_count; i++)
            if (!strcmp(catalog[i].id, fixed_profile)) restart_title(&catalog[i], launch.cycle, "fixed-profile");
        PS5LOG_LOG("PW_WINE64 startup blocked reason=no-fixed-profile profile=%s", fixed_profile);
        ps5log_close("no-fixed-profile");
        return 1;
    }
    if (launch.mode != PW_WINE_LAUNCH_GAME) run_launcher();
    wine_argv[1] = launch.executable;
    /* The game's profile: its prefix, desktop, scaling and input. A bare
     * path= runs in the default prefix with nothing bound. */
    pw_game_input_init(&game_input);
    game = per_game_mode ? per_game_profile :
           (launch.app ? pw_wine_library_find(&library, launch.app->id) : NULL);
    if (game && strcmp(game->app.prefix, "default"))
        snprintf(prefix, sizeof(prefix), "%s/prefixes/%s", library_root, game->app.prefix);
    else
        snprintf(prefix, sizeof(prefix), "%s/prefix", library_root);
    snprintf(radv_cache, sizeof(radv_cache), "%s/cache/radv", library_root);
    if (per_game_mode && (!game || strcmp(game->app.prefix, "game") ||
                          !pw_title_path_is_directory(prefix))) {
        PS5LOG_LOG("PW_WINE64 per-game prefix refused profile=%s prefix=%s",
                   game ? game->app.id : "-", prefix);
        (void)pw_title_session_failed(&title_session, 0, 0);
        pw_title_session_close(&title_session);
        pw_diagnostics_close("per-game-prefix-refused");
        return 1;
    }
    if (per_game_mode) {
        char executable_path[PW_TITLE_SESSION_PATH_SIZE];

        if (pw_wine_launch_host_dir(prefix, launch.executable, executable_path,
                                    sizeof(executable_path)) != 0 ||
            !pw_title_path_is_regular_file(executable_path)) {
            PS5LOG_LOG("PW_WINE64 per-game executable refused exe=%s", launch.executable);
            (void)pw_title_session_failed(&title_session, 0, 0);
            pw_title_session_close(&title_session);
            pw_diagnostics_close("per-game-executable-refused");
            return 1;
        }
    }
    /* [display] refresh and frame_rate: the video output's rate and cap, set
     * for every game, so one without a refresh line, or a bare path=
     * without a profile, asks for 60 (src/pw_game_profile.h). */
    {
        static const PwGameDisplay display_default = { .refresh = 60 };
        const PwGameDisplay *display = game ? &game->display : &display_default;
        PwGameEnv display_env[PW_GAME_DISPLAY_ENV_MAX];
        size_t display_count = pw_game_display_env(display, display_env);

        for (size_t i = 0; i < display_count; i++)
            extra[config.extra_env_count++] = (PwWineStartEnv){ display_env[i].name, display_env[i].value };
        PS5LOG_LOG("PW_WINE64 display refresh=%u frame_rate=%u", (unsigned)display->refresh,
                   (unsigned)display->frame_rate);
    }
    if (game) {
        int input_status;
        if (per_game_mode) {
            if (title_publication.has_preset) game_input = title_publication.preset;
            pw_game_input_overlay(&game_input, &game->input);
            pw_game_input_default_mode(&game_input);
            input_status = PW_OK;
        } else {
            input_status = pw_wine_library_input(game, library_root, &game_input);
        }
        int input2_status = pw_wine_library_player2_input(game, library_root, &game_input2);
        player2 = input2_status == PW_OK;
        if (input2_status != PW_ERR_NOT_FOUND)
            PS5LOG_LOG("PW_WINE64 player2 preset=%s input=%s", game->input.player2,
                       pw_result_name(input2_status));
        scaling = (int)game->display.scaling;
        if (game->display.view == PW_GAME_VIEW_DESKTOP) snprintf(view, sizeof(view), "desktop");
        if (game->display.width) {
            snprintf(desktop, sizeof(desktop), "%ux%u", (unsigned)game->display.width,
                     (unsigned)game->display.height);
            extra[config.extra_env_count++] = (PwWineStartEnv){ "WINE_PS5_DESKTOP", desktop };
        }
        /* OpenGL selects the native Zink provider; preserve other per-game
         * overrides such as DXVK when composing it. */
        int overrides_status = pw_app_profile_effective_dll_overrides(
            &game->app, effective_dll_overrides, sizeof(effective_dll_overrides));
        if (overrides_status == PW_OK && effective_dll_overrides[0])
            extra[config.extra_env_count++] = (PwWineStartEnv){ "WINEDLLOVERRIDES", effective_dll_overrides };
        else if (overrides_status != PW_OK) {
            PS5LOG_LOG("PW_WINE64 DLL overrides refused: %s", game->app.id);
            return 1;
        }
        {
            PwGameEnv graphics_env[PW_GAME_GRAPHICS_ENV_MAX];
            size_t graphics_count = pw_game_graphics_env(game, graphics_env);
            for (size_t i = 0; i < graphics_count; i++)
                extra[config.extra_env_count++] = (PwWineStartEnv){ graphics_env[i].name, graphics_env[i].value };
        }
        if (game->app.graphics == PW_APP_GRAPHICS_ZINK) {
            int provider_status = PW_ERR_NOT_FOUND;
            unsigned copied = 0;
            for (size_t i = 0; i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
                char provider[512];
                int length = snprintf(provider, sizeof(provider), "%s/win/mesa-zink/%s", runtime_roots[i],
                    game->app.architecture == PW_APP_ARCH_PE32 ? "i386-windows" : "x86_64-windows");
                if (length < 0 || (size_t)length >= sizeof(provider)) { provider_status = PW_ERR_LIMIT; break; }
                provider_status = pw_wine_prefix_zink_install(prefix, provider, game->app.architecture, &copied);
                if (provider_status != PW_ERR_NOT_FOUND) break;
            }
            PS5LOG_LOG("PW_WINE64 zink provider=%s copied=%u architecture=%s", pw_result_name(provider_status),
                       copied, game->app.architecture == PW_APP_ARCH_PE32 ? "pe32" : "pe64");
            if (provider_status != PW_OK) return 1;
        }
        /* xinput mode: SDL2 games (Half-Life) look for controllers through
         * raw input first, which Wine on the PS5 has none of, and then skip
         * XInput; this hint makes them read controller 0 through XInput. */
        if (game_input.mode == PW_GAME_INPUT_XINPUT)
            extra[config.extra_env_count++] = (PwWineStartEnv){ "SDL_JOYSTICK_RAWINPUT", "0" };
        /* xinput mode: the game also hosts the controller's HID drivers
         * (patches 0951-0953), so DirectInput, winmm and
         * Windows.Gaming.Input find the DualSense as an Xbox 360 controller. */
        if (game_input.mode == PW_GAME_INPUT_XINPUT) {
            static const char *const drivers[] = { "hidclass.sys", "hidparse.sys", "winebus.sys", "winehid.sys",
                                                   "winexinput.sys" };
            int installed = 0;

            extra[config.extra_env_count++] = (PwWineStartEnv){ "WINE_NTOSKRNL_HOSTED", "1" };
            /* A 32-bit game loads them from syswow64\drivers, which older
             * bottles lack. */
            for (size_t d = 0; game->app.architecture == PW_APP_ARCH_PE32 && d < sizeof(drivers) / sizeof(drivers[0]);
                 d++) {
                int result = -1;
                for (size_t i = 0; result < 0 && i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
                    char source[256];
                    snprintf(source, sizeof(source), "%s/win/wine/lib/wine/i386-windows/%s", runtime_roots[i],
                             drivers[d]);
                    result = pw_wine_prefix_driver_install(prefix, source, drivers[d]);
                }
                installed = installed < 0 || result < 0 ? -1 : installed + result;
            }
            PS5LOG_LOG("PW_WINE64 hid hosted=1 drivers_copied=%d", installed);
        }
        /* [runtime]: opt-in runtime settings for this game. */
        {
            PwGameEnv runtime_env[PW_GAME_RUNTIME_ENV_MAX];
            size_t runtime_count = pw_game_runtime_env(&game->runtime, runtime_env);

            for (size_t i = 0; i < runtime_count; i++)
                extra[config.extra_env_count++] = (PwWineStartEnv){ runtime_env[i].name, runtime_env[i].value };
            PS5LOG_LOG("PW_WINE64 runtime thread_scheduling=%d shared_input=%d", game->runtime.thread_scheduling,
                       game->runtime.shared_input);
        }
        /* [runtime] fast_clock: patch 0900 answers QueryPerformanceCounter
         * from the TSC only when told its frequency; measure it now, and
         * leave the game on Wine's normal counter if the two measurements
         * disagree or look wrong (src/pw_tsc_calibrate.h). */
        if (game->runtime.fast_clock) {
            static char tsc_hz_text[24];
            uint64_t hz = 0;
            PwTscStatus clock_status = pw_tsc_measure(calibration_clock, calibration_tsc, NULL, &hz);

            if (clock_status == PW_TSC_OK) {
                snprintf(tsc_hz_text, sizeof(tsc_hz_text), "%llu", (unsigned long long)hz);
                extra[config.extra_env_count++] = (PwWineStartEnv){ "PW_QPC_TSC_VALIDATED", "1" };
                extra[config.extra_env_count++] = (PwWineStartEnv){ "PW_QPC_TSC_HZ", tsc_hz_text };
            }
            PS5LOG_LOG("PW_WINE64 fast_clock=%s tsc_hz=%llu calibration=%s",
                       clock_status == PW_TSC_OK ? "on" : "off", (unsigned long long)hz,
                       pw_tsc_status_name(clock_status));
        }
        /* [runtime] cpu: a 32-bit game runs on the native WoW64 CPU unless it
         * asks for the translator. The native CPU always brings the Vulkan
         * batching (src/pw_game_profile.h). */
        {
            PwGameEnv cpu_env[PW_GAME_CPU_ENV_MAX];
            size_t cpu_count = pw_game_cpu_env(game, cpu_env);
            int installed = -2;

            for (size_t i = 0; i < cpu_count; i++)
                extra[config.extra_env_count++] = (PwWineStartEnv){ cpu_env[i].name, cpu_env[i].value };
            /* WoW64 loads its CPU from the prefix's system32; when this copy
             * fails, patch 0611 falls back to the prefix's own CPU. */
            for (size_t i = 0; cpu_count && installed < 0 &&
                               i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
                char source[256];
                snprintf(source, sizeof(source), "%s" PW_WINE64_PE_RUNTIME "/wow64native.dll",
                         runtime_roots[i]);
                installed = pw_wine_prefix_cpu_install(prefix, source, "wow64native.dll");
            }
            PS5LOG_LOG("PW_WINE64 cpu=%s prefix_cpu=%d", cpu_count ? "native" : "translator", installed);
        }
        /* [debug] winedebug: this game's channels in place of the title's. */
        if (game->winedebug[0]) {
            extra[0].value = game->winedebug;
            PS5LOG_LOG("PW_WINE64 winedebug=%s", game->winedebug);
        }
        /* [debug] env: diagnostics variables; the parser refuses any the
         * title sets itself, so these only add (src/pw_game_profile.h). */
        {
            PwGameEnv debug_env[PW_GAME_DEBUG_ENV_MAX];
            size_t debug_count = pw_game_debug_env(game, debug_env);
            char names[PW_GAME_DEBUG_ENV_MAX * PW_GAME_DEBUG_ENV_NAME] = "";

            for (size_t i = 0; i < debug_count; i++) {
                extra[config.extra_env_count++] = (PwWineStartEnv){ debug_env[i].name, debug_env[i].value };
                if (i) strcat(names, ",");
                strcat(names, debug_env[i].name);
            }
            if (debug_count) PS5LOG_LOG("PW_WINE64 debug_env=%s", names);
        }
        /* [environment]: the profile's own variables, after everything the
         * title sets, so they win; the parser refuses the ones the title must
         * decide (src/pw_game_profile.h). Each is logged with its value, so a
         * session log shows what the game ran with. */
        {
            PwGameEnv environment[PW_GAME_ENVIRONMENT_MAX];
            size_t environment_count = pw_game_environment_env(game, environment);

            for (size_t i = 0; i < environment_count; i++) {
                extra[config.extra_env_count++] = (PwWineStartEnv){ environment[i].name, environment[i].value };
                PS5LOG_LOG("PW_WINE64 environment %s=%s", environment[i].name, environment[i].value);
            }
        }
        /* opengl_thread set the PS5 OpenGL SDK's glthread; Zink, which
         * replaced it, has none, so it is ignored. */
        if (game->display.opengl_thread)
            PS5LOG_LOG("PW_WINE64 ignored display opengl_thread=%d (no longer supported)",
                       game->display.opengl_thread);
        PS5LOG_LOG("PW_WINE64 profile id=%s prefix=%s desktop=%s scaling=%d view=%s show_fps=%d framegen=%u "
                   "input=%s preset=%s mode=%s mouse=%d dll_overrides=%s", game->app.id, prefix,
                   desktop[0] ? desktop : "default",
                   scaling, view, game->display.show_fps, game->display.framegen,
                   pw_result_name(input_status),
                   game->input.preset[0] ? game->input.preset : "-",
                   game_input.mode == PW_GAME_INPUT_XINPUT ? "xinput" : "keyboard", (int)game_input.mouse,
                   effective_dll_overrides[0] ? effective_dll_overrides : "-");
        /* What Wine gives the game as NumberOfProcessors. */
        PS5LOG_LOG("PW_WINE64 cpus online=%ld", sysconf(_SC_NPROCESSORS_ONLN));
        {
            /* The temp folders the prefix names, which a prefix made on the
             * PC may lack on the console (src/pw_prefix_temp.h). */
            static PwPrefixTemp temp;
            int folders = pw_wine_prefix_temp_create(prefix, &temp);
            PS5LOG_LOG("PW_WINE64 prefix temp folders=%d last=%s", folders,
                       temp.count ? temp.folders[temp.count - 1] : "-");
        }
        if (PW_WINE64_WAIT_WATCHDOG) setenv("WINE_PS5_WAIT_WATCHDOG", "1", 1);
        /* [application] arguments follow the executable in Wine's argv. */
        int words = pw_wine_launch_split(game->app.arguments, argument_words,
                                         sizeof(argument_words), wine_argv + 2,
                                         PW_WINE_LAUNCH_WORDS);
        if (words > 0) config.argc = 2 + words;
        if (words < 0) PS5LOG_LOG("PW_WINE64 arguments refused: %s", game->app.arguments);
        else if (words) PS5LOG_LOG("PW_WINE64 arguments words=%d line=%s", words, game->app.arguments);
    }

    for (size_t i = 0; i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
        struct stat st;
        snprintf(ntdll_dir, sizeof(ntdll_dir), "%s" PW_WINE64_RUNTIME, runtime_roots[i]);
        snprintf(ntdll_path, sizeof(ntdll_path), "%s/ntdll.prx", ntdll_dir);
        if (stat(ntdll_path, &st) == 0) break;
        PS5LOG_LOG("PW_WINE64 runtime not at %s", ntdll_dir);
    }
    PS5LOG_LOG("PW_WINE64 ntdll=%s prefix=%s exe=%s", config.ntdll_path, config.prefix,
               launch.executable);
    {
        static PwFdForward forward_stderr = { -1, "WINESERVER" }, forward_stdout = { -1, "STDOUT" };
        status = capture_fd(2, &forward_stderr);
        if (status != 0) PS5LOG_LOG("PW_WINE64 fd2_capture=failed status=%d", status);
        status = capture_fd(1, &forward_stdout);
        if (status != 0) PS5LOG_LOG("PW_WINE64 fd1_capture=failed status=%d", status);
    }

    status = pw_wine_start_load(&start, &config, &ops);
    PS5LOG_LOG("PW_WINE64 load status=%d stage=%d module=0x%x segments=%u module_start=%d "
               "adopted=%d stats=%d", status, start.stage, (unsigned)start.module,
               start.segment_count, start.module_start_result, start.adopted != NULL,
               start.virtual_stats != NULL);
    install_early_fault_report(&start);
    if (status == PW_OK)
        wine_dlsym_fn = (void *(*)(void *, const char *))(uintptr_t)pw_prx_lookup(start.descriptor, "dlsym");
    atexit(on_exit_report);
    if (status == PW_OK) {
        void (*set_sink)(void (*)(const char *, size_t)) = (void (*)(void (*)(const char *, size_t)))
            (uintptr_t)pw_prx_lookup(start.descriptor, "__wine_ps5_set_output_sink");
        PS5LOG_LOG("PW_WINE64 output_sink=%d", set_sink != NULL);
        if (set_sink) set_sink(wine_output_sink);
        void (*set_present)(PwWinePresentSink, void *) = (void (*)(PwWinePresentSink, void *))
            (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_present_sink");
        post_input = (int (*)(const PwWineInput *))
            (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_post_input");
        /* xinput mode: the DualSense is also the game's XInput controller
         * 0, read by Wine's xinput (patch 0470); bindings still apply. */
        if (game_input.mode == PW_GAME_INPUT_XINPUT) {
            set_pad = (void (*)(const PwWinePad *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_pad");
            rumble = (int (*)(uint32_t *, uint32_t *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_rumble");
        }
        if (game && game->display.menu) {
            set_menu = (void (*)(const struct pw_title_menu *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_menu");
            PS5LOG_LOG("PW_WINE64 menu=%s", set_menu ? "l3+r3" : "unavailable");
        }
        video_status = pw_videoout_ps5_open(&video);
        pad_status = pw_pad_ps5_platform_ops(&pad_ops);
        if (pad_status == PW_OK)
            pad_status = pw_pad_ps5_open(&pad, &pad_ops, pad_open_map,
                                         sizeof(pad_open_map) / sizeof(pad_open_map[0]));
        if (player2 && pad_status == PW_OK)
            pad2_status = pw_pad_ps5_open_other(&pad2, &pad, -1, pad_open_map,
                                                sizeof(pad_open_map) / sizeof(pad_open_map[0]));
        /* A USB keyboard and mouse, the foreground user's, as the pad's. */
        if (post_input) {
            int32_t user = pad_status == PW_OK ? pad.user_id : -1;
            if (user < 0 && pw_pad_ps5_platform_ops(&pad_ops) == PW_OK) {
                (void)pad_ops.user_initialize(NULL);
                (void)pad_ops.foreground_user(&user);
            }
            hid_status = pw_hid_ps5_open(&hid, pw_hid_ps5_system_ops(hid_log), user);
            /* The same user's on-screen keyboard: the system shows its
             * dialog over the game and the title types what comes back. */
            osk_status = pw_osk_ps5_open(&osk_ps5, pw_osk_ps5_system_ops(osk_log), user);
            game_menu.keyboard = osk_status == PW_OK;
        }
        {
            uint8_t *storage = mmap(NULL, 2u * PW_WINE64_MAX_FRAME, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0);
            if (storage != MAP_FAILED &&
                pw_wine_frame_box_init(&frames, storage, PW_WINE64_MAX_FRAME) == 0) {
                frame_shown = storage + PW_WINE64_MAX_FRAME;
                if (set_present) set_present(wine_present, NULL);
            }
        }
        {
            void (*set_release)(PwWineDisplayRelease, void *) = (void (*)(PwWineDisplayRelease, void *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_display_release");
            if (set_release) set_release(wine_release_display, NULL);
        }
        PS5LOG_LOG("PW_WINE64 display present_sink=%d frames=%d post_input=%d xinput=%d video=%s "
                   "pad=%s hid=%s osk=%s", set_present != NULL, frame_shown != NULL, post_input != NULL,
                   set_pad != NULL,
                   pw_result_name(video_status),
                   pw_result_name(pad_status), pw_result_name(hid_status),
                   pw_result_name(osk_status));
        if (player2)
            PS5LOG_LOG("PW_WINE64 pad2 status=%s user=%d", pw_result_name(pad2_status),
                       (int)pad2.user_id);
        {
            void (*set_audio)(PwWineAudioSink, void *) = (void (*)(PwWineAudioSink, void *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_audio_sink");
            int audio_status = set_audio ? pw_audio_ps5_platform_ops(&audio_ops) : PW_ERR_NOT_FOUND;
            /* The audio library sets up its connection with the system's
             * audio service under the process umask; with the private 077 the
             * port does not open (status state, measured after eba69f8). Open
             * it under the console's own umask 0, then restore the private one. */
            const int audio_mask = per_game_mode ? pw_title_ps5_umask(0) : -1;
            if (audio_status == PW_OK) audio_status = pw_audio_ps5_open_port(&audio_ops, &audio_port);
            if (audio_mask >= 0) (void)pw_title_ps5_umask((unsigned)audio_mask);
            if (audio_status == PW_OK) set_audio(wine_audio, NULL);
            PS5LOG_LOG("PW_WINE64 audio sink=%d port=%d status=%s", set_audio != NULL, audio_port,
                       pw_result_name(audio_status));
        }
        /* The game starts in its profile's working directory. A title
         * cannot chdir, so ntdll keeps a logical one (wine/ps5/pw_wine_cwd.h),
         * which Wine reads as the process's current directory. */
        if (game && game->app.working_directory[0]) {
            int (*set_cwd)(const char *) = (int (*)(const char *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_cwd_set");
            char host_dir[PW_WINE_LIBRARY_PATH + PW_APP_ID_CAPACITY + PW_APP_PATH_CAPACITY];
            int mapped = pw_wine_launch_host_dir(prefix, game->app.working_directory, host_dir,
                                                 sizeof(host_dir));
            int cwd_status = set_cwd && mapped == 0 ? set_cwd(host_dir) : -1;
            PS5LOG_LOG("PW_WINE64 cwd=%s host=%s status=%d", game->app.working_directory,
                       mapped == 0 ? host_dir : "-", cwd_status);
        }
        status = pw_wine_start_environment(&start, &config, &ops);
        PS5LOG_LOG("PW_WINE64 environment status=%d", status);
    }
    if (status == PW_OK) {
        status = pw_wine_start_run(&start, &config, &ops);
        PS5LOG_LOG("PW_WINE64 run status=%d", status);
    }
#if PW_WINE64_SCRIPT
    const uint64_t script_start_ns = now_ns();
    script_keys_load(library_root);
    script_input_load(library_root);
    if (script_input.pad_events && !set_pad)
        PS5LOG_LOG("PW_WINE64 script pad events ignored: the profile's [input] mode is not xinput");
#endif
    for (uint64_t tick = 1; status == PW_OK; tick++) {
        uint64_t now = now_ns();
        pw_diagnostics_tick(now);
        PwPresentView view;
        PwWineInput events[2 * PW_GAME_BUTTON_COUNT + 1];
        int presented = 0;

        /* The USB keyboard and mouse; one plugged in later is found by a
         * retry every few seconds. */
        if (post_input && hid.ops.load_module) {
            PwHidPs5Poll poll;
            PwWineInput input;
            /* While the console's keyboard is up the USB keyboard writes
             * into it: its keys and the mouse's motion and clicks are not
             * the game's then, or the line would reach the game twice.
             * Releases still are, so nothing stays held. */
            int dialog = osk_ps5.showing;

            if (hid_status != PW_OK && tick % (5 * PW_WINE64_TICKS_PER_S) == 0)
                hid_status = pw_hid_ps5_retry(&hid);
            pw_hid_ps5_poll(&hid, &poll);
            /* Wine keeps the pointer's position; the title sends motion. */
            if ((poll.dx || poll.dy) && !dialog) {
                input = (PwWineInput){ PW_WINE_INPUT_MOUSE_MOVE, 0, poll.dx, poll.dy, 0 };
                if (post_input(&input) == 0) posted++;
                else refused++;
            }
            if (poll.wheel) {
                input = (PwWineInput){ PW_WINE_INPUT_MOUSE_WHEEL, 0, poll.wheel, 0, 0 };
                if (post_input(&input) == 0) posted++;
                else refused++;
            }
            for (size_t i = 0; i < poll.count; i++) {
                if (dialog && poll.events[i].down) continue;
                input = (PwWineInput){ poll.events[i].kind == PW_HID_EVENT_KEY ? PW_WINE_INPUT_KEY :
                                       PW_WINE_INPUT_MOUSE_BUTTON, poll.events[i].code, 0, 0,
                                       poll.events[i].down };
                if (post_input(&input) == 0) posted++;
                else refused++;
            }
        }
#if PW_WINE64_SCRIPT
        while (post_input && script_key_next < script_key_count &&
               now - script_start_ns >= script_keys[script_key_next].at_ns) {
            PwWineInput press = { PW_WINE_INPUT_KEY, script_keys[script_key_next].vk, 0, 0, 1 };
            PwWineInput release = press;
            release.down = 0;
            int pressed = post_input(&press), released = post_input(&release);
            PS5LOG_LOG("PW_WINE64 script key=%#x status=%d/%d", (unsigned)press.code, pressed, released);
            script_key_next++;
        }
        if (post_input && script_input_next < script_input.count) {
            uint16_t pad_changed = 0; /* a button changes at most once a frame */

            if (!script_input_start_ns && tick % (PW_WINE64_TICKS_PER_S / 2) == 0) script_input_poll_sync(now);
            while (script_input_start_ns && script_input_next < script_input.count &&
                   now - script_input_start_ns >= (uint64_t)script_input.events[script_input_next].at_ms * 1000000u) {
                const PwScriptInputEvent *event = &script_input.events[script_input_next];
                uint16_t held = script_pad.buttons;
                int pad_event = pw_script_pad_apply(&script_pad, event, &pad_changed);

                if (pad_event < 0) break; /* next frame, after the game saw this one */
                script_input_next++;
                if (pad_event) {
                    if (held != script_pad.buttons)
                        PS5LOG_LOG("PW_WINE64 script pad buttons=%#x at_ms=%u", (unsigned)script_pad.buttons,
                                   (unsigned)event->at_ms);
                } else {
                    PwWineInput input = { event->kind == PW_SCRIPT_INPUT_KEY ? PW_WINE_INPUT_KEY :
                                          event->kind == PW_SCRIPT_INPUT_BUTTON ? PW_WINE_INPUT_MOUSE_BUTTON :
                                          PW_WINE_INPUT_MOUSE_MOVE, event->code, event->dx, event->dy, event->down };
                    if (post_input(&input) == 0) posted++;
                    else refused++;
                }
                if (script_input_next == script_input.count || script_input_next % 2000 == 0)
                    PS5LOG_LOG("PW_WINE64 script input replayed=%u of %u", (unsigned)script_input_next,
                               (unsigned)script_input.count);
            }
        }
#endif
        int pad_read = pad_status == PW_OK && post_input && pw_pad_ps5_read(&pad) == PW_OK;
        /* Each time the pad comes and goes (the system overlay, a drop out),
         * what the reads said, so a lost controller shows in the report. */
        if (pad_read && (int)(pad.core.connected | pad.intercepted << 1) != pad_seen) {
            pad_seen = (int)(pad.core.connected | pad.intercepted << 1);
            PS5LOG_LOG("PW_WINE64 pad connected=%u overlay=%u rc=%d intercepted=%llu disconnected=%llu "
                       "errors=%llu generation=%u",
                       pad.core.connected, pad.intercepted, pad.last_read_rc,
                       (unsigned long long)pad.intercepted_samples, (unsigned long long)pad.disconnected_samples,
                       (unsigned long long)pad.read_errors, (unsigned)pad.last_generation);
        }
        /* The menu and the on-screen keyboard take the pad in turn while
         * they are open: the game then sees it idle, and the buttons that
         * closed them stay theirs until they are let go. Closing the game
         * below still reads the real pad. */
        const PwPadPs5 *seen = &pad;
        if (pad_read && (set_menu || osk_status == PW_OK)) {
            uint32_t held = pad.core.previous_buttons, pressed = pad.core.pressed_edges,
                     released = pad.core.released_edges;
            int neutral = 0;

            if (set_menu) {
                struct pw_title_menu shown;
                PwGameMenuView view;

                pw_game_menu_tick(&game_menu, held, pressed, released, &shown, &view);
                set_menu(&shown);
                held = view.held;
                pressed = view.pressed;
                released = view.released;
                neutral = view.neutral;
                if (view.action == PW_GAME_MENU_ACTION_KEYBOARD && osk_status == PW_OK)
                    pw_osk_open(&osk);
            }
            if (osk_status == PW_OK) {
                PwOskView view;

                neutral |= pw_osk_tick(&osk, held, pressed, released, pad.touch_fingers >= 2, &view);
                held = view.held;
                pressed = view.pressed;
                released = view.released;
            }
            game_pad = pad;
            game_pad.core.previous_buttons = held;
            game_pad.core.pressed_edges = pressed;
            game_pad.core.released_edges = released;
            if (neutral) {
                game_pad.left_stick.x = game_pad.left_stick.y = 0x80;
                game_pad.right_stick.x = game_pad.right_stick.y = 0x80;
                game_pad.l2 = game_pad.r2 = 0;
            }
            seen = &game_pad;
        }
        /* XInput controller 0: the DualSense, and in a script build the
         * macro's controller added to it, every frame, so a scripted press
         * stays down until its release. */
        if (set_pad) {
            PwWinePad state;
            int connected = pw_wine_game_pad(pad_read ? seen : NULL, &state), scripted = 0;
#if PW_WINE64_SCRIPT
            scripted = script_input.pad_events != 0;
            connected = pw_wine_script_pad(&state, connected, &script_pad, scripted);
#endif
            if (pad_read || scripted) set_pad(connected ? &state : NULL);
        }
        if (pad_read) {
            size_t count = pw_wine_game_inputs(&game_input, seen->core.pressed_edges,
                                               seen->core.released_edges, events,
                                               sizeof(events) / sizeof(events[0]) - 1);
            if (game_input.mouse != PW_GAME_STICK_NONE) {
                const PwPadPs5Stick *stick = game_input.mouse == PW_GAME_STICK_LEFT ?
                                             &seen->left_stick : &seen->right_stick;
                count += (size_t)pw_wine_pointer_step(&pointer, stick->x, stick->y,
                                                      game_input.mouse_speed,
                                                      PW_WINE64_TICK_US, &events[count]);
            }
            for (size_t i = 0; i < count; i++) {
                if (post_input(&events[i]) == 0) posted++;
                else refused++;
            }
            /* The rumble the game asked for: XInput's left motor is the
             * DualSense's large one, speeds 0..65535 become 0..255. */
            uint32_t left, right;
            if (rumble && rumble(&left, &right) == 1) {
                int vibrate = pw_pad_ps5_vibrate(&pad, (uint8_t)(left >> 8), (uint8_t)(right >> 8));
                if (!vibrations++ || vibrate != PW_OK)
                    PS5LOG_LOG("PW_WINE64 rumble left=%u right=%u status=%s rc=%d trigger_rc=%d",
                               (unsigned)left, (unsigned)right, pw_result_name(vibrate),
                               pad.vibration_rc, pad.trigger_rc);
            }
        }
        /* The console's own keyboard: the two-finger click or the menu's
         * entry asks the system for its dialog, which it draws over the
         * game and drives itself; the accepted line is then typed into the
         * game, each key held for a tick, with Enter after it. */
        if (osk_status == PW_OK && post_input) {
            char typed[PW_OSK_TEXT];
            PwOskKey osk_keys[PW_OSK_KEYS_PER_TICK];
            size_t count;

            if (pw_osk_request(&osk))
                pw_osk_opened(&osk, pw_osk_ps5_show(&osk_ps5,
                                                    game ? game->app.name : NULL) == PW_OK);
            if (osk_ps5.showing) {
                int ended = pw_osk_ps5_poll(&osk_ps5, typed, sizeof(typed));
                if (ended) pw_osk_finished(&osk, ended == 1 ? typed : NULL);
            }
            count = pw_osk_keys(&osk, osk_keys, sizeof(osk_keys) / sizeof(osk_keys[0]));
            for (size_t i = 0; i < count; i++) {
                PwWineInput input = { PW_WINE_INPUT_KEY, osk_keys[i].vk, 0, 0, osk_keys[i].down };
                if (post_input(&input) == 0) posted++;
                else refused++;
            }
        }
        /* The second player's pad: keys only, through its own preset, read
         * whether or not the first pad's read worked. A user who signs in
         * later is found by a retry every few seconds; a pick that shows no
         * controller for that long, or loses it, is closed and the next
         * signed-in user tried. Closing the game and rumble stay the first
         * pad's. */
        if (player2 && post_input) {
            const uint64_t retry = 3 * PW_WINE64_TICKS_PER_S;
            if (pad2_status == PW_OK && pw_pad_ps5_read(&pad2) == PW_OK) {
                if (pad2.core.connected) pad2_seen = tick;
                size_t count2 = pw_wine_game_inputs(&game_input2, pad2.core.pressed_edges,
                                                    pad2.core.released_edges, events,
                                                    sizeof(events) / sizeof(events[0]) - 1);
                for (size_t i = 0; i < count2; i++) {
                    if (post_input(&events[i]) == 0) posted++;
                    else refused++;
                }
            }
            int32_t after = -1;
            if (pad2_status == PW_OK && !pad2.core.connected && tick - pad2_seen >= retry) {
                after = pad2.user_id;
                (void)pw_pad_ps5_close(&pad2);
                pad2_status = PW_ERR_NOT_FOUND;
            }
            if (pad2_status != PW_OK && pad2_status != PW_ERR_UNSUPPORTED &&
                (after >= 0 || tick % retry == 0)) {
                pad2_status = pw_pad_ps5_open_other(&pad2, &pad, after, pad_open_map,
                                                    sizeof(pad_open_map) / sizeof(pad_open_map[0]));
                pad2_seen = tick;
                if (pad2_status == PW_OK && pad2.user_id != after)
                    PS5LOG_LOG("PW_WINE64 pad2 status=%s user=%d", pw_result_name(pad2_status),
                               (int)pad2.user_id);
            }
        }
        if (__atomic_load_n(&display_request, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&display_closed, __ATOMIC_RELAXED)) {
            int closed = video_status == PW_OK ? pw_videoout_ps5_close(&video) : PW_OK;
            video_status = PW_ERR_STATE;
            __atomic_store_n(&display_closed, 1, __ATOMIC_RELEASE);
            PS5LOG_LOG("PW_WINE64 display released to vulkan close=%s shown=%llu",
                       pw_result_name(closed), (unsigned long long)shown);
        }
        if (video_status == PW_OK && frame_shown) {
            PwPresentFrame spinner;
            const PwPresentFrame *next;

            if (pw_wine_frame_box_take(&frames, &shown_sequence, frame_shown, PW_WINE64_MAX_FRAME,
                                       &view) == 1 && loading_admits(&loading, &view)) {
                if (show_scaled(&video, &view, scaling) == PW_OK) {
                    shown++;          /* present waits for the vblank */
                    presented = 1;
                    if (!pw_spinner_frame_blank(view.pixels, view.width, view.height, view.stride))
                        observe_game_start();
                }
            } else if ((next = loading_frame(&loading, now, started, &spinner)) &&
                       pw_videoout_ps5_present_scaled(&video, next, PW_PRESENT_SCALE_FIT, 0) == PW_OK) {
                presented = 1;
            }
        } else if (frame_shown && __atomic_load_n(&display_closed, __ATOMIC_ACQUIRE)) {
            if (!vulkan_frames.resolved) vulkan_frames_resolve(&vulkan_frames, start.descriptor);
            (void)vulkan_frames_show(&vulkan_frames, &shown_sequence, scaling, &loading, now, started);
        }
        if (!presented) usleep(PW_WINE64_TICK_US);

        /* Closing: Options+Create held for PW_WINE64_CLOSE_HOLD_S, or the
         * deadline, asks the game to close with Alt+F4; Wine's exit then
         * restarts the title (on_exit_report). A game that does not close
         * is left. */
        if (pad_status == PW_OK && (pad.core.pressed_edges & PAD_CREATE) &&
            !(pad.core.previous_buttons & PAD_OPTIONS))
            PS5LOG_LOG("PW_WINE64 marker by=player tick=%llu", (unsigned long long)tick);
        if (pad_status == PW_OK && (pad.core.pressed_edges & PW_PAD_PS5_TOUCHPAD))
            PS5LOG_LOG("PW_WINE64 touchpad x=%u y=%u fingers=%u from=%s half=%s", (unsigned)pad.touch_x,
                       (unsigned)pad.touch_y, pad.touch_fingers,
                       pad.touch_from == PW_PAD_PS5_TOUCH_FROM_CLICK ? "click" :
                       pad.touch_from == PW_PAD_PS5_TOUCH_FROM_EARLIER ? "earlier" : "none",
                       pad.touch_left ? "left" : "right");
        combo_ticks = pad_status == PW_OK && (pad.core.previous_buttons & PAD_CLOSE) == PAD_CLOSE ?
                      combo_ticks + 1 : 0;
        if (!close_requested &&
            (combo_ticks >= PW_WINE64_CLOSE_HOLD_S * PW_WINE64_TICKS_PER_S ||
             (PW_WINE64_SECONDS && now - started >= (uint64_t)PW_WINE64_SECONDS * 1000000000u))) {
            static const PwWineInput alt_f4[] = {
                { PW_WINE_INPUT_KEY, 0x12, 0, 0, 1 }, { PW_WINE_INPUT_KEY, 0x73, 0, 0, 1 },
                { PW_WINE_INPUT_KEY, 0x73, 0, 0, 0 }, { PW_WINE_INPUT_KEY, 0x12, 0, 0, 0 },
            };
            close_requested = now;
            if (rumble && pad_status == PW_OK) (void)pw_pad_ps5_vibrate(&pad, 0, 0);
            for (size_t i = 0; post_input && i < sizeof(alt_f4) / sizeof(alt_f4[0]); i++)
                (void)post_input(&alt_f4[i]);
            PS5LOG_LOG("PW_WINE64 close requested by=%s", combo_ticks ? "combo" : "deadline");
        }
        if (close_requested && now - close_requested >= (uint64_t)PW_WINE64_CLOSE_WAIT_S * 1000000000u) {
            PS5LOG_LOG("PW_WINE64 close timeout: leaving the game");
            if (per_game_mode) (void)pw_title_session_failed(&title_session, 0, 0);
            restart_title(NULL, launch.cycle + 1u, "close-timeout");
            break;
        }
        if (tick % 60 == 0) {
            uint64_t v[16] = { 0 };

            PS5LOG_LOG("PW_WINE64 alive tick=%llu sink_calls=%lu frames_put=%llu shown=%llu "
                       "rejected=%llu last=%ux%u inputs=%llu refused=%llu vk_shown=%llu vk_busy=%llu "
                       "cpu_ms=%llu audio=%lu audible=%lu",
                       (unsigned long long)tick, sink_calls, (unsigned long long)frames.sequence,
                       (unsigned long long)shown, (unsigned long long)frames.rejected,
                       frames.width, frames.height, (unsigned long long)posted,
                       (unsigned long long)refused, (unsigned long long)vulkan_frames.shown,
                       (unsigned long long)vulkan_frames.busy, cpu_ms(), audio_grains, audio_audible);
            if (start.virtual_stats && tick % 300 == 0) {
                static uint64_t faults_logged;

                start.virtual_stats(v, 16);
                PS5LOG_LOG("PW_WINE64 mmap=%llu munmap=%llu mprotect=%llu skipped=%llu "
                           "faults=%llu images=%llu",
                           (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
                           (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6]);
                /* Where the faults are, when there were more since the last
                 * line: the pages counted most (Wine patch 0545). kind 0
                 * read, 1 write, 8 execute; result 1 resolved, 2 access
                 * violation, 3 other. */
                if (start.fault_top && v[5] > faults_logged) {
                    uint64_t top[4 * 4];
                    unsigned n = start.fault_top(top, 4 * 4);

                    for (unsigned i = 0; i < n; i++)
                        PS5LOG_LOG("PW_WINE64 fault_top rank=%u count=%llu page=%#llx pc=%#llx kind=%u "
                                   "vprot=%#x host_vprot=%#x result=%u",
                                   i + 1, (unsigned long long)top[i * 4], (unsigned long long)top[i * 4 + 1],
                                   (unsigned long long)top[i * 4 + 2], (unsigned)(top[i * 4 + 3] >> 24),
                                   (unsigned)(top[i * 4 + 3] >> 16) & 0xffu,
                                   (unsigned)(top[i * 4 + 3] >> 8) & 0xffu, (unsigned)top[i * 4 + 3] & 0xffu);
                    faults_logged = v[5];
                }
            }
            if (tick % 30 == 0) {
                static int holders_logged;
                size_t flexible = 0;

                (void)sceKernelAvailableFlexibleMemorySize(&flexible);
                if ((holders_logged == 0 && flexible < ((size_t)64 << 20)) ||
                    (holders_logged == 1 && flexible < ((size_t)4 << 20))) {
                    log_flexible_holders(flexible);
                    holders_logged = flexible < ((size_t)4 << 20) ? 2 : 1;
                }
            }
            if (tick % 300 == 0) {
                uint64_t m[10] = { 0 };
                size_t flexible = 0;

                if (start.memory_stats) start.memory_stats(m, 10);
                (void)sceKernelAvailableFlexibleMemorySize(&flexible);
                /* dmem_low: the part below 4 GiB, where an i386 guest's own
                 * memory lives (2 GiB, or 4 GiB when large-address-aware). */
                PS5LOG_LOG("PW_WINE64 memory flexible_free=%zuK dmem=%lluK dmem_peak=%lluK runs=%llu "
                           "dmem_failures=%llu heap=%lluK heap_peak=%lluK dmem_low=%lluK dmem_low_peak=%lluK "
                           "heap_dmem=%lluK heap_spill=%lluK",
                           flexible >> 10, (unsigned long long)(m[0] >> 10),
                           (unsigned long long)(m[1] >> 10), (unsigned long long)m[2],
                           (unsigned long long)m[3], (unsigned long long)(m[4] >> 10),
                           (unsigned long long)(m[5] >> 10), (unsigned long long)(m[6] >> 10),
                           (unsigned long long)(m[7] >> 10), (unsigned long long)(m[8] >> 10),
                           (unsigned long long)(m[9] >> 10));
            }
        }
    }
    PS5LOG_LOG("PW_WINE64 done status=%d stage=%d", status, start.stage);
    if (status != PW_OK) {
        if (per_game_mode) (void)pw_title_session_failed(&title_session, 1, status);
        restart_title(NULL, launch.cycle + 1u, "start-failed");
    }
    if (per_game_mode) pw_title_session_close(&title_session);
    pw_diagnostics_close(status == PW_OK ? "wine64-restart-failed" : "wine64-start-failed");
    _exit(1);
}
