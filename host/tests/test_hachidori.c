/* Relay contract tests use real HTTP/parser code with an in-memory socket. */
#include <stdio.h>
#include <string.h>
#include "acutest.h"
#include "client.h"
#include "replay.h"

static uint8_t mem[4u << 20];
static VjoArena A;
static void setup(void) { memset(mem, 0xA5, sizeof(mem)); vjo_arena_init(&A, mem, sizeof(mem)); }

static void test_relay_config(void)
{
    VjoConfig c;
    int relay = vjo_dict_find("HACHIDORI");
    const char *ini = "dictionary = hachidori\nhachidori_host = anki.local:19634 ; LAN\n";
    TEST_ASSERT(relay >= 0);
    vjo_config_defaults(&c);
    TEST_CHECK(c.dictionary == relay);
    TEST_CHECK(!*vjo_config_api_key(&c));
    TEST_CHECK(vjo_dict_info(relay)->key_env == NULL);
    TEST_CHECK(vjo_dict_info(relay)->key_setting == NULL);
    vjo_config_parse(&c, ini, strlen(ini));
    TEST_CHECK(c.dictionary == relay && c.n_warnings == 0);
    TEST_CHECK(strstr(vjo_config_default_text(), "dictionary = hachidori\n") != NULL);
    TEST_CHECK(strstr(vjo_config_default_text(), "hachidori_host =\n") != NULL);
}


#define MISS "{\"index\":0,\"dictionaryEntries\":[],\"originalTextLength\":0}"
#define ENTRY(term, reading, gloss) "{\"headwords\":[{\"term\":\"" term "\",\"reading\":\"" reading "\"}],\"definitions\":[{\"entries\":[\"" gloss "\"]}]}"
#define HIT(n, entry) "{\"index\":0,\"originalTextLength\":" #n ",\"dictionaryEntries\":[" entry "]}"

static struct {
    char replies[128][4096], sent[128][8192], host[64];
    VjoMemConn conn;
    int n, next, port, disconnected;
} F;
static int connect_fake(void *ud, const char *host, int port, int timeout, int io_timeout, VjoConn *out)
{
    (void)ud; (void)timeout; (void)io_timeout;
    if (F.next >= F.n) return VJO_E_NET;
    snprintf(F.host, sizeof(F.host), "%s", host);
    F.port = port;
    memset(&F.conn, 0, sizeof(F.conn));
    F.conn.in = F.replies[F.next]; F.conn.len = strlen(F.conn.in);
    F.conn.out = F.sent[F.next]; F.conn.out_cap = sizeof(F.sent[0]) - 1;
    F.next++;
    vjo_memconn_init(&F.conn, out);
    return VJO_OK;
}
static void disconnect_fake(void *ud, VjoConn *c)
{
    (void)ud; (void)c;
    F.sent[F.next - 1][F.conn.out_len] = '\0';
    F.disconnected++;
}
static VjoPlatform P;
static VjoConfig C;
static void net_setup(void)
{
    setup(); memset(&F, 0, sizeof(F)); memset(&P, 0, sizeof(P));
    P.connect = connect_fake; P.disconnect = disconnect_fake;
    vjo_config_defaults(&C); C.dictionary = vjo_dict_find("hachidori");
    snprintf(C.hachidori_host, sizeof(C.hachidori_host), "anki.local");
    strcpy(C.api_key[VJO_DICT_JITEN], "never-send-this-key");
}
static void reply(int status, const char *json)
{
    TEST_ASSERT(F.n < 128);
    snprintf(F.replies[F.n++], sizeof(F.replies[0]),
             "HTTP/1.1 %d Reply\r\nContent-Length: %lu\r\n\r\n%s", status, (unsigned long)strlen(json), json);
}
static void test_relay_endpoint(void)
{
    static const char *bad[] = {"http://anki.local", "https://h", "auto", ":19633", "h:", "h:0",
        "h:65536", "h:999999999999", "h:-1", "h:1/x", "h/path", "h@x", "h x", "h\rX:1", "[::1]", ".", "-h", "h..local"};
    char ini[256];
    VjoConfig c;
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        vjo_config_defaults(&c);
        snprintf(ini, sizeof(ini), "hachidori_host = %s\n", bad[i]);
        vjo_config_parse(&c, ini, strlen(ini));
        TEST_CHECK_(c.n_warnings > 0 && !c.hachidori_host[0], "%s rejected", bad[i]);
    }
    vjo_config_defaults(&c);
    const char *good = "hachidori_host = 192.168.1.23:65535\n";
    vjo_config_parse(&c, good, strlen(good));
    TEST_CHECK(c.n_warnings == 0 && !strcmp(c.hachidori_host, "192.168.1.23:65535"));
}
static void test_relay_missing_host(void)
{
    VjoDictResult r; VjoErr e;
    net_setup(); C.hachidori_host[0] = '\0';
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) != VJO_OK);
    TEST_CHECK(F.next == 0 && e.rc != VJO_E_NO_KEY);
    TEST_CHECK(strstr(vjo_err_text(&A, VJO_STAGE_DICT, &e), "hachidori_host") != NULL);
    strcpy(C.hachidori_host, "h\r\nX:1");
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) != VJO_OK && F.next == 0);
}
static void test_relay_scan(void)
{
    VjoDictResult r; VjoErr e;
    net_setup();
    reply(200, HIT(4, ENTRY("分かる", "わかる", "to understand")));
    reply(200, MISS); /* supplementary unknown must be advanced as one code point */
    reply(200, HIT(3, ENTRY("大きい", "おおきい", "big")));
    reply(200, HIT(4, ENTRY("分かる", "わかる", "to understand")));
    TEST_ASSERT(vjo_dict_lookup(&A, &P, &C, "分かった😀大きい分かった", &r, &e) == VJO_OK);
    TEST_CHECK(F.next == 4 && F.disconnected == 4 && F.port == 19633 && !strcmp(F.host, "anki.local"));
    TEST_CHECK(r.n_tokens == 3 && r.n_vocab == 2);
    TEST_ASSERT(r.n_tokens == 3);
    TEST_CHECK(r.tokens[0].pos16 == 0 && r.tokens[0].len16 == 4);
    TEST_CHECK(r.tokens[1].pos16 == 6 && r.tokens[1].len16 == 3);
    TEST_CHECK(r.tokens[2].pos16 == 9 && r.tokens[2].len16 == 4);
    TEST_CHECK(r.tokens[0].vocab == r.tokens[2].vocab);
    TEST_CHECK(!strcmp(r.vocab[0].spelling, "分かる") && !strcmp(r.vocab[0].reading, "わかる"));
    TEST_CHECK(!strcmp(r.vocab[1].meanings[0], "big") && r.vocab[1].rank == VJO_NO_RANK);
    TEST_CHECK(strstr(F.sent[0], "POST /termEntries HTTP/1.1\r\nHost: anki.local:19633\r\n") != NULL);
    TEST_CHECK(strstr(F.sent[0], "{\"term\":\"分かった😀大きい分かった\"}") != NULL);
    TEST_CHECK(strstr(F.sent[1], "{\"term\":\"😀大きい分かった\"}") != NULL);
    TEST_CHECK(strstr(F.sent[2], "{\"term\":\"大きい分かった\"}") != NULL);
    for (int i = 0; i < F.next; i++) {
        TEST_CHECK(!strstr(F.sent[i], "Api-Key") && !strstr(F.sent[i], "Authorization") && !strstr(F.sent[i], "never-send"));
    }
}
static void test_relay_glossary(void)
{
    VjoDictResult r; VjoErr e;
    net_setup(); strcpy(C.hachidori_host, "127.0.0.1:12345");
    reply(200, HIT(1, "{\"headwords\":[{\"term\":\"猫\",\"reading\":\"ねこ\"}],"
        "\"definitions\":[{\"dictionary\":\"secret metadata\",\"entries\":[\"cat\","
        "{\"type\":\"structured-content\",\"content\":{\"tag\":\"div\",\"style\":{\"fontStyle\":\"italic\"},"
        "\"content\":[\"feline \",{\"tag\":\"a\",\"href\":\"https://secret\",\"content\":\"animal\"},"
        "{\"tag\":\"br\"}, {\"tag\":\"span\",\"data\":{\"x\":\"secret\"},\"content\":\"pet\"}]}}]}],"
        "\"frequencies\":[{\"frequency\":0},{\"frequency\":-1},{\"frequency\":\"bad\"},{\"frequency\":123}]}"));
    TEST_ASSERT(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_OK);
    TEST_CHECK(F.port == 12345 && !strcmp(F.host, "127.0.0.1"));
    TEST_CHECK(strstr(F.sent[0], "Host: 127.0.0.1:12345\r\n") != NULL);
    TEST_ASSERT(r.n_vocab == 1 && r.vocab[0].n_meanings == 2);
    TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "cat"));
    TEST_CHECK(!strcmp(r.vocab[0].meanings[1], "feline animal\npet"));
    TEST_CHECK(r.vocab[0].rank == 123);
}
static void test_relay_newline_positions(void)
{
    VjoOverlayData d;
    net_setup(); memset(&d, 0, sizeof(d));
    reply(200, HIT(4, ENTRY("分かる", "わかる", "understand")));
    reply(200, HIT(1, ENTRY("猫", "ねこ", "cat")));
    TEST_ASSERT(vjo_overlay_from_text(&A, &P, &C, "分か\nった\n猫", &d) == VJO_OK);
    TEST_CHECK(d.list.n_entries == 2);
    TEST_CHECK(!strcmp(d.list.entries[0].vocab->spelling, "分かる"));
    TEST_CHECK(strstr(F.sent[0], "{\"term\":\"分かった猫\"}") != NULL);

}


static void test_relay_malformed(void)
{
    const char *bad[] = {"", "{}", "null", "[]", "{", "{\"index\":0,\"dictionaryEntries\":[],\"originalTextLength\":0} {}",
        "{\"index\":0 \"dictionaryEntries\":[],\"originalTextLength\":0}",
        "{\"index\":0,\"dictionaryEntries\":[],\"originalTextLength\":0,}",
        "{\"index\":0,\"dictionaryEntries\":[],\"originalTextLength\":00}",
        HIT(0, ENTRY("猫", "ねこ", "cat")), HIT(2, ENTRY("猫", "ねこ", "cat")),
        HIT(-1, ENTRY("猫", "ねこ", "cat")), HIT(1.5, ENTRY("猫", "ねこ", "cat")),
        HIT(1, "{\"headwords\":[],\"definitions\":[]}"),
        HIT(1, "{\"headwords\":[{\"term\":\"\",\"reading\":\"x\"}],\"definitions\":[]}"),
        HIT(1, "{\"headwords\":[{\"term\":\"猫\",\"reading\":null}],\"definitions\":[]}"),
        HIT(1, "{\"headwords\":[{\"term\":\"猫\",\"reading\":\"x\"}],\"definitions\":null}"),
        HIT(1, ENTRY("猫", "ねこ", "bad\\u0000hidden")),
        HIT(1, ENTRY("猫", "ねこ", "bad\ncontrol")),
        HIT(1, ENTRY("猫", "ねこ", "bad\\xescape"))};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        VjoDictResult r; VjoErr e;
        net_setup(); reply(200, bad[i]);
        TEST_CHECK_(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_E_PARSE, "malformed case %u rejected", i);
        TEST_CHECK(r.n_vocab == 0 && r.n_tokens == 0 && F.next == 1);
    }
    VjoDictResult r; VjoErr e;
    net_setup(); reply(200, HIT(1, ENTRY("猫", "ねこ", "cat"))); reply(200, "{}");
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫犬", &r, &e) == VJO_E_PARSE);
    TEST_CHECK(r.n_tokens == 0 && r.n_vocab == 0); /* never expose a partial sentence */
    net_setup(); reply(200, HIT(1, ENTRY("😀", "", "smile")));
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "😀", &r, &e) == VJO_E_PARSE); /* half a surrogate */
}
static void test_relay_status_errors(void)
{
    const int codes[] = {400, 401, 403, 500, 501, 503, 504};
    for (unsigned i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
        VjoDictResult r; VjoErr e;
        net_setup(); reply(codes[i], "{\"error\":\"relay unavailable: update Hachidori\"}");
        TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_E_STATUS);
        TEST_CHECK(e.http_status == codes[i] && e.dict == VJO_DICT_HACHIDORI && F.next == 1);
        TEST_CHECK(e.detail && !strcmp(e.detail, "relay unavailable: update Hachidori"));
        TEST_CHECK(!strstr(vjo_err_text(&A, VJO_STAGE_DICT, &e), "API key"));
        /* released scratch may be reused without changing the copied error */
        memset(vjo_arena_alloc(&A, 4096), 0xCC, 4096);
        TEST_CHECK(e.detail && !strcmp(e.detail, "relay unavailable: update Hachidori"));
    }
    VjoDictResult r; VjoErr e;
    net_setup();
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_E_NET && F.next == 0);
}
static void test_relay_empty_unmatched(void)
{
    VjoDictResult r; VjoErr e;
    net_setup();
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "", &r, &e) == VJO_OK && F.next == 0 && r.n_tokens == 0);
    reply(200, MISS); reply(200, MISS);
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "😀。", &r, &e) == VJO_OK && F.next == 2);
    TEST_CHECK(r.n_tokens == 0 && r.n_vocab == 0);
    TEST_CHECK(strstr(F.sent[1], "{\"term\":\"。\"}") != NULL);
    net_setup(); reply(200, HIT(2, ENTRY("𠮷", "よし", "name"))); reply(200, HIT(1, ENTRY("猫", "ねこ", "cat")));
    TEST_ASSERT(vjo_dict_lookup(&A, &P, &C, "𠮷猫", &r, &e) == VJO_OK);
    TEST_CHECK(r.tokens[0].len16 == 2 && r.tokens[1].pos16 == 2);
}
static void test_relay_request_escaping(void)
{
    VjoDictResult r; VjoErr e;
    net_setup(); for (int i = 0; i < 4; i++) reply(200, MISS);
    TEST_ASSERT(vjo_dict_lookup(&A, &P, &C, "猫\"\\\n", &r, &e) == VJO_OK);
    const char *body = strstr(F.sent[0], "\r\n\r\n");
    TEST_ASSERT(body); body += 4;
    TEST_CHECK(!strcmp(body, "{\"term\":\"猫\\\"\\\\\\n\"}"));
    char length[64]; snprintf(length, sizeof(length), "Content-Length: %lu\r\n", (unsigned long)strlen(body));
    TEST_CHECK(strstr(F.sent[0], length) != NULL);
}
static void test_relay_vita_budget(void)
{
    VjoDictResult r; VjoErr e;
    char sentence[361], json[3800], padding[3001];
    net_setup(); vjo_arena_init(&A, mem, 384u * 1024u);
    uint8_t *ocr = vjo_arena_alloc(&A, 100u * 1024u);
    memset(ocr, 0x5A, 100u * 1024u);
    memset(padding, 'x', 3000); padding[3000] = '\0';
    snprintf(json, sizeof(json), "{\"index\":0,\"originalTextLength\":1,\"padding\":\"%s\",\"dictionaryEntries\":[%s]}",
             padding, ENTRY("猫", "ねこ", "cat"));
    for (int i = 0; i < 120; i++) { memcpy(sentence + i * 3, "猫", 3); reply(200, json); }
    sentence[360] = '\0';
    TEST_ASSERT(vjo_dict_lookup(&A, &P, &C, sentence, &r, &e) == VJO_OK);
    TEST_CHECK(r.n_tokens == 120 && r.n_vocab == 1 && F.next == 120);
    TEST_CHECK(A.used < 132u * 1024u && A.peak < 384u * 1024u);
    TEST_CHECK(ocr[0] == 0x5A && ocr[100u * 1024u - 1] == 0x5A);
    TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "cat"));
    TEST_MSG("used=%lu peak=%lu", (unsigned long)A.used, (unsigned long)A.peak);
    net_setup(); vjo_arena_init(&A, mem, 4096); reply(200, json);
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_E_OOM);
    TEST_CHECK(A.used <= A.size && F.disconnected == 1 && r.n_vocab == 0 && r.n_tokens == 0);
}
static void test_relay_limits(void)
{
    VjoDictResult r; VjoErr e;
    char text[5000];
    net_setup(); memset(text, 'x', sizeof(text)-1); text[sizeof(text)-1] = '\0';
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, text, &r, &e) == VJO_E_TOO_LARGE && F.next == 0);
    text[1025] = '\0';
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, text, &r, &e) == VJO_E_TOO_LARGE && F.next == 0);
    snprintf(F.replies[F.n++], sizeof(F.replies[0]), "HTTP/1.1 200 OK\r\nContent-Length: 196609\r\n\r\n");
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_E_TOO_LARGE && F.disconnected == 1);
    net_setup();
    snprintf(F.replies[F.n++], sizeof(F.replies[0]), "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 2\r\n\r\n{}");
    TEST_CHECK(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_E_PARSE);
}
static void test_relay_block_glossary(void)
{
    VjoDictResult r; VjoErr e;
    net_setup(); reply(200, HIT(1,
        "{\"headwords\":[{\"term\":\"猫\",\"reading\":\"ねこ\"}],\"definitions\":[{\"entries\":["
        "{\"type\":\"structured-content\",\"content\":{\"tag\":\"ul\",\"content\":["
        "{\"tag\":\"li\",\"content\":\"cat\"},{\"tag\":\"li\",\"content\":\"feline\"}]}}]}],"
        "\"frequencies\":[{\"frequency\":12.5},{\"frequency\":0}]}"));
    TEST_ASSERT(vjo_dict_lookup(&A, &P, &C, "猫", &r, &e) == VJO_OK);
    TEST_ASSERT(r.vocab[0].n_meanings == 1);
    TEST_CHECK(!strcmp(r.vocab[0].meanings[0], "cat\nfeline"));
    TEST_CHECK(r.vocab[0].rank == VJO_NO_RANK);
}

TEST_LIST = {{"relay_config", test_relay_config}, {"relay_endpoint", test_relay_endpoint},
    {"relay_missing_host", test_relay_missing_host}, {"relay_scan", test_relay_scan},
    {"relay_glossary", test_relay_glossary}, {"relay_newline_positions", test_relay_newline_positions},
    {"relay_malformed", test_relay_malformed}, {"relay_status_errors", test_relay_status_errors},
    {"relay_empty_unmatched", test_relay_empty_unmatched}, {"relay_request_escaping", test_relay_request_escaping},
    {"relay_vita_budget", test_relay_vita_budget}, {"relay_limits", test_relay_limits},
    {"relay_block_glossary", test_relay_block_glossary}, {NULL, NULL}};
