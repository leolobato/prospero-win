/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The local process a title starts for [helper] (bdj.elf): one Windows
 * program beside the game, on the title's Wine server.
 *
 * It is started with sceSystemServiceAddLocalProcess under the name bdj.elf,
 * which makes the system skip its module preload (libSceAgcDriver fails in a
 * second process), so it imports libkernel, the system C library and the
 * system module loader only, and has its own _start. Wine's unix libraries
 * import libSceNet, which the preload would have loaded: this process loads
 * it by its module ID. There is no GPU here, and Wine is told it is headless
 * (patch 1014). It follows the title through the mailbox
 * (pw_helper_mailbox.h): hello, the credential clone for the /data grant,
 * then Wine with the server socket the title passes (Wine patch 1011).
 *
 * A crash here ends the whole app, so every failure leaves through _exit.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "pw_helper_mailbox.h"
#include "pw_wine_start.h"

int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                                 const void *option, int *result);
int sceKernelGetModuleInfo(int32_t handle, void *info);
void _init_env(void *parameters);
int setenv(const char *name, const char *value, int overwrite);
int setreuid(uid_t ruid, uid_t euid);
int sceSysmoduleLoadModuleInternal(uint32_t id);

/* libSceNet.sprx's system module ID (internal, as the PS4 SDK numbers it). */
enum { PW_SYSMODULE_NET = 0x8000001Cu };

/* As the title shell reads it (native/wine64_main.c). */
struct pw_app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c + 0x400];
};
int sceKernelGetAppInfo(pid_t pid, struct pw_app_info *info);

enum { POLL_US = 2000, HELLO_WAIT_MS = 120000, SOCKET_WAIT_MS = 10000 };

static struct pw_helper_mailbox *mailbox;
static int log_fd = -1;
static uid_t app_uid;  /* this process's user before the grant */

/* Until the grant there is no file this process may write that the title can
 * read, so its first lines go into the mailbox. */
static void say(const char *text)
{
    const size_t length = strlen(text);

    if (log_fd >= 0) {
        (void)!write(log_fd, text, length);
        (void)!write(log_fd, "\n", 1);
        return;
    }
    if (!mailbox) return;
    uint32_t used = mailbox->log_used;
    if (used + length + 1 > PW_HELPER_LOG) return;
    memcpy(mailbox->log + used, text, length);
    mailbox->log[used + length] = '\n';
    mailbox->log_used = used + (uint32_t)length + 1;
}

static void say_number(const char *text, long value)
{
    char line[160];

    snprintf(line, sizeof(line), "%s=%ld", text, value);
    say(line);
}

static void leave(int error, const char *why)
{
    say(why);
    if (mailbox) {
        mailbox->child_error = error;
        mailbox->answer = PW_HELPER_ANSWER_FAILED;
    }
    _exit(error ? 1 : 0);
}

static int set_env(const char *name, const char *value) { return setenv(name, value, 1); }

struct thread_start { void (*entry)(void *); void *arg; };
static struct thread_start thread_start;

static void *thread_trampoline(void *unused)
{
    (void)unused;
    thread_start.entry(thread_start.arg);
    return NULL;
}

static int start_thread(void (*entry)(void *), void *arg, size_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_t thread;
    int status;

    thread_start.entry = entry;
    thread_start.arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    status = pthread_create(&thread, &attr, thread_trampoline, NULL);
    pthread_attr_destroy(&attr);
    return status;
}

static void output_sink(const char *text, size_t size)
{
    if (log_fd >= 0) (void)!write(log_fd, text, size);
}

static int receive_fd(int sock)
{
    char byte = 0;
    struct iovec io = { &byte, 1 };
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr message;
    struct cmsghdr *header;
    int fd = -1;

    memset(&message, 0, sizeof(message));
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(sock, &message, 0) != 1) return -1;
    header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) return -1;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    return fd;
}

/* Wait for the title's word to become one of two values; 0 on a timeout. */
static uint32_t wait_step(uint32_t first, uint32_t second, unsigned wait_ms)
{
    for (unsigned waited = 0; waited < wait_ms * 1000u; waited += POLL_US) {
        const uint32_t step = mailbox->step;
        if (step == first || step == second || step == PW_HELPER_STEP_STOP) return step;
        usleep(POLL_US);
    }
    return 0;
}

static char terminated(char *text, size_t size)
{
    text[size - 1] = '\0';
    return text[0];
}

static PwWineStart start;

/* The system's loader refuses a program whose init and fini entries, or its
 * init and fini arrays, lie outside its segments (ENOEXEC, "segment not
 * found"). The linker leaves them at 0 for a program that has none, and the
 * title's layout starts the code at 0x100000000, so 0 is outside. The title
 * gets them from its start code (app_crt); this program defines its own, and
 * referencing the arrays keeps them in the image. */
typedef void (*pw_helper_initializer)(void);
extern pw_helper_initializer __preinit_array_start[] __attribute__((weak, visibility("hidden")));
extern pw_helper_initializer __preinit_array_end[] __attribute__((weak, visibility("hidden")));
extern pw_helper_initializer __init_array_start[] __attribute__((weak, visibility("hidden")));
extern pw_helper_initializer __init_array_end[] __attribute__((weak, visibility("hidden")));
extern pw_helper_initializer __fini_array_start[] __attribute__((weak, visibility("hidden")));
extern pw_helper_initializer __fini_array_end[] __attribute__((weak, visibility("hidden")));

__attribute__((used, visibility("default"))) void _init(void)
{
    for (pw_helper_initializer *entry = __preinit_array_start; entry != __preinit_array_end; ++entry) (*entry)();
    for (pw_helper_initializer *entry = __init_array_start; entry != __init_array_end; ++entry) (*entry)();
}

__attribute__((used, visibility("default"))) void _fini(void)
{
    for (pw_helper_initializer *entry = __fini_array_end; entry != __fini_array_start;) (*--entry)();
}

__attribute__((visibility("default"))) void _start(void *parameters, void *teardown)
{
    static char name[PW_HELPER_NAME], fd_text[16], ntdll_path[PW_HELPER_PATH + 16];
    static PwWineStartEnv extra[] = {
        { "WINEDEBUG", NULL },
        { "WINEARCH", "wow64" },
        { "HOME", NULL },
        { "USER", "prospero" },
        { "WINE_PS5_SERVER_FD", fd_text },
        /* No GPU modules here: win32u loads no Vulkan and leaves the display to the title (Wine patch 1014). */
        { "WINE_PS5_HEADLESS", "1" },
    };
    static const char *wine_argv[] = { "wine", NULL, NULL };
    static const PwWineStartOps ops = { sceKernelLoadStartModule, sceKernelGetModuleInfo, set_env, start_thread };
    static PwWineStartConfig config;
    struct pw_app_info info;
    struct sockaddr_un address;
    void (*set_sink)(void (*)(const char *, size_t));
    uint32_t step;
    int shm, sock, server, status;

    (void)teardown;
    _init_env(parameters);

    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(getpid(), &info) != 0) _exit(2);
    info.title_id[sizeof(info.title_id) - 1] = '\0';
    if (pw_helper_mailbox_name(name, sizeof(name), info.title_id) != 0) _exit(2);
    if ((shm = shm_open(name, O_RDWR, 0)) < 0) _exit(2);
    mailbox = mmap(NULL, sizeof(*mailbox), PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    if (mailbox == MAP_FAILED) _exit(2);
    close(shm);
    if (mailbox->magic != PW_HELPER_MAILBOX_MAGIC || mailbox->size != sizeof(*mailbox)) _exit(2);

    /* Before the grant: the loader refuses libSceNet once this process has the title's /data rights. Every Wine
     * unix library imports it (ws2_32 resolves names through it), and an import of a module that is not loaded
     * stays empty. Without it Wine still starts, and the program finds no network. */
    {
        const int net = sceSysmoduleLoadModuleInternal(PW_SYSMODULE_NET);
        if (net != 0) say_number("libSceNet not loaded", net);
    }
    mailbox->child_pid = getpid();
    app_uid = geteuid();
    mailbox->answer = PW_HELPER_ANSWER_HELLO;

    /* The grant: the title asks its helper for this process, and this process
     * gives itself a credential of its own when the helper is ready for it. */
    step = wait_step(PW_HELPER_STEP_CLONE, PW_HELPER_STEP_CONNECT, HELLO_WAIT_MS);
    if (step == PW_HELPER_STEP_CLONE) {
        const int cloned = seteuid(geteuid());
        if (cloned != 0) say_number("seteuid errno", errno);
        mailbox->answer = cloned == 0 ? PW_HELPER_ANSWER_CLONED : PW_HELPER_ANSWER_CLONE_FAILED;
        step = wait_step(PW_HELPER_STEP_CONNECT, PW_HELPER_STEP_CONNECT, HELLO_WAIT_MS);
    }
    if (step != PW_HELPER_STEP_CONNECT) leave(0, step ? "stopped by the title" : "no word from the title");
    /* The grant leaves this process on UID 0, as it does the title (native/pw_data_mount.c): return to the app
     * user, the real UID first while still root. Wine refuses a prefix its real UID doesn't own, and the title's
     * socket belongs to the app user. */
    if ((getuid() != app_uid && setreuid(app_uid, (uid_t)-1) != 0) ||
        (geteuid() != app_uid && seteuid(app_uid) != 0) || getuid() != app_uid || geteuid() != app_uid)
        leave(errno ? errno : EACCES, "cannot return to the app user");

    if (!terminated(mailbox->socket_path, sizeof(mailbox->socket_path)) ||
        !terminated(mailbox->runtime_dir, sizeof(mailbox->runtime_dir)) ||
        !terminated(mailbox->prefix, sizeof(mailbox->prefix)) ||
        !terminated(mailbox->program, sizeof(mailbox->program)))
        leave(EINVAL, "the title left a path empty");
    (void)terminated(mailbox->winedebug, sizeof(mailbox->winedebug));
    if (terminated(mailbox->log_path, sizeof(mailbox->log_path))) {
        /* The console protects descriptors 0-3 of a local process, so Wine's
         * messages are redirected at the stdio level and through its sink. */
        /* Emptied once, then every writer appends: a stream opened for "w" writes from its own offset 0, over the
         * lines written before it. */
        const int emptied = open(mailbox->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (emptied >= 0) close(emptied);
        FILE *err = freopen(mailbox->log_path, "a", stderr);
        FILE *out = freopen(mailbox->log_path, "a", stdout);
        if (err) setvbuf(err, NULL, _IONBF, 0);
        if (out) setvbuf(out, NULL, _IONBF, 0);
        log_fd = open(mailbox->log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    }
    say_number("helper pid", getpid());
    say_number("uid", (long)getuid());

    /* The same tree as the title now: its socket is reachable by path. */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    address.sun_len = sizeof(address);
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", mailbox->socket_path);
    status = -1;
    for (unsigned waited = 0; sock >= 0 && waited < SOCKET_WAIT_MS * 1000u; waited += 100000) {
        if ((status = connect(sock, (struct sockaddr *)&address, sizeof(address))) == 0) break;
        usleep(100000);
    }
    if (status != 0) {
        const int error = errno ? errno : EIO;
        /* What this process sees of the title's tree: 0, or the errno of stat. */
        static const char *const views[] = { "/mnt", "/mnt/sandbox", "/data/playden", "/app0" };
        char line[200];
        struct stat status_of;
        int length = snprintf(line, sizeof(line), "view socket=%d", stat(mailbox->socket_path, &status_of) == 0 ? 0 : errno);
        for (size_t i = 0; i < sizeof(views) / sizeof(views[0]) && length > 0 && (size_t)length < sizeof(line); i++)
            length += snprintf(line + length, sizeof(line) - (size_t)length, " %s=%d", views[i],
                               stat(views[i], &status_of) == 0 ? 0 : errno);
        say(line);
        leave(error, "cannot connect to the title's socket");
    }
    if ((server = receive_fd(sock)) < 0) leave(errno ? errno : EIO, "no server socket from the title");
    /* The socket stays open: the title sees this process end when it closes. */
    snprintf(fd_text, sizeof(fd_text), "%d", server);
    say_number("server socket", server);

    snprintf(ntdll_path, sizeof(ntdll_path), "%s/ntdll.prx", mailbox->runtime_dir);
    extra[0].value = mailbox->winedebug[0] ? mailbox->winedebug : "fixme-all,err+all";
    extra[2].value = mailbox->prefix;
    wine_argv[1] = mailbox->program;
    memset(&config, 0, sizeof(config));
    config.ntdll_path = ntdll_path;
    config.ntdll_dir = mailbox->runtime_dir;
    config.prefix = mailbox->prefix;
    config.extra_env = extra;
    config.extra_env_count = sizeof(extra) / sizeof(extra[0]);
    config.argc = 2;
    config.argv = wine_argv;
    config.stack_bytes = 16u << 20;

    status = pw_wine_start_load(&start, &config, &ops);
    if (status != 0) {
        say_number("load stage", start.stage);
        say_number("load status", start.status);
        leave(EIO, "cannot load Wine");
    }
    set_sink = (void (*)(void (*)(const char *, size_t)))(uintptr_t)
        pw_prx_lookup(start.descriptor, "__wine_ps5_set_output_sink");
    if (set_sink) set_sink(output_sink);
    if ((status = pw_wine_start_environment(&start, &config, &ops)) != 0 ||
        (status = pw_wine_start_run(&start, &config, &ops)) != 0) {
        say_number("start stage", start.stage);
        leave(EIO, "cannot start Wine");
    }
    say("Wine started");
    mailbox->answer = PW_HELPER_ANSWER_STARTED;

    /* Wine's threads carry on; the program's ExitProcess ends this process
     * through exit(). The title's end ends it too. */
    for (;;) sleep(3600);
}
