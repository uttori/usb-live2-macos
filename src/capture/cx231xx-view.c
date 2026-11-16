// SPDX-License-Identifier: GPL-2.0-or-later
#define _DARWIN_C_SOURCE
#include "cx231xx-view.h"
#include "cx231xx-raw.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/** Only the worker writes to the child. Keep at most one frame waiting behind the current write. */
struct live_view {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    int socket;
    pid_t child;
    bool pending, stopping, failed;
    uint8_t latest[NTSC_FRAME_BYTES];
    uint8_t current[NTSC_FRAME_BYTES];
};

/** Send a complete raw frame, returning an errno value if the pipe closes. */
static int send_frame(struct live_view *view)
{
    size_t offset = 0;
    while (offset < sizeof view->current) {
        ssize_t n = send(view->socket, view->current + offset, sizeof view->current - offset, 0);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return n < 0 ? errno : EPIPE;
    }
    return 0;
}

static void *send_frames(void *opaque)
{
    struct live_view *view = opaque;
    int error = 0;
    pthread_mutex_lock(&view->lock);
    while (!error && !view->stopping) {
        while (!view->pending && !view->stopping)
            pthread_cond_wait(&view->ready, &view->lock);
        if (view->stopping)
            break;

        // Copy before unlocking so USB can replace the waiting frame while the child is blocked.
        memcpy(view->current, view->latest, sizeof view->current);
        view->pending = false;
        pthread_mutex_unlock(&view->lock);
        error = send_frame(view);
        pthread_mutex_lock(&view->lock);
    }
    if (!view->stopping) {
        view->failed = true;
        fprintf(stderr, "Live output closed: %s.\n", strerror(error));
    }
    view->stopping = true;
    pthread_mutex_unlock(&view->lock);
    shutdown(view->socket, SHUT_WR);
    return NULL;
}

static int spawn_view(pid_t *child, enum live_view_kind kind, int input, bool handoff)
{
    // Display conversion belongs to the viewer and never reaches the archival encoder.
    static char *const preview[] = {
        "ffplay", "-hide_banner", "-loglevel", "warning", "-nostats", "-autoexit",
        "-fflags", "nobuffer", "-framedrop", "-f", "rawvideo", "-pixel_format", "yuyv422",
        "-video_size", "720x480", "-framerate", "30000/1001", "-i", "pipe:0",
        "-vf", "yadif=mode=send_frame:parity=tff:deint=all,setsar=8/9",
        "-window_title", "USB-Live2 Preview", NULL
    };
    static char default_publisher[] = "/Applications/USB-Live2 Camera.app/Contents/Helpers/usblive2-camera-publish";
    char *publisher = getenv("USBLIVE2_CAMERA_PUBLISHER");
    char *const camera[] = {publisher && *publisher ? publisher : default_publisher,
                            handoff ? "--usb-handoff" : NULL, NULL};
    char *const *command = kind == VIEW_CAMERA ? camera : preview;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
        return error;
    posix_spawnattr_t attributes;
    error = posix_spawnattr_init(&attributes);
    if (!error) {
        // Children must not hold the recording, report, or other viewer's descriptors open.
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
        if (!error)
            error = posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
        if (!error)
            error = posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
        if (!error)
            error = posix_spawnp(child, command[0], &actions, &attributes, command, environ);
        posix_spawnattr_destroy(&attributes);
    }
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

/** Give the child time to exit on EOF, then stop it if it cannot shut down on its own. */
static void reap_view(pid_t child)
{
    if (!child)
        return;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pid_t result = waitpid(child, NULL, WNOHANG);
        if (result == child || (result < 0 && errno != EINTR))
            return;
        const struct timespec delay = {.tv_nsec = 25000000};
        nanosleep(&delay, NULL);
    }
    kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
    }
}

struct live_view *live_view_open(enum live_view_kind kind)
{
    struct live_view *view = calloc(1, sizeof *view);
    if (!view) {
        perror("Allocate live video output");
        return NULL;
    }
    int sockets[2] = {-1, -1};
    int error = pthread_mutex_init(&view->lock, NULL);
    bool mutex = error == 0;
    if (!error)
        error = pthread_cond_init(&view->ready, NULL);
    bool cond = error == 0;
    if (!error && socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
        error = errno;
    const int no_signal = 1;
    // Keep future viewers from inheriting either socket. A closed viewer cannot SIGPIPE the recorder.
    if (!error && (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) < 0 ||
                   fcntl(sockets[1], F_SETFD, FD_CLOEXEC) < 0 ||
                   setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof no_signal) < 0))
        error = errno;
    const char *publisher = getenv("USBLIVE2_CAMERA_PUBLISHER");
    // Older installed apps and custom publisher paths still use the input-only protocol.
    const char *handoff_marker = "/Applications/USB-Live2 Camera.app/Contents/Resources/publisher-usb-handoff";
    bool handoff = kind == VIEW_CAMERA && (!publisher || !*publisher) && access(handoff_marker, F_OK) == 0;
    if (!error)
        error = spawn_view(&view->child, kind, sockets[1], handoff);
    if (sockets[1] >= 0)
        close(sockets[1]);
    if (!error && handoff) {
        // Capture must wait for the extension to release USB when a camera client is already open.
        struct pollfd readiness = {.fd = sockets[0], .events = POLLIN};
        char ready;
        int result = poll(&readiness, 1, 10000);
        if (result < 0)
            error = errno;
        else if (result == 0)
            error = ETIMEDOUT;
        else if (recv(sockets[0], &ready, 1, 0) != 1 || ready != 'R')
            error = EIO;
    }
    if (!error) {
        view->socket = sockets[0];
        error = pthread_create(&view->thread, NULL, send_frames, view);
    }
    if (!error)
        return view;
    static const char *const names[] = {"preview (ffplay)", "USB-Live2 camera publisher"};
    fprintf(stderr, "Could not start %s: %s.\n", names[kind], strerror(error));
    if (sockets[0] >= 0)
        close(sockets[0]);
    reap_view(view->child);
    if (cond)
        pthread_cond_destroy(&view->ready);
    if (mutex)
        pthread_mutex_destroy(&view->lock);
    free(view);
    return NULL;
}

bool live_view_running(struct live_view *view)
{
    if (!view)
        return false;
    pthread_mutex_lock(&view->lock);
    // Do not reap until close, so the stored PID cannot be reused during cleanup.
    siginfo_t child = {0};
    int status = waitid(P_PID, (id_t)view->child, &child, WEXITED | WNOHANG | WNOWAIT);
    bool running = !view->stopping && (status < 0 ? errno == EINTR : child.si_pid == 0);
    pthread_mutex_unlock(&view->lock);
    return running;
}

bool live_view_failed(struct live_view *view)
{
    if (!view)
        return false;
    pthread_mutex_lock(&view->lock);
    bool failed = view->failed;
    pthread_mutex_unlock(&view->lock);
    siginfo_t child = {0};
    return failed || (waitid(P_PID, (id_t)view->child, &child, WEXITED | WNOHANG | WNOWAIT) == 0 &&
                      child.si_pid != 0 && (child.si_code != CLD_EXITED || child.si_status != 0));
}

void live_view_offer(struct live_view *view, const uint8_t *frame)
{
    if (!view || pthread_mutex_trylock(&view->lock))
        return;
    if (!view->stopping) {
        memcpy(view->latest, frame, sizeof view->latest);
        view->pending = true;
        pthread_cond_signal(&view->ready);
    }
    pthread_mutex_unlock(&view->lock);
}

void live_view_close(struct live_view *view)
{
    if (!view)
        return;
    pthread_mutex_lock(&view->lock);
    view->stopping = true;
    pthread_cond_signal(&view->ready);
    pthread_mutex_unlock(&view->lock);
    // Interrupt a blocked send before joining the worker, even if the child stopped reading.
    shutdown(view->socket, SHUT_RDWR);
    pthread_join(view->thread, NULL);
    close(view->socket);
    reap_view(view->child);
    pthread_cond_destroy(&view->ready);
    pthread_mutex_destroy(&view->lock);
    free(view);
}
