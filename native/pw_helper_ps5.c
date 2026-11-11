/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_helper_ps5.h"
#include "pw_helper_mailbox.h"
#include "../src/pw_helper_notify.h"
#include "pw_lapy_elevation.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int sceSystemServiceAddLocalProcess(int app_id, const char *path, char *const *argv, const void *param);

/* As the title shell reads it (native/wine64_main.c). */
struct pw_helper_app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c + 0x400];
};
int sceKernelGetAppInfo(pid_t pid, struct pw_helper_app_info *info);

/* The request block of sceSystemServiceAddLocalProcess, with the values the
 * system library uses by default (read from the library: size 0x48, then 1,
 * -1, -1, -1 in the request it builds). The meaning of the fields is unknown. */
struct local_process_param {
    uint32_t size, field4, field8;
    uint64_t field_c;
    uint32_t field14;
    uint64_t field18;
    uint32_t field20;
    char name[0x20];
    uint32_t field44;
} __attribute__((packed));
_Static_assert(sizeof(struct local_process_param) == 0x48, "local process request size");

enum { POLL_US = 2000, HELLO_WAIT_MS = 15000, CLONE_WAIT_MS = 5000, ACCEPT_WAIT_MS = 15000, START_WAIT_MS = 30000 };

typedef struct Start {
    const PwHelperConfig *config;
    struct pw_helper_mailbox *mailbox;
    uint32_t log_shown;
} Start;

static void say(const PwHelperConfig *config, const char *format, ...)
{
    char line[320];
    va_list args;

    if (!config->log) return;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    config->log(line);
}

/* The child's own lines, as they arrive. */
static void show_child_log(Start *start)
{
    struct pw_helper_mailbox *mailbox = start->mailbox;
    uint32_t used = mailbox->log_used;

    if (used > PW_HELPER_LOG) used = PW_HELPER_LOG;
    while (start->log_shown < used) {
        const char *line = mailbox->log + start->log_shown;
        const char *end = memchr(line, '\n', used - start->log_shown);
        if (!end) break;
        say(start->config, "helper child: %.*s", (int)(end - line), line);
        start->log_shown = (uint32_t)(end - mailbox->log) + 1;
    }
}

static void notify(const PwHelperConfig *config, const char *text)
{
    if (config->notify) config->notify(text);
}

/* "<name>: <step>", when the start is traced. */
static void step(const PwHelperConfig *config, int traced, const char *name, const char *what)
{
    char text[PW_HELPER_NOTIFY_TEXT];

    if (!traced) return;
    snprintf(text, sizeof(text), "%s: %s", name, what);
    notify(config, text);
}

/* Shows the whole lines the program added to the notify file since *shown. */
static void show_added_lines(const PwHelperConfig *config, const char *path, size_t *shown)
{
    static char buffer[4096];
    char line[PW_HELPER_NOTIFY_TEXT];
    size_t at = 0;
    ssize_t got;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);

    if (fd < 0) return;
    got = pread(fd, buffer, sizeof(buffer), (off_t)*shown);
    close(fd);
    if (got <= 0) return;
    while (pw_helper_next_line(buffer, (size_t)got, &at, line, sizeof(line))) notify(config, line);
    *shown += at;
}

static int wait_answer(Start *start, uint32_t first, uint32_t second, unsigned wait_ms)
{
    for (unsigned waited = 0; waited < wait_ms * 1000u; waited += POLL_US) {
        const uint32_t answer = start->mailbox->answer;
        show_child_log(start);
        if (answer == first || answer == second) return (int)answer;
        if (answer == PW_HELPER_ANSWER_FAILED) return PW_HELPER_ANSWER_FAILED;
        usleep(POLL_US);
    }
    return 0;
}

/* The helper's cooperative point, relayed to the child. */
static int clone_in_child(void *context)
{
    Start *start = context;

    start->mailbox->step = PW_HELPER_STEP_CLONE;
    return wait_answer(start, PW_HELPER_ANSWER_CLONED, PW_HELPER_ANSWER_CLONE_FAILED, CLONE_WAIT_MS) ==
           PW_HELPER_ANSWER_CLONED ? 0 : -1;
}

static int send_fd(int sock, int fd)
{
    char byte = 'S';
    struct iovec io = { &byte, 1 };
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr message;
    struct cmsghdr *header;

    memset(control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(sock, &message, 0) == 1 ? 0 : -1;
}

/* A listening Unix socket in a folder that takes socket files: /data and
 * /dev refuse them (errno 45). */
static int listen_somewhere(const PwHelperConfig *config, char *path, size_t size)
{
    static const char *const folders[] = { "/mnt", "/tmp", "/system_tmp", "/var/run" };
    struct sockaddr_un address;

    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        snprintf(path, size, "%s/pw-helper-%s.sock", folders[i], config->title_id);
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        address.sun_len = sizeof(address);
        snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
        unlink(path);
        if (bind(sock, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(sock, 1) == 0) {
            chmod(path, 0600);
            return sock;
        }
        say(config, "helper: no socket in %s errno=%d", folders[i], errno);
        close(sock);
    }
    path[0] = '\0';
    return -1;
}

static int copy_text(char *to, size_t size, const char *from)
{
    const size_t length = from ? strlen(from) : 0;

    if (length >= size) return -1;
    memcpy(to, from ? from : "", length + 1);
    return 0;
}

int pw_helper_alive(const PwHelper *helper)
{
    struct pollfd watch;

    if (!helper || helper->pid <= 0) return 0;
    if (helper->channel < 0) return kill(helper->pid, 0) == 0;
    watch.fd = helper->channel;
    watch.events = POLLIN;
    watch.revents = 0;
    return poll(&watch, 1, 0) <= 0 || !(watch.revents & (POLLHUP | POLLERR | POLLIN));
}

int pw_helper_start(const PwHelperConfig *config, PwHelper *helper)
{
    static char name[PW_HELPER_NAME], socket_path[PW_HELPER_SOCKET_PATH];
    struct pw_helper_app_info info;
    struct local_process_param param;
    struct pw_lapy_elevation_diagnostics diagnostics;
    struct stat st;
    Start start = { config, NULL, 0 };
    char arg0[64];
    static char notify_path[512], display[PW_HELPER_NOTIFY_NAME], text[PW_HELPER_NOTIFY_TEXT];
    int shm = -1, listener = -1, channel = -1, server = -1, status, answer, result = -1, traced = 0, started = 0;
    size_t shown = 0;

    if (!config || !helper || !config->title_id || !config->child_path || !config->runtime_dir ||
        !config->prefix || !config->program || !config->server_connect)
        return -1;
    helper->pid = 0;
    helper->channel = -1;
    helper->ready = 0;

    if (pw_helper_mailbox_name(name, sizeof(name), config->title_id) != 0) return -1;
    shm_unlink(name);
    if ((shm = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0666)) < 0) {
        say(config, "helper: no mailbox errno=%d", errno);
        return -1;
    }
    /* The child opens it with the title's first credentials, whatever umask the title set since. */
    fchmod(shm, 0666);
    if (ftruncate(shm, sizeof(*start.mailbox)) != 0 ||
        (start.mailbox = mmap(NULL, sizeof(*start.mailbox), PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0)) == MAP_FAILED) {
        say(config, "helper: cannot map the mailbox errno=%d", errno);
        start.mailbox = NULL;
        goto done;
    }
    memset(start.mailbox, 0, sizeof(*start.mailbox));
    start.mailbox->size = sizeof(*start.mailbox);
    if (copy_text(start.mailbox->runtime_dir, sizeof(start.mailbox->runtime_dir), config->runtime_dir) != 0 ||
        copy_text(start.mailbox->prefix, sizeof(start.mailbox->prefix), config->prefix) != 0 ||
        copy_text(start.mailbox->program, sizeof(start.mailbox->program), config->program) != 0 ||
        copy_text(start.mailbox->winedebug, sizeof(start.mailbox->winedebug), config->winedebug) != 0 ||
        copy_text(start.mailbox->log_path, sizeof(start.mailbox->log_path), config->log_path) != 0) {
        say(config, "helper: a path is too long");
        goto done;
    }
    start.mailbox->magic = PW_HELPER_MAILBOX_MAGIC;
    if (config->ready_path) unlink(config->ready_path);
    pw_helper_display_name(config->program, display, sizeof(display));
    /* A notify file beside the ready file asks for the start's steps: emptied, then the program adds to it. */
    if (config->notify && config->ready_path &&
        snprintf(notify_path, sizeof(notify_path), "%s.notify", config->ready_path) < (int)sizeof(notify_path)) {
        const int fd = open(notify_path, O_WRONLY | O_TRUNC | O_NOFOLLOW);

        if (fd >= 0) {
            close(fd);
            traced = 1;
        }
    }

    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(getpid(), &info) != 0) {
        say(config, "helper: no app info");
        goto done;
    }
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.field4 = 0xffffffffu;
    param.field8 = 1;
    param.field_c = 0xffffffffu;
    snprintf(arg0, sizeof(arg0), "%s", config->child_path);
    {
        char *const argv[] = { arg0, NULL };
        status = sceSystemServiceAddLocalProcess((int)info.app_id, arg0, argv, &param);
    }
    say(config, "helper: local process %s status=0x%x", config->child_path, (unsigned)status);
    started = 1;
    if (status < 0) goto done;
    step(config, traced, display, "helper started");

    if (wait_answer(&start, PW_HELPER_ANSWER_HELLO, PW_HELPER_ANSWER_HELLO, HELLO_WAIT_MS) != PW_HELPER_ANSWER_HELLO) {
        say(config, "helper: the child did not start");
        goto done;
    }
    helper->pid = start.mailbox->child_pid;
    say(config, "helper: child pid=%d", (int)helper->pid);

    if (config->grant_helper) {
        memset(&diagnostics, 0, sizeof(diagnostics));
        status = pw_lapy_elevation_request_for(helper->pid, config->grant_helper, clone_in_child, &start, &diagnostics);
        say(config, "helper: grant status=%d stage=%s errno=%d helper_status=%d", status,
            diagnostics.stage ? diagnostics.stage : "-", status ? errno : 0,
            diagnostics.helper_status_valid ? (int)diagnostics.helper_status : -1);
        if (status != 0) goto stop;
    }

    if ((listener = listen_somewhere(config, socket_path, sizeof(socket_path))) < 0) goto stop;
    if ((server = config->server_connect(config->context)) < 0) {
        say(config, "helper: no second connection to the Wine server");
        goto stop;
    }
    copy_text(start.mailbox->socket_path, sizeof(start.mailbox->socket_path), socket_path);
    start.mailbox->step = PW_HELPER_STEP_CONNECT;
    {
        struct pollfd watch = { listener, POLLIN, 0 };
        if (poll(&watch, 1, ACCEPT_WAIT_MS) <= 0 || (channel = accept(listener, NULL, NULL)) < 0) {
            show_child_log(&start);
            say(config, "helper: the child did not connect errno=%d", errno);
            goto stop;
        }
    }
    status = send_fd(channel, server);
    /* The child holds the connection now: closing this copy lets the server see the child end. */
    close(server);
    server = -1;
    if (status != 0) {
        say(config, "helper: cannot pass the server socket errno=%d", errno);
        goto stop;
    }

    answer = wait_answer(&start, PW_HELPER_ANSWER_STARTED, PW_HELPER_ANSWER_STARTED, START_WAIT_MS);
    if (answer != PW_HELPER_ANSWER_STARTED) {
        say(config, "helper: Wine did not start in the child error=%d", (int)start.mailbox->child_error);
        goto done;   /* the child left by itself, or will not answer */
    }
    helper->channel = channel;
    channel = -1;
    result = 0;
    step(config, traced, display, "Wine running");

    /* The program's own start: it writes ready_path when the game may run. */
    if (config->ready_path) {
        const unsigned limit = (config->wait_seconds ? config->wait_seconds : 30) * 10;
        unsigned waited = 0;
        while (waited < limit && stat(config->ready_path, &st) != 0 && pw_helper_alive(helper)) {
            usleep(100000);
            waited++;
            if (traced) show_added_lines(config, notify_path, &shown);
        }
        helper->ready = stat(config->ready_path, &st) == 0;
        if (traced) show_added_lines(config, notify_path, &shown);
        say(config, "helper: program %s after %u.%u s%s", helper->ready ? "ready" : "not ready", waited / 10, waited % 10,
            pw_helper_alive(helper) ? "" : ", and it has ended");
        if (!helper->ready) {
            char reason[96];

            snprintf(reason, sizeof(reason), pw_helper_alive(helper) ? "no answer in %u s" : "it stopped",
                     waited / 10);
            pw_helper_failure_text(display, reason, text, sizeof(text));
            notify(config, text);
        } else {
            /* Ready, or given up: "failed <reason>" says why. */
            char ready_text[256], reason[PW_HELPER_NOTIFY_TEXT];
            const int fd = open(config->ready_path, O_RDONLY | O_NOFOLLOW);
            const ssize_t got = fd >= 0 ? read(fd, ready_text, sizeof(ready_text)) : -1;

            if (fd >= 0) close(fd);
            if (got > 0 && pw_helper_ready_failed(ready_text, (size_t)got, reason, sizeof(reason))) {
                pw_helper_failure_text(display, reason, text, sizeof(text));
                notify(config, text);
            }
        }
    } else {
        helper->ready = 1;
    }
    goto done;

stop:
    start.mailbox->step = PW_HELPER_STEP_STOP;
    /* It leaves by itself; a child that hangs is ended, since nothing else will. */
    for (int i = 0; i < 20 && helper->pid > 0 && kill(helper->pid, 0) == 0; i++) usleep(50000);
    if (helper->pid > 0 && kill(helper->pid, 0) == 0) kill(helper->pid, SIGKILL);
    helper->pid = 0;
done:
    if (start.mailbox) show_child_log(&start);
    if (server >= 0) close(server);
    if (channel >= 0) close(channel);
    if (listener >= 0) {
        close(listener);
        if (socket_path[0]) unlink(socket_path);
    }
    if (start.mailbox) munmap(start.mailbox, sizeof(*start.mailbox));
    if (shm >= 0) {
        close(shm);
        shm_unlink(name);
    }
    if (result != 0 && helper->pid > 0 && kill(helper->pid, 0) != 0) helper->pid = 0;
    /* It never ran its program: the player hears so too. */
    if (result != 0 && started) {
        pw_helper_failure_text(display, "its process did not start", text, sizeof(text));
        notify(config, text);
    }
    return result;
}
