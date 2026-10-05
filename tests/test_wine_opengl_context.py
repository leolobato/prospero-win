#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise Wine's exact context transition body with a resizing drawable."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def body(source):
    start = source.index("static BOOL context_sync_drawables(")
    brace = source.index("{", start)
    depth = 1
    for end in range(brace + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if not depth:
            return source[start:end + 1]
    raise AssertionError("unterminated context_sync_drawables")


HARNESS = r'''
#include <assert.h>
#include <stddef.h>
typedef int BOOL;
typedef int HDC;
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
struct client { int hwnd; };
struct opengl_drawable { struct client *client; int interval; };
struct opengl_context {
    int format; void *driver_private;
    struct opengl_drawable *draw, *read;
};
static struct { struct opengl_context *glContext; } teb;
static struct { int client_current; } thread_data;
#define NtCurrentTeb() (&teb)
/* Use a macro because the actual body accesses this field. */
#define get_opengl_thread_data() (&thread_data)
static struct opengl_drawable drawable;
static int flushes, restored, fail_current;
static struct opengl_drawable *get_updated_drawable(HDC h,int f,struct opengl_drawable *d)
{ (void)h;(void)f;(void)d;return &drawable; }
static void opengl_drawable_add_ref(struct opengl_drawable *d) { assert(d); }
static void opengl_drawable_release(struct opengl_drawable *d) { assert(d); }
static struct opengl_drawable *get_target(struct opengl_drawable *d) { assert(d);return d; }
static BOOL make_current(struct opengl_drawable *d,struct opengl_drawable *r,void *p)
{ (void)p;assert(d && r);return !fail_current; }
static struct { BOOL (*p_make_current)(struct opengl_drawable *,struct opengl_drawable *,void *); }
    driver={make_current}, *driver_funcs=&driver;
static void context_exchange_drawables(struct opengl_context *c,
                                      struct opengl_drawable **d,struct opengl_drawable **r)
{ struct opengl_drawable *t=c->draw;c->draw=*d;*d=t;t=c->read;c->read=*r;*r=t; }
static void set_window_opengl_drawable(int h,struct opengl_drawable *d,BOOL active)
{ (void)h;(void)d;(void)active; }
static void opengl_drawable_flush(struct opengl_drawable *d,int interval,int flags)
{
    (void)d;(void)interval;(void)flags;flushes++;
    /* Resizing uses Wine's internal context, then restores the client. */
    struct opengl_context *c=teb.glContext;
    assert(c && c->draw && c->read);
    assert(driver_funcs->p_make_current(get_target(c->draw),get_target(c->read),c->driver_private));
    thread_data.client_current=1;restored++;
}
'''

MAIN = r'''
int main(void)
{
    struct client client={1};drawable.client=&client;
    struct opengl_context first={0}, second={0};
    assert(context_sync_drawables(&first,1,1));
    assert(teb.glContext==&first && first.draw==&drawable && first.read==&drawable);
    assert(flushes==2 && restored==2);
    assert(context_sync_drawables(&second,1,1));
    assert(teb.glContext==&second && second.draw==&drawable && second.read==&drawable);
    assert(!first.draw && !first.read && flushes==4);
    assert(context_sync_drawables(&second,1,1));
    assert(flushes==4); /* already current: no extra transition */
    struct opengl_context failed={0};fail_current=1;
    assert(!context_sync_drawables(&failed,1,1));
    assert(teb.glContext==&second && second.draw==&drawable && second.read==&drawable);
    assert(!failed.draw && !failed.read && flushes==4);
    return 0;
}
'''


def main():
    if len(sys.argv) > 1:
        source = Path(sys.argv[1]).read_text()
    else:
        patch = (ROOT / "wine/patches/0663-win32u-attach-drawables-before-flushing.patch").read_text()
        source = "\n".join(line[1:] for line in patch.splitlines()
                           if line.startswith((" ", "+")) and not line.startswith("+++"))
    with tempfile.TemporaryDirectory(prefix="pw-opengl-context-") as temp:
        folder = Path(temp)
        fixture = folder / "test.c"
        fixture.write_text(HARNESS + body(source) + MAIN)
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c11", *shlex.split(os.environ.get("CFLAGS", "-O2 -Wall -Wextra -Werror")),
                   str(fixture), "-o", str(folder / "test")]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(folder / "test")], check=True, timeout=5)
    print("OpenGL context transitions passed: initial, replacement, already current, failed rollback")


if __name__ == "__main__":
    main()
