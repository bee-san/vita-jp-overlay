/* Live, bounded Lens-only comparison: the same JPEG over fresh and pooled
 * BearSSL connections. JSON contains hashes and counts, never recognized text. */
#include "client.h"
#include "net_posix.h"
#include <bearssl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARENA_BYTES (384u * 1024u)
#define MAX_IMAGE_BYTES (1024u * 1024u)

typedef struct Meter Meter;
typedef struct { Meter *meter; VjoConn raw; int open; } Socket;
struct Meter {
    PosixPlatform posix;
    VjoPlatform base;
    Socket sockets[2];
    unsigned connects, closes, send_calls;
    size_t sent_bytes, received_bytes, protobuf_bytes;
    int connect_ms, tls_ms, request_ms, kept, http_status;
};
typedef struct { const unsigned char *data; size_t bytes; } Image;

static int send_bytes(void *ctx, const void *data, size_t bytes)
{
    Socket *s = ctx;
    int rc=s->raw.send(s->raw.ctx,data,bytes);
    s->meter->send_calls++;
    if (rc > 0) s->meter->sent_bytes+=(size_t)rc;
    return rc;
}

static int receive_bytes(void *ctx, void *data, size_t bytes)
{
    Socket *s=ctx;
    int rc=s->raw.recv(s->raw.ctx,data,bytes);
    if (rc > 0) s->meter->received_bytes+=(size_t)rc;
    return rc;
}

static int connect_measured(void *ud, const char *host, int port, int timeout,
                             int io_timeout, VjoConn *out)
{
    Meter *m=ud;
    Socket *s=NULL;
    for (unsigned i=0; i<2; i++) if (!m->sockets[i].open) { s=&m->sockets[i]; break; }
    if (!s) return VJO_E_NET;
    m->connects++;
    int rc=m->base.connect(m->base.ud,host,port,timeout,io_timeout,&s->raw);
    if (rc) return rc;
    s->meter=m; s->open=1;
    *out=(VjoConn){.ctx=s,.send=send_bytes,.recv=receive_bytes};
    return VJO_OK;
}

static void disconnect_measured(void *ud, VjoConn *connection)
{
    Meter *m=ud;
    Socket *s=connection->ctx;
    if (s->open) {
        m->base.disconnect(m->base.ud,&s->raw);
        s->open=0; m->closes++;
    }
}

static void log_measured(void *ud, const char *line)
{
    Meter *m=ud;
    unsigned long width,height,image,protobuf;
    if (sscanf(line,"Lens image=%lux%lu jpeg=%lu protobuf=%lu",&width,&height,&image,&protobuf) == 4)
        m->protobuf_bytes=(size_t)protobuf;
    const char *http_status=strstr(line," status=");
    if (http_status) (void)sscanf(http_status," status=%d",&m->http_status);
    const char *phases=strchr(line,'(');
    if (phases) {
        if (sscanf(phases,"(connect %d ms, tls %d ms, request %d ms",&m->connect_ms,&m->tls_ms,&m->request_ms) == 3)
            m->kept=0;
        else if (sscanf(phases,"(kept connection, request %d ms",&m->request_ms) == 1)
            m->kept=1;
    }
}

static void reset_counts(Meter *m)
{
    m->connects=m->closes=m->send_calls=0;
    m->sent_bytes=m->received_bytes=m->protobuf_bytes=0;
    m->connect_ms=m->tls_ms=m->request_ms=m->kept=0;
    m->http_status=0;
}

static int image_read(void *ud, uint32_t offset, void *out, uint32_t bytes)
{
    Image *image=ud;
    if (offset > image->bytes || bytes > image->bytes-offset) return -1;
    memcpy(out,image->data+offset,bytes);
    return 0;
}

static void hash_hex(const void *data, size_t bytes, char out[65])
{
    static const char hex[]="0123456789abcdef";
    unsigned char digest[32];
    br_sha256_context context;
    br_sha256_init(&context); br_sha256_update(&context,data,bytes); br_sha256_out(&context,digest);
    for (unsigned i=0; i<32; i++) { out[i*2]=hex[digest[i]>>4]; out[i*2+1]=hex[digest[i]&15]; }
    out[64]=0;
}

static int number(const char *text, unsigned minimum, unsigned maximum, unsigned *out)
{
    char *end;
    errno=0;
    unsigned long value=strtoul(text,&end,10);
    if (errno || !text[0] || *end || value < minimum || value > maximum) return -1;
    *out=(unsigned)value;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned width,height,runs;
    if (argc != 5 || number(argv[2],1,4096,&width) || number(argv[3],1,4096,&height) ||
        number(argv[4],2,20,&runs)) {
        fprintf(stderr,"usage: vjo-lens-bench IMAGE.jpg WIDTH HEIGHT RUNS(2..20)\n");
        return 2;
    }
    FILE *file=fopen(argv[1],"rb");
    if (!file || fseek(file,0,SEEK_END)) { if (file) fclose(file); return 2; }
    long size=ftell(file);
    if (size < 4 || size > MAX_IMAGE_BYTES || fseek(file,0,SEEK_SET)) { fclose(file); return 2; }
    unsigned char *image=malloc((size_t)size), *memory=malloc(ARENA_BYTES);
    size_t pool_bytes=vjo_net_pool_size(1);
    void *pool_memory=malloc(pool_bytes);
    if (!image || !memory || !pool_memory || fread(image,1,(size_t)size,file) != (size_t)size) {
        fclose(file); free(image); free(memory); free(pool_memory); return 2;
    }
    fclose(file);
    if (image[0] != 0xff || image[1] != 0xd8) { free(image); free(memory); free(pool_memory); return 2; }
    Meter meter={0};
    VjoPlatform platform;
    posix_platform_init(&meter.posix,&meter.base);
    platform=meter.base; platform.ud=&meter;
    platform.connect=connect_measured; platform.disconnect=disconnect_measured;
    platform.log=log_measured; platform.on_response=NULL;
    VjoNetPool pool;
    vjo_net_pool_init(&pool,pool_memory,pool_bytes,60000000);
    Image source={image,(size_t)size};
    VjoJpegSource jpeg={.ud=&source,.width=width,.height=height,.size=(uint32_t)size,.read=image_read};
    char image_hash[65], reference_hash[65]={0};
    hash_hex(image,(size_t)size,image_hash);
    int status=0;
    /* Alternate pair order to reduce simple drift bias. The first pooled
     * sample is cold; all subsequent pooled samples must prove reuse. */
    for (unsigned run=0; run<runs && !status; run++) for (unsigned order=0; order<2 && !status; order++) {
        unsigned pooled=order^(run&1);
        platform.pool=pooled ? &pool : NULL;
        reset_counts(&meter);
        VjoArena arena;
        vjo_arena_init(&arena,memory,ARENA_BYTES);
        VjoLensResult result;
        VjoErr error;
        const char *text=NULL;
        uint64_t start=platform.now_us(platform.ud);
        int rc=vjo_lens_ocr(&arena,&platform,&jpeg,&result,&text,&error);
        double elapsed=(double)(platform.now_us(platform.ud)-start)/1000.0;
        char text_hash[65]={0};
        int same=0;
        size_t text_bytes=text ? strlen(text) : 0;
        if (!rc && text_bytes) {
            hash_hex(text,text_bytes,text_hash);
            if (!reference_hash[0]) memcpy(reference_hash,text_hash,sizeof(reference_hash));
            same=!strcmp(reference_hash,text_hash);
        }
        printf("{\"mode\":\"%s\",\"sample\":%u,\"rc\":%d,\"http_status\":%d,\"tls_error\":%d,"
               "\"width\":%u,\"height\":%u,\"jpeg_bytes\":%ld,\"jpeg_sha256\":\"%s\",\"protobuf_bytes\":%zu,"
               "\"elapsed_ms\":%.3f,\"connect_ms\":%d,\"tls_ms\":%d,\"request_ms\":%d,\"kept\":%d,"
               "\"new_connections\":%u,\"closes\":%u,\"raw_send_calls\":%u,\"tls_wire_sent_bytes\":%zu,"
               "\"tls_wire_received_bytes\":%zu,\"arena_peak_bytes\":%zu,\"pool_reserved_bytes\":%zu,"
               "\"text_bytes\":%zu,\"text_sha256\":\"%s\",\"same_text\":%d}\n",
               pooled ? "pooled" : "fresh",run+1,rc,meter.http_status,error.tls_error,
               width,height,size,image_hash,meter.protobuf_bytes,elapsed,meter.connect_ms,meter.tls_ms,
               meter.request_ms,meter.kept,meter.connects,meter.closes,meter.send_calls,meter.sent_bytes,
               meter.received_bytes,arena.peak,pooled ? pool_bytes : 0,text_bytes,text_hash,same);
        fflush(stdout);
        if (rc || !text_bytes || !same || (pooled && run && (meter.connects || !meter.kept))) status=1;
        struct timespec pause={0,500000000}; nanosleep(&pause,NULL);
    }
    platform.pool=&pool;
    vjo_net_pool_close(&platform);
    free(image); free(memory); free(pool_memory);
    return status;
}
