#include "game_ocr_client.h"
#include "../core/conn.h"
#include <string.h>

int vjo_game_ocr_exchange(const VjoGameOcrClient *client,
                          const VjoGameOcrRequest *request,
                          VjoGameOcrResult *result,
                          int (*cancelled)(void *), void *cancel_ud,
                          int64_t timeout_us)
{
    int rc, seq;
    int64_t start;
    if (!result) return VJO_E_SOURCE;
    memset(result, 0, sizeof(*result));
    if (!client || !request || !client->submit || !client->read ||
        !client->cancel || !client->now_us || !client->delay_us || timeout_us <= 0)
        return VJO_E_SOURCE;
    if (cancelled && cancelled(cancel_ud)) return VJO_E_CANCELLED;
    start = client->now_us(client->ud);
    if (start < 0) return VJO_E_SOURCE;
    seq = client->submit(client->ud, request);
    if (seq <= 0) return VJO_E_OCR_UNAVAILABLE;
    for (;;) {
        if (cancelled && cancelled(cancel_ud)) { rc = VJO_E_CANCELLED; break; }
        int64_t now = client->now_us(client->ud);
        if (now < start || now - start >= timeout_us) { rc = VJO_E_OCR_INFERENCE; break; }
        memset(result, 0, sizeof(*result));
        result->size = sizeof(*result);
        rc = client->read(client->ud, (uint32_t)seq, result);
        if (!rc) {
            if (result->size != sizeof(*result) || result->seq != (uint32_t)seq ||
                !memchr(result->text, 0, sizeof(result->text)) || result->rc > 0 ||
                result->cleanup_status) { rc = VJO_E_OCR_INFERENCE; break; }
            rc = result->rc;
            if (rc) memset(result->text, 0, sizeof(result->text));
            if (cancelled && cancelled(cancel_ud)) {
                memset(result->text, 0, sizeof(result->text));
                return VJO_E_CANCELLED;
            }
            return rc;
        }
        if (rc != 1) { rc = VJO_E_CANCELLED; break; }
        client->delay_us(client->ud, 50000);
    }
    client->cancel(client->ud, (uint32_t)seq);
    memset(result->text, 0, sizeof(result->text));
    return rc;
}
