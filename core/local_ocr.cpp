#include "local_ocr.h"
#include "ocr_heap.h"
#include <net.h>
#include <datareader.h>
#include <bearssl_hash.h>
#include <string.h>
#include "vjo_ppocr_data.h"

static const unsigned char model_hash[32] = {
    0x49,0xd9,0x90,0x7a,0x55,0xba,0x20,0xfa,0x66,0x37,0xf9,0xf7,0x88,0xf6,0x6a,0xb0,
    0x07,0x93,0xbc,0x8c,0xe5,0x7a,0x73,0x3a,0x09,0xdb,0xe8,0x6b,0x9a,0x2e,0x3d,0xb0
};
static const size_t model_size = 8242276;

static int cancelled(const VjoOcrImage *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static int recognize(const VjoPlatform *platform, const char *dir, const VjoOcrImage *im,
                     const VjoOcrLines *lines, char *text, size_t cap)
{
    char path[256];
    const char *filename = "/PP_OCRv5_mobile_rec.ncnn.bin";
    size_t dn = strlen(dir), fn = strlen(filename);
    if (!dn || dn + fn >= sizeof(path)) return VJO_E_OCR_MODEL;
    memcpy(path, dir, dn); memcpy(path+dn, filename, fn+1);
    VjoFile file = {};
    if (!platform->file_open || platform->file_open(platform->ud, path, &file) < 0)
        return VJO_E_OCR_MODEL;
    if (file.size != model_size || !file.read || !file.close) {
        if (file.close) file.close(file.ctx);
        return VJO_E_OCR_MODEL;
    }
    unsigned char *weights = (unsigned char *)vjo_ncnn_malloc(model_size);
    if (!weights) { file.close(file.ctx); return VJO_E_OOM; }
    br_sha256_context hash; br_sha256_init(&hash);
    int rc = VJO_OK;
    for (size_t off = 0; off < model_size;) {
        size_t n = model_size - off; if (n > 32768) n = 32768;
        if (cancelled(im)) { rc = VJO_E_CANCELLED; break; }
        if (file.read(file.ctx, off, weights+off, n) < 0) { rc = VJO_E_OCR_MODEL; break; }
        br_sha256_update(&hash, weights+off, n); off += n;
    }
    file.close(file.ctx);
    if (rc) return rc;
    unsigned char digest[32]; br_sha256_out(&hash, digest);
    if (memcmp(digest, model_hash, sizeof(digest))) return VJO_E_OCR_MODEL;

    ncnn::Net net;
    net.opt.use_vulkan_compute = false;
    net.opt.num_threads = 1;
    net.opt.lightmode = true;
    net.opt.use_local_pool_allocator = false;
    net.opt.use_fp16_arithmetic = false;
    net.opt.use_fp16_storage = false;
    net.opt.use_fp16_packed = false;
    net.opt.use_bf16_storage = false;
    net.opt.use_winograd_convolution = false;
    net.opt.use_sgemm_convolution = false;
    const unsigned char *param = vjo_ppocr_param;
    ncnn::DataReaderFromMemory pr(param);
    const unsigned char *model = weights;
    ncnn::DataReaderFromMemory mr(model);
    if (net.load_param_bin(pr) || net.load_model(mr))
        return vjo_ocr_heap_failed() ? VJO_E_OOM : VJO_E_OCR_INFERENCE;
    unsigned char *row = (unsigned char *)vjo_ncnn_malloc(im->width * 4);
    unsigned char *pixels = (unsigned char *)vjo_ncnn_malloc(VJO_OCR_MAX_WIDTH * VJO_OCR_MAX_LINE_HEIGHT * 4);
    if (!row || !pixels) return VJO_E_OOM;
    size_t used = 0;
    for (unsigned l = 0; l < lines->count; l++) {
        if (cancelled(im)) return VJO_E_CANCELLED;
        const VjoOcrLine &line = lines->lines[l];
        for (unsigned y = 0; y < line.h; y++) {
            if (cancelled(im)) return VJO_E_CANCELLED;
            if (im->rows(im->ud, line.y+y, 1, row) != 1) return VJO_E_SOURCE;
            memcpy(pixels + y*line.w*4, row+line.x*4, line.w*4);
        }
        unsigned width = (line.w * 48 + line.h/2) / line.h;
        if (width < 8) width = 8;
        if (width > 1024) width = 1024; /* bound activations and the CTC matrix */
        ncnn::Mat input = ncnn::Mat::from_pixels_resize(pixels, ncnn::Mat::PIXEL_RGBA2BGR,
                                                       line.w, line.h, width, 48);
        if (input.empty()) return VJO_E_OOM;
        /* Direct normalization avoids ncnn's extra, dynamically created
         * Scale layer and keeps preprocessing allocation-free. */
        for (int c = 0; c < 3; c++) {
            float *p = input.channel(c);
            for (int i = 0; i < input.w * input.h; i++) p[i] = (p[i] - 127.5f) / 127.5f;
        }
        ncnn::Extractor ex = net.create_extractor();
        ncnn::Mat output;
        if (ex.input(vjo_ppocr_input, input) || ex.extract(vjo_ppocr_output, output))
            return vjo_ocr_heap_failed() ? VJO_E_OOM : VJO_E_OCR_INFERENCE;
        const unsigned vocab_count = sizeof(vjo_ppocr_vocab)/sizeof(vjo_ppocr_vocab[0]);
        if (output.dims != 2 || output.w != (int)vocab_count+1 || output.h <= 0 ||
            output.h > 1024 || output.elempack != 1 || output.elemsize != sizeof(float))
            return VJO_E_OCR_INFERENCE;
        if (l && used) {
            if (used+1 >= cap) return VJO_E_TOO_LARGE;
            text[used++] = '\n'; text[used] = 0;
        }
        unsigned last = 0;
        for (int y = 0; y < output.h; y++) {
            const float *scores = output.row(y);
            unsigned token = 0;
            for (unsigned i = 1; i <= vocab_count; i++)
                if (scores[i] > scores[token]) token = i;
            rc = vjo_ocr_ctc_append(token, &last, vjo_ppocr_vocab, vocab_count, text, cap, &used);
            if (rc) return rc;
        }
    }
    return VJO_OK;
}

extern "C" int vjo_local_ocr(const VjoPlatform *platform, const char *dir,
                             const VjoOcrImage *im, void *workspace, size_t size,
                             char *text, size_t cap, VjoOcrStats *stats)
{
    if (!platform || !dir || !im || !text || !cap || !stats) return VJO_E_SOURCE;
    text[0] = 0; memset(stats, 0, sizeof(*stats));
    VjoOcrLines lines;
    int rc = vjo_ocr_find_lines(im, &lines);
    if (rc || !lines.count) return rc;
    if (cancelled(im)) return VJO_E_CANCELLED;
    if (size < VJO_OCR_HEAP_BYTES || vjo_ocr_heap_begin(workspace, size) < 0) return VJO_E_OOM;
    rc = recognize(platform, dir, im, &lines, text, cap);
    stats->heap_peak = vjo_ocr_heap_end();
    stats->lines = lines.count;
    if (rc) text[0] = 0; /* never publish a partial/failed recognition */
    return rc;
}
