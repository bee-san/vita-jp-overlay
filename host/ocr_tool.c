/* Diagnostic runner for the bounded local recognizers SceShell uses.
 * Input: tightly packed RGBA8 (no image-decoder dependency).
 *
 *   vjo-ocr [--backend ncnn|vocr] [--model FILE] MODEL_DIR WIDTH HEIGHT INPUT.rgba
 *
 * stderr gets one line, `rc=... lines=... tensor_heap_peak=...`: the peak bytes
 * used inside the job's workspace (ncnn: its tensor heap in the 64 MiB block;
 * vocr: the exact workspace, all of which the job uses), then backend details. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local_ocr.h"
#include "net_posix.h"
#ifdef VJO_WITH_VOCR
#include "vocr_ocr.h"
#endif

typedef struct { unsigned char *pixels; unsigned width, height; } Image;
static int rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    Image *im = ud;
    if (y > im->height || n > im->height-y) return -1;
    memcpy(dst, im->pixels+(size_t)y*im->width*4, (size_t)n*im->width*4);
    return (int)n;
}

/* This process's peak resident set since exec (Linux VmHWM), or 0. A parent's
 * wait4 ru_maxrss would also count the pages it forked from. */
static unsigned long peak_rss(void)
{
    char line[128];
    unsigned long kib = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "VmHWM: %lu kB", &kib) == 1) break;
    fclose(f);
    return kib * 1024;
}

static int usage(void)
{
    fprintf(stderr, "Usage: vjo-ocr [--backend ncnn|vocr] [--model FILE] MODEL_DIR WIDTH HEIGHT INPUT.rgba\n"
                    "backends built:"
#ifdef VJO_WITH_NCNN
                    " ncnn"
#endif
#ifdef VJO_WITH_VOCR
                    " vocr (--model default " VJO_VOCR_DEFAULT_MODEL ")"
#endif
                    "\n");
    return 2;
}

int main(int argc, char **argv)
{
#ifdef VJO_WITH_NCNN
    const char *backend = "ncnn";
#else
    const char *backend = "vocr";
#endif
    const char *model = NULL;
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); i += 2) {
        if (i + 1 >= argc) return usage();
        if (!strcmp(argv[i], "--backend")) backend = argv[i+1];
        else if (!strcmp(argv[i], "--model")) model = argv[i+1];
        else return usage();
    }
    if (argc - i != 4) return usage();
    Image im = {0};
    im.width = (unsigned)atoi(argv[i+1]); im.height = (unsigned)atoi(argv[i+2]);
    if (!im.width || im.width > VJO_OCR_MAX_WIDTH || !im.height || im.height > VJO_OCR_MAX_HEIGHT)
        return 2;
    size_t bytes = (size_t)im.width*im.height*4;
    im.pixels = malloc(bytes);
    FILE *f = fopen(argv[i+3], "rb");
    if (!im.pixels || !f || fread(im.pixels, 1, bytes, f) != bytes || fgetc(f) != EOF) {
        fprintf(stderr, "Invalid RGBA input\n"); return 2;
    }
    fclose(f);
    VjoPlatform platform;
    PosixPlatform pp = {0};
    posix_platform_init(&pp, &platform);
    VjoOcrImage input = {&im, im.width, im.height, rows, NULL};
    char text[VJO_OCR_TEXT_CAP];
    int rc;
    void *workspace = NULL;
    if (!strcmp(backend, "ncnn")) {
#ifdef VJO_WITH_NCNN
        VjoOcrStats stats;
        if (posix_memalign(&workspace, 64, VJO_OCR_HEAP_BYTES)) return 2;
        rc = vjo_local_ocr(&platform, argv[i], &input, workspace, VJO_OCR_HEAP_BYTES,
                           text, sizeof(text), &stats);
        fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu backend=ncnn workspace=%u\n", rc, stats.lines,
                stats.heap_peak, (unsigned)VJO_OCR_HEAP_BYTES);
#else
        return usage();
#endif
    } else if (!strcmp(backend, "vocr")) {
#ifdef VJO_WITH_VOCR
        VjoVocrStats st;
        if (!model) model = VJO_VOCR_DEFAULT_MODEL;
        size_t need = vjo_vocr_workspace_bytes(model, im.width, im.height);
        if (!need) {
            fprintf(stderr, "rc=%d lines=0 tensor_heap_peak=0 backend=vocr: unsupported model %s\n",
                    VJO_E_OCR_MODEL, model);
            return 1;
        }
        /* Exactly what the Shell allocates for this region, nothing more. */
        if (posix_memalign(&workspace, 64, need)) return 2;
        rc = vjo_vocr_ocr(&platform, argv[i], model, &input, workspace, need, text, sizeof(text), &st);
        fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu backend=vocr model=%s workspace=%zu "
                "weights=%zu scratch=%zu region=%zu dark=%u truncated=%u frames=%u loaded=%u "
                "find_us=%u load_us=%u recognize_us=%u\n",
                rc, st.lines, st.workspace, model, st.workspace, st.model_bytes, st.scratch_bytes,
                st.region_bytes, st.dark, st.truncated, st.frames, st.model_loaded,
                (unsigned)st.find_us, (unsigned)st.load_us, (unsigned)st.recognize_us);
#else
        return usage();
#endif
    } else {
        return usage();
    }
    fprintf(stderr, "peak_rss=%lu\n", peak_rss());
    if (!rc) puts(text);
    free(workspace); free(im.pixels);
    return rc ? 1 : 0;
}
