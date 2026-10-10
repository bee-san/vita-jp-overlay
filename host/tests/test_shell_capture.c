/* Capture helper errors must never read stale raw rows. Real JPEG encoder. */
#include "acutest.h"
#include "client.h"
#include <psp2/host_stubs.h>
static VjoState state;
static uint64_t time_us;
static unsigned discarded, rows_read, flags_seen;
static int state_rc, request_rc, rows_rc;
uint64_t sceKernelGetProcessTimeWide(void) { return time_us; }
int sceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }
void vjo_log(const char *fmt, ...) {}
int vjoGetState(VjoState *out) { *out = state; return state_rc; }
int vjoRequestCapture(uint32_t flags)
{ if (flags == VJO_CAPTURE_DISCARD) { discarded++; return 0; }
  flags_seen = flags; return request_rc; }
int vjoWaitEvent(uint32_t mask, uint32_t *bits, uint32_t timeout)
{ time_us += timeout; *bits = 0; return -1; }
int vjoReadRaw(uint32_t row, uint32_t count, void *dst)
{ rows_read++; if (rows_rc < 0) return rows_rc;
  memset(dst, 255, count * state.raw_stride); return rows_rc ? (int)count-1 : (int)count; }
#include "../../shell/capture.c"
static uint8_t memory[32768];
static void reset(VjoArena *a, VjoBuf *b)
{ memset(&state, 0, sizeof(state)); state.done_seq = 1; state.width = 8;
  state.height = 2; state.raw_stride = 32; time_us = 0;
  discarded = rows_read = flags_seen = 0; state_rc = rows_rc = 0; request_rc = 1;
  vjo_arena_init(a, memory, sizeof(memory)); vjo_buf_init(b, a); }
static void test_capture_lifecycle(void)
{ VjoArena a; VjoBuf b; VjoState out; reset(&a, &b);
  TEST_CHECK(vjo_capture_jpeg(&a, 0, 80, &b, &out) == VJO_OK);
  TEST_CHECK(b.len > 2 && b.data[0] == 255 && b.data[1] == 216);
  TEST_CHECK(flags_seen == VJO_CAPTURE_ONCE && rows_read > 0 && discarded == 0); }
static void test_failed_state_cannot_authorize_rows(void)
{ VjoArena a; VjoBuf b; VjoState out; reset(&a, &b); state_rc = -1;
  TEST_CHECK(vjo_capture_jpeg(&a, 0, 80, &b, &out) == VJO_E_SOURCE);
  TEST_CHECK(!rows_read && discarded == 1); }
static void test_rows_failure(void)
{ for (int mode = -1; mode <= 1; mode += 2) {
    VjoArena a; VjoBuf b; VjoState out; reset(&a, &b); rows_rc = mode;
    TEST_CHECK(vjo_capture_jpeg(&a, 0, 80, &b, &out) == VJO_E_SOURCE);
    TEST_CHECK(discarded == 1); } }
static void test_timeout(void)
{ VjoArena a; VjoBuf b; VjoState out; reset(&a, &b); state.done_seq = 9;
  TEST_CHECK(vjo_capture_jpeg(&a, 0, 80, &b, &out) == VJO_E_SOURCE);
  TEST_CHECK(time_us >= CAPTURE_TIMEOUT_US && !rows_read && discarded == 1); }
static void test_kernel_memory_failure(void)
{ VjoArena a; VjoBuf b; VjoState out; reset(&a, &b); request_rc = VJO_ERR_NO_MEMORY;
  TEST_CHECK(vjo_capture_jpeg(&a, 0, 80, &b, &out) == VJO_E_OOM);
  TEST_CHECK(!rows_read && !discarded); }
TEST_LIST = {
  {"jpeg_releases_capture", test_capture_lifecycle},
  {"failed_state_never_reads_rows", test_failed_state_cannot_authorize_rows},
  {"raw_error_and_short_read_release_capture", test_rows_failure},
  {"capture_timeout_releases", test_timeout},
  {"capture_memory_failure", test_kernel_memory_failure}, {NULL, NULL}
};
