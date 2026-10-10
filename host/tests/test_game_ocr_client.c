#include "acutest.h"
#include "../../shell/game_ocr_client.h"
#include "../../core/conn.h"
#include <string.h>

typedef struct {
    int submit_rc, read_rc, cancelled, malformed, cleanup, engine_rc;
    int submits, reads, cancels, delays, ready_after, abort_after;
    int64_t time;
} Fake;
static int submit(void *ud, const VjoGameOcrRequest *req) {
    Fake *f = ud; f->submits++; TEST_CHECK(req->size == sizeof(*req)); return f->submit_rc;
}
static int read_result(void *ud, uint32_t seq, VjoGameOcrResult *out) {
    Fake *f = ud; f->reads++;
    TEST_CHECK(seq == (uint32_t)f->submit_rc);
    if (f->read_rc) { memset(out, 0xA5, sizeof(*out)); return f->read_rc; }
    if (f->reads <= f->ready_after) return 1;
    out->size = sizeof(*out); out->seq = seq; out->rc = f->engine_rc;
    out->cleanup_status = f->cleanup;
    strcpy(out->text, "test");
    if (f->malformed == 1) out->seq++;
    if (f->malformed == 2) out->size--;
    if (f->malformed == 3) memset(out->text, 'x', sizeof(out->text));
    return 0;
}
static int cancel(void *ud, uint32_t seq) {
    Fake *f = ud; TEST_CHECK(seq == (uint32_t)f->submit_rc); f->cancels++; return -1;
}
static int64_t now(void *ud) { return ((Fake *)ud)->time; }
static void delay(void *ud, uint32_t us) { Fake *f = ud; f->delays++; f->time += us; }
static int cancelled(void *ud) {
    Fake *f = ud; return f->cancelled || (f->abort_after && f->reads >= f->abort_after);
}
static int exchange(Fake *f, VjoGameOcrResult *out) {
    VjoGameOcrRequest req = {.size = sizeof(req)};
    VjoGameOcrClient c = {f, submit, read_result, cancel, now, delay};
    return vjo_game_ocr_exchange(&c, &req, out, cancelled, f, 150000);
}
static void successful(void) {
    Fake f = {.submit_rc = 17, .ready_after = 2}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == 0 && !strcmp(out.text, "test"));
    TEST_CHECK(f.submits == 1 && f.reads == 3 && f.delays == 2 && !f.cancels);
}
static void refuses_missing_worker(void) {
    Fake f = {.submit_rc = -1}; VjoGameOcrResult out;
    memset(&out, 0xA5, sizeof(out));
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_UNAVAILABLE && !out.text[0]);
    TEST_CHECK(!f.reads && !f.cancels);
}
static void timeout_keeps_worker_cleanup_separate(void) {
    Fake f = {.submit_rc = 9, .ready_after = 99}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0]);
    TEST_CHECK(f.time == 150000 && f.reads == 3 && f.cancels == 1);
}
static void cancellation(void) {
    Fake f = {.submit_rc = 9, .ready_after = 99, .abort_after = 1}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && !out.text[0]);
    TEST_CHECK(f.reads == 1 && f.cancels == 1);
    f = (Fake){.submit_rc = 9, .cancelled = 1};
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && !f.submits);
}
static void rejects_stale_and_incomplete_results(void) {
    for (int malformed = 1; malformed <= 3; malformed++) {
        Fake f = {.submit_rc = 9, .malformed = malformed}; VjoGameOcrResult out;
        TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0]);
        TEST_CHECK(f.cancels == 1);
    }
    Fake f = {.submit_rc = 9, .read_rc = -4}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_CANCELLED && !out.text[0] && f.cancels == 1);
}
static void refuses_unclean_or_failed_engine(void) {
    Fake f = {.submit_rc = 9, .cleanup = -1}; VjoGameOcrResult out;
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0] && f.cancels == 1);
    f = (Fake){.submit_rc = 9, .engine_rc = VJO_E_OOM};
    TEST_CHECK(exchange(&f, &out) == VJO_E_OOM && !out.text[0] && !f.cancels);
    f = (Fake){.submit_rc = 9, .engine_rc = 1};
    TEST_CHECK(exchange(&f, &out) == VJO_E_OCR_INFERENCE && !out.text[0] && f.cancels == 1);
}
TEST_LIST = {
    {"completed worker", successful}, {"missing worker", refuses_missing_worker},
    {"bounded timeout", timeout_keeps_worker_cleanup_separate},
    {"cancellation", cancellation}, {"stale and incomplete", rejects_stale_and_incomplete_results},
    {"cleanup and engine failure", refuses_unclean_or_failed_engine}, {NULL, NULL}
};
