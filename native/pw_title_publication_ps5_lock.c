/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_title_publication_ps5_lock.h"
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

extern int sceKernelDlsym(int module, const char *name, void **symbol);
static _Atomic(uintptr_t) lock_gateway;
struct LockResult { long value; long failed; };

/* Same SDK CRT/libkernel gateway qualified by Playden's native lock probe.
 * Enter libkernel text; capture carry before changing stack flags. */
__attribute__((naked, noinline)) static struct LockResult lock_syscall(
    void *entry, long number, long first, long second)
{
    __asm__ volatile(
        "movq %rdi, %r11\n"
        "movq %rsi, %rax\n"
        "movq %rdx, %rdi\n"
        "movq %rcx, %rsi\n"
        "subq $8, %rsp\n"
        "call *%r11\n"
        "setc %dl\n"
        "movzbl %dl, %edx\n"
        "addq $8, %rsp\n"
        "ret\n");
}

int pw_title_publication_ps5_lock(void *context, int descriptor)
{
    uintptr_t gateway = atomic_load_explicit(&lock_gateway, memory_order_acquire);
    struct LockResult result;
    (void)context;
    if (!gateway) {
        void *entry = NULL;
        if (sceKernelDlsym(0x2001, "getpid", &entry) != 0 || !entry) {
            errno = ENOSYS;
            return -1;
        }
        gateway = (uintptr_t)entry + 0xa;
        result = lock_syscall((void *)gateway, SYS_getpid, 0, 0);
        if (result.failed || result.value != getpid()) {
            errno = ENOSYS;
            return -1;
        }
        atomic_store_explicit(&lock_gateway, gateway, memory_order_release);
    }
    result = lock_syscall((void *)gateway, SYS_flock, descriptor, LOCK_SH | LOCK_NB);
    if (result.failed) {
        errno = (int)result.value;
        return -1;
    }
    return (int)result.value;
}
