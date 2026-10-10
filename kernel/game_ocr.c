/* OCR requests and copied results only. Inference remains in the game process.
 * g.lock serializes this transport with capture allocation/read/free. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include "vjo_kernel.h"
#include "game_ocr.h"

static VjoGameOcrRequest request;
static VjoGameOcrResult result, incoming; /* Never put a 4 KiB result on syscall stacks. */
static SceUID worker_pid, worker_module, request_pid, request_shell;
static uint32_t worker_epoch, request_epoch, sequence;
static int valid, pending, claimed, cancelled, completed, stopping;

static int shell_caller(SceUID pid)
{
    return pid > 0 && pid == g.shell_pid;
}

static int registration_current(SceUID pid)
{
    return pid > 0 && pid == worker_pid && pid == g.game_pid && g.game_active &&
           !stopping &&
           worker_epoch == __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE);
}

static void invalidate_locked(void)
{
    if (!valid) return;
    cancelled = 1;
    completed = 0;
    pending = 0;
    /* A taken job can still be executing after suspend, timeout or Shell
     * restart. Only its drained completion or actual process exit unpins it. */
}

static void sync_locked(void)
{
    if (valid && (request_epoch != __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE) ||
                  request_pid != g.game_pid || !g.game_active ||
                  request_shell != g.shell_pid))
        invalidate_locked();
}

void game_ocr_init(void)
{
    memset(&request, 0, sizeof(request));
    memset(&result, 0, sizeof(result));
    memset(&incoming, 0, sizeof(incoming));
    worker_pid = worker_module = request_pid = request_shell = 0;
    worker_epoch = request_epoch = sequence = 0;
    valid = pending = claimed = cancelled = completed = stopping = 0;
}

int game_ocr_capture_busy_locked(void)
{
    sync_locked();
    return pending || claimed;
}

int game_ocr_raw_caller_locked(SceUID pid)
{
    sync_locked();
    return valid && claimed && !cancelled && pid == request_pid &&
           registration_current(pid) && request.done_seq == g.done_seq &&
           request.done_seq == g.capture_seq && request.width == g.crop_w &&
           g.capture_game_pid == request_pid && g.capture_epoch == request_epoch &&
           request.height == g.crop_h && request.stride == g.raw_stride &&
           !g.capture_once;
}

int game_ocr_shutdown_locked(void)
{
    sync_locked();
    if (pending || claimed) return VJO_ERR_BUSY;
    stopping = 1;
    invalidate_locked();
    return 0;
}

void game_ocr_shell_changed_locked(void)
{
    __atomic_add_fetch(&g.text_epoch, 1, __ATOMIC_RELEASE);
    invalidate_locked();
    worker_epoch = 0;
}

void game_ocr_foreground_changed_locked(void)
{
    sync_locked();
}

void game_ocr_process_gone(SceUID pid)
{
    VJO_LOCK();
    if (pid == request_pid) {
        invalidate_locked();
        claimed = pending = 0;
    }
    if (pid == worker_pid) {
        worker_pid = worker_module = 0;
        worker_epoch = 0;
    }
    VJO_UNLOCK();
}

int vjoOcrRegister(void)
{
    uint32_t state;
    tai_module_info_t info;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    rc = taiGetModuleInfoForKernel(pid, VJO_GAME_OCR_MODULE, &info);
    if (rc < 0) { rc = VJO_ERR_PERM; goto out; }
    VJO_LOCK();
    sync_locked();
    if (stopping || pid <= 0 || pid != g.game_pid || !g.game_active) rc = VJO_ERR_NO_GAME;
    else if (claimed && (pid != worker_pid || worker_module != info.modid ||
                        worker_epoch != __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE)))
        rc = VJO_ERR_BUSY;
    else {
        worker_pid = pid;
        worker_module = info.modid;
        worker_epoch = __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE);
        rc = 0;
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrSubmit(const VjoGameOcrRequest *user_request)
{
    uint32_t state;
    VjoGameOcrRequest next;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    if (!user_request || ksceKernelMemcpyUserToKernel(&next, user_request, sizeof(next)) < 0) {
        rc = VJO_ERR_COPY; goto out;
    }
    if (next.size != sizeof(next) || next.layout > VJO_GAME_OCR_DIALOGUE_BOX ||
        !next.model_dir[0] || next.model_dir[sizeof(next.model_dir)-1] != 0) {
        rc = VJO_ERR_ARG; goto out;
    }
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (pending || claimed) rc = VJO_ERR_BUSY;
    else if (!registration_current(worker_pid)) rc = VJO_ERR_NO_GAME;
    else if (g.capture_state != CAPTURE_IDLE || !g.raw_valid || !g.raw ||
             g.capture_result || g.capture_once || !next.done_seq ||
             next.done_seq != g.done_seq || next.done_seq != g.capture_seq ||
             g.capture_game_pid != worker_pid ||
             g.capture_epoch != __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE) ||
             !g.crop_w || !g.crop_h || g.crop_w > VJO_MAX_W || g.crop_h > VJO_MAX_H ||
             g.raw_stride != g.crop_w * 4 || g.raw_capacity < g.raw_stride * g.crop_h)
        rc = VJO_ERR_ARG;
    else if (sequence == 0x7FFFFFFFu) rc = VJO_ERR_BUSY; /* Never recycle a sequence. */
    else {
        next.seq = ++sequence;
        next.width = g.crop_w;
        next.height = g.crop_h;
        next.stride = g.raw_stride;
        request = next;
        request_pid = worker_pid;
        request_shell = pid;
        request_epoch = __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE);
        memset(&result, 0, sizeof(result));
        valid = pending = 1;
        claimed = cancelled = completed = 0;
        rc = (int)request.seq;
    }
    VJO_UNLOCK();
out:
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrTake(VjoGameOcrRequest *out)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!registration_current(pid)) rc = VJO_ERR_PERM;
    else if (claimed) rc = VJO_ERR_BUSY;
    else if (!pending || !valid || cancelled) rc = 1;
    else if (!out || ksceKernelMemcpyKernelToUser(out, &request, sizeof(request)) < 0)
        rc = VJO_ERR_COPY;
    else { pending = 0; claimed = 1; rc = 0; }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrCancelled(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed || seq != request.seq) rc = VJO_ERR_ARG;
    else rc = cancelled ? 1 : 0;
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrComplete(const VjoGameOcrResult *user_result)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (pid <= 0 || pid != request_pid) rc = VJO_ERR_PERM;
    else if (!valid || !claimed) rc = VJO_ERR_ARG;
    else if (!user_result || ksceKernelMemcpyUserToKernel(&incoming, user_result, sizeof(incoming)) < 0)
        rc = VJO_ERR_COPY;
    else if (incoming.size != sizeof(incoming) || incoming.seq != request.seq ||
             incoming.text[sizeof(incoming.text)-1] != 0 ||
             (!incoming.rc && incoming.cleanup_status)) rc = VJO_ERR_ARG;
    else {
        if (!cancelled) {
            result = incoming;
            completed = 1;
        }
        if (!incoming.cleanup_status) claimed = 0;
        /* Failed cleanup retains the claim and may be retried. Completed
         * cancellation drains the reader but never republishes stale text. */
        rc = 0;
    }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrRead(uint32_t seq, VjoGameOcrResult *out)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
    else if (cancelled) rc = VJO_ERR_CANCELLED;
    else if (!completed) rc = 1;
    else if (!out || ksceKernelMemcpyKernelToUser(out, &result, sizeof(result)) < 0)
        rc = VJO_ERR_COPY;
    else rc = 0;
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}

int vjoOcrCancel(uint32_t seq)
{
    uint32_t state;
    SceUID pid = ksceKernelGetProcessId();
    int rc;
    if (!shell_caller(pid)) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    VJO_LOCK();
    sync_locked();
    if (!shell_caller(pid)) rc = VJO_ERR_PERM;
    else if (!valid || seq != request.seq || request_shell != pid) rc = VJO_ERR_ARG;
    else { invalidate_locked(); rc = 0; }
    VJO_UNLOCK();
    EXIT_SYSCALL(state);
    return rc;
}
