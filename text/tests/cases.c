/* Shared host/ARM behavioral cases. Invented dialogue, no game data. */
#include "cases.h"
#include "../../core/text_scan.h"
#include "../../core/hook_profile.h"
#include <string.h>

static unsigned checks, failures;
static FILE *output;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(output, "FAIL line=%d %s\n", __LINE__, #condition); } } while (0)
static VjoTextSources sources;
static VjoTextCandidate choices[VJO_TEXT_CHOICES];
static uint8_t window[4096 + VJO_TEXT_BYTES];

static void codecs(void)
{
    char text[VJO_TEXT_BYTES];
    const char *expected = "猫と犬がいる";
    const uint8_t wide[] = {0x2B,0x73,0x68,0x30,0xAC,0x72,0x4C,0x30,0x44,0x30,0x8B,0x30,0,0};
    const uint8_t sjis[] = {0x94,0x4C,0x82,0xC6,0x8C,0xA2,0x82,0xAA,0x82,0xA2,0x82,0xE9,0};
    CHECK(vjo_text_decode(VJO_TEXT_UTF8, expected, strlen(expected), text, sizeof(text)) > 0 && !strcmp(text, expected));
    CHECK(vjo_text_decode(VJO_TEXT_UTF16LE, wide, sizeof(wide), text, sizeof(text)) > 0 && !strcmp(text, expected));
    CHECK(vjo_text_decode(VJO_TEXT_CP932, sjis, sizeof(sjis), text, sizeof(text)) > 0 && !strcmp(text, expected));
    CHECK(vjo_text_decode(VJO_TEXT_AUTO, expected, strlen(expected), text, sizeof(text)) > 0 && !strcmp(text, expected));
    CHECK(vjo_text_decode(VJO_TEXT_AUTO, sjis, sizeof(sjis), text, sizeof(text)) > 0 && !strcmp(text, expected));
    CHECK(vjo_text_decode(VJO_TEXT_AUTO, "menu", 4, text, sizeof(text)) == 0 && !text[0]);
    const uint8_t emoji[] = {0x3D,0xD8,0x00,0xDE};
    CHECK(vjo_text_decode(VJO_TEXT_UTF16LE, emoji, sizeof(emoji), text, sizeof(text)) == 4 && !strcmp(text, "😀"));
    const uint8_t bad_surrogate[] = {0x00,0xD8,0x41,0};
    CHECK(vjo_text_decode(VJO_TEXT_UTF16LE, bad_surrogate, sizeof(bad_surrogate), text, sizeof(text)) < 0);
    CHECK(vjo_text_decode(VJO_TEXT_UTF8, "\xC0\xAF", 2, text, sizeof(text)) < 0);
    CHECK(vjo_text_decode(VJO_TEXT_CP932, "\x81\x7F", 2, text, sizeof(text)) < 0);
    CHECK(vjo_text_decode(VJO_TEXT_UTF8, "a\1b", 3, text, sizeof(text)) < 0);
    CHECK(vjo_text_decode(VJO_TEXT_UTF8, expected, strlen(expected), text, 5) < 0);
    char oversized[VJO_TEXT_CODEPOINTS+2];
    memset(oversized, 'a', sizeof(oversized));
    CHECK(vjo_text_decode(VJO_TEXT_UTF8, oversized, sizeof(oversized), text, sizeof(text)) < 0);
}

static void matching(void)
{
    vjo_text_sources_init(&sources);
    CHECK(vjo_text_reference(&sources, "あいうえおかきくけこ") == 0);
    uint32_t id = vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 1, 0, "あいうえおかきくさし");
    CHECK(id != 0);
    CHECK(vjo_text_top(&sources, choices) == 1 && choices[0].score == 80);
    CHECK(vjo_text_auto_select(choices, 1) == id);
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 2, 0, "あいうえおかきくさし");
    CHECK(vjo_text_top(&sources, choices) == 2 && !vjo_text_auto_select(choices, 2));
    vjo_text_forget(&sources, choices[1].id);
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 1, 0, "あいうえおかきくさすせ");
    vjo_text_top(&sources, choices);
    CHECK(choices[0].score < 80 && !vjo_text_auto_select(choices, 1));
    vjo_text_reference(&sources, "「猫 と 犬 が い る！」Ａ");
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 1, 0, "猫と犬がいるA");
    vjo_text_top(&sources, choices);
    CHECK(choices[0].score == 100);
    vjo_text_reference(&sources, "猫です");
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 1, 0, "猫です");
    vjo_text_top(&sources, choices);
    CHECK(choices[0].score == 100 && !vjo_text_auto_select(choices, 1));
}

static void streams(void)
{
    vjo_text_sources_init(&sources);
    vjo_text_reference(&sources, "English menu");
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 1, 0, "English menu");
    uint32_t id = vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 2, 0, "日本語の台詞を読む");
    CHECK(vjo_text_top(&sources, choices) == 2 && choices[0].id == id && choices[1].score == 100);
    sources.selected = id;
    for (unsigned i = 0; i < 60; i++) {
        char line[128];
        snprintf(line, sizeof(line), "次の日本語の台詞を読む%u", i);
        vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, i+10, 0, line);
    }
    CHECK(vjo_text_find(&sources, id) != NULL);
    CHECK(vjo_text_top(&sources, choices) == VJO_TEXT_CHOICES);
    uint32_t same = vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 2, 0, "新しい台詞に切り替わる");
    CHECK(same == id && vjo_text_find(&sources, id)->updates == 2);
    unsigned sequence = sources.sequence;
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 2, 0, "新しい台詞に切り替わる");
    CHECK(sources.sequence == sequence);
    vjo_text_forget(&sources, id);
    CHECK(!sources.selected && !vjo_text_find(&sources, id));
}

static void scanner(void)
{
    const char *line = "今日は新しい台詞を読む";
    uint32_t base = 0x81000000u, address = base+4090;
    vjo_text_sources_init(&sources);
    vjo_text_reference(&sources, line);
    memset(window, 0, sizeof(window));
    memcpy(window+4090, line, strlen(line)+1);
    for (unsigned i = 0; i < 4; i++) window[i] = (uint8_t)(address >> (i*8));
    vjo_text_scan(&sources, window, sizeof(window), 0, 4096, base, 77);
    int found = 0;
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++) {
        const VjoTextCandidate *c = &sources.items[i];
        if (c->id && c->kind == VJO_TEXT_MEMORY && c->address == address && c->score == 100) found++;
    }
    CHECK(found == 1);
    vjo_text_scan(&sources, window, sizeof(window), 0, 4096, base, 77);
    found = 0;
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++) {
        const VjoTextCandidate *c = &sources.items[i];
        if (c->id && c->kind == VJO_TEXT_POINTER && c->address == base && !strcmp(c->text, line)) found++;
    }
    CHECK(found == 1);
    unsigned sequence = sources.sequence;
    vjo_text_scan(&sources, window+4094, sizeof(window)-4094, 2, sizeof(window)-4094, base+4094, 77);
    CHECK(sources.sequence == sequence); /* no suffix at the next page */
    vjo_text_sources_init(&sources);
    memset(window, 0xFF, sizeof(window));
    vjo_text_scan(&sources, window, sizeof(window), 0, 4096, base, 77);
    CHECK(vjo_text_top(&sources, choices) == 0);
}

static void profiles(void)
{
    VjoNativeProfile profile;
    const char *good = "VJOHOOK1 VJOHK0001 12345678\n0 20 1 4 2 1 -C 1122334455667788\n";
    CHECK(vjo_hook_profile_parse(&profile, good, strlen(good)) == 0);
    CHECK(profile.count == 1 && profile.module_nid == 0x12345678 &&
          profile.hooks[0].reg == 4 && profile.hooks[0].padding == -12);
    const char *zero_nid = "VJOHOOK1 VJOHK0001 0\n0 20 1 4 2 1 0 1122334455667788\n";
    CHECK(vjo_hook_profile_parse(&profile, zero_nid, strlen(zero_nid)) < 0);
    const char *short_signature = "VJOHOOK1 VJOHK0001 1\n0 20 1 4 2 1 0 11223344\n";
    CHECK(vjo_hook_profile_parse(&profile, short_signature, strlen(short_signature)) < 0);
    const char *bad_reg = "VJOHOOK1 VJOHK0001 1\n0 20 1 F 2 1 0 1122334455667788\n";
    CHECK(vjo_hook_profile_parse(&profile, bad_reg, strlen(bad_reg)) < 0);
    const char *bad_alignment = "VJOHOOK1 VJOHK0001 1\n0 22 0 4 2 1 0 1122334455667788\n";
    CHECK(vjo_hook_profile_parse(&profile, bad_alignment, strlen(bad_alignment)) < 0);
    const char *unaligned_thumb = "VJOHOOK1 VJOHK0001 1\n0 22 1 4 2 1 0 1122334455667788\n";
    CHECK(vjo_hook_profile_parse(&profile, unaligned_thumb, strlen(unaligned_thumb)) < 0);
}

int vjo_text_run_tests(FILE *report)
{
    output = report; checks = failures = 0;
    codecs(); matching(); streams(); scanner(); profiles();
    fprintf(report, "portable checks=%u failures=%u sources_bytes=%u cp932_bytes=22560\n",
            checks, failures, (unsigned)sizeof(sources));
    return (int)failures;
}
