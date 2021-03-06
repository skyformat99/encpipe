#include "encpipe_p.h"

static struct option getopt_long_options[] = {
    { "help", 0, NULL, 'h' },    { "decrypt", 0, NULL, 'd' },
    { "encrypt", 0, NULL, 'e' }, { "in", 1, NULL, 'i' },
    { "out", 1, NULL, 'o' },     { "p", 1, NULL, 'p' },
    { NULL, 0, NULL, 0 }
};
static const char *getopt_options = "hdei:o:p:";

static void
usage(void)
{
    puts(
        "Usage:\n\n"
        "Encrypt: encpipe -e -p <password> [-i <inputfile>] [-o <outputfile>]\n"
        "Decrypt: encpipe -d -p <password> [-i <inputfile>] [-o <outputfile>]");
    exit(0);
}

static void
options_parse(Context *ctx, int argc, char *argv[])
{
    int opt_flag;
    int option_index = 0;

    ctx->encrypt  = -1;
    ctx->in       = NULL;
    ctx->out      = NULL;
    ctx->password = NULL;
    optind        = 0;
#ifdef _OPTRESET
    optreset = 1;
#endif
    while ((opt_flag = getopt_long(argc, argv, getopt_options,
                                   getopt_long_options, &option_index)) != -1) {
        switch (opt_flag) {
        case 'd':
            ctx->encrypt = 0;
            break;
        case 'e':
            ctx->encrypt = 1;
            break;
        case 'i':
            ctx->in = optarg;
            break;
        case 'o':
            ctx->out = optarg;
            break;
        case 'p':
            ctx->password = optarg;
            break;
        default:
            usage();
        }
    }
    if (ctx->password == NULL || ctx->encrypt == -1) {
        usage();
    }
}

static int
file_open(const char *file, int create)
{
    int fd;

    if (file == NULL || (file[0] == '-' && file[1] == 0)) {
        return create ? STDOUT_FILENO : STDIN_FILENO;
    }
    if (create) {
        fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    } else {
        fd = open(file, O_RDONLY);
    }
    if (fd == -1) {
        fprintf(stderr, "Unable to access [%s]: [%s]\n", file, strerror(errno));
        exit(1);
    }
    return fd;
}

static void
derive_key(Context *ctx)
{
    static uint8_t master_key[hydro_pwhash_MASTERKEYBYTES] = { 0 };
    size_t         password_len = strlen(ctx->password);

    if (hydro_pwhash_deterministic(ctx->key, sizeof ctx->key, ctx->password,
                                   password_len, HYDRO_CONTEXT, master_key,
                                   PWHASH_OPSLIMIT, PWHASH_MEMLIMIT,
                                   PWHASH_THREADS) != 0) {
        fprintf(stderr, "Password hashing failed\n");
        exit(1);
    }
    hydro_memzero(ctx->password, password_len);
}

static int
stream_encrypt(Context *ctx)
{
    unsigned char *const chunk_size_p = ctx->buf;
    unsigned char *const chunk        = chunk_size_p + 4;
    uint64_t             chunk_id;
    ssize_t              max_chunk_size;
    ssize_t              chunk_size;

    assert(ctx->sizeof_buf > 4 + hydro_secretbox_HEADERBYTES);
    max_chunk_size = ctx->sizeof_buf - 4 - hydro_secretbox_HEADERBYTES;
    assert(max_chunk_size < 0x7fffffff);
    chunk_id = 0;
    while ((chunk_size =
                safe_read_partial(ctx->fd_in, chunk, max_chunk_size)) >= 0) {
        STORE32_LE(chunk_size_p, (uint32_t) chunk_size);
        if (hydro_secretbox_encrypt(chunk, chunk, chunk_size, chunk_id,
                                    HYDRO_CONTEXT, ctx->key) != 0) {
            fprintf(stderr, "Encryption error\n");
            exit(1);
        }
        if (safe_write(ctx->fd_out, chunk_size_p,
                       4 + hydro_secretbox_HEADERBYTES + (size_t) chunk_size,
                       -1) < 0) {
            perror("write()");
            exit(1);
        }
        if (chunk_size == 0) {
            break;
        }
        chunk_id++;
    }
    if (chunk_size < 0) {
        perror("read()");
        exit(1);
    }
    return 0;
}

static int
stream_decrypt(Context *ctx)
{
    unsigned char *const chunk_size_p = ctx->buf;
    unsigned char *const chunk        = chunk_size_p + 4;
    uint64_t             chunk_id;
    ssize_t              readnb;
    ssize_t              max_chunk_size;
    ssize_t              chunk_size;

    assert(ctx->sizeof_buf > 4 + hydro_secretbox_HEADERBYTES);
    max_chunk_size = ctx->sizeof_buf - 4 - hydro_secretbox_HEADERBYTES;
    assert(max_chunk_size < 0x7fffffff);
    chunk_id = 0;
    while ((readnb = safe_read(ctx->fd_in, chunk_size_p, 4)) == 4) {
        chunk_size = LOAD32_LE(chunk_size_p);
        if (chunk_size > max_chunk_size) {
            fprintf(stderr, "Chunk size too large ([%zd] > [%zd])\n",
                    chunk_size, max_chunk_size);
            exit(1);
        }
        if (safe_read(ctx->fd_in, chunk,
                      (size_t) chunk_size + hydro_secretbox_HEADERBYTES) !=
            chunk_size + hydro_secretbox_HEADERBYTES) {
            fprintf(stderr, "Chunk too short ([%zd] bytes expected)\n",
                    chunk_size);
            exit(1);
        }
        if (hydro_secretbox_decrypt(chunk, chunk,
                                    chunk_size + hydro_secretbox_HEADERBYTES,
                                    chunk_id, HYDRO_CONTEXT, ctx->key) != 0) {
            fprintf(stderr, "Unable to decrypt chunk #%" PRIu64 " - ",
                    chunk_id);
            if (chunk_id == 0) {
                fprintf(stderr, "Wrong password or key?\n");
            } else {
                fprintf(stderr, "Corrupted or incomplete file?\n");
            }
            exit(1);
        }
        if (chunk_size == 0) {
            break;
        }
        if (safe_write(ctx->fd_out, chunk, chunk_size, -1) < 0) {
            perror("write()");
            exit(1);
        }
        chunk_id++;
    }
    if (readnb < 0) {
        perror("read()");
        exit(1);
    }
    if (chunk_size != 0) {
        fprintf(stderr, "Premature end of file\n");
        exit(1);
    }
    return 0;
}

int
main(int argc, char *argv[])
{
    Context ctx;

    if (hydro_init() < 0) {
        fprintf(stderr, "Unable to initialize the crypto library");
        exit(1);
    }
    memset(&ctx, 0, sizeof ctx);
    options_parse(&ctx, argc, argv);
    derive_key(&ctx);
    ctx.sizeof_buf = DEFAULT_BUFFER_SIZE;
    if (ctx.sizeof_buf < MIN_BUFFER_SIZE) {
        ctx.sizeof_buf = MIN_BUFFER_SIZE;
    } else if (ctx.sizeof_buf > MAX_BUFFER_SIZE) {
        ctx.sizeof_buf = MAX_BUFFER_SIZE;
    }
    if ((ctx.buf = malloc(ctx.sizeof_buf)) == NULL) {
        perror("malloc()");
        exit(1);
    }
    assert(sizeof HYDRO_CONTEXT == hydro_secretbox_CONTEXTBYTES);

    ctx.fd_in  = file_open(ctx.in, 0);
    ctx.fd_out = file_open(ctx.out, 1);
    if (ctx.encrypt) {
        stream_encrypt(&ctx);
    } else {
        stream_decrypt(&ctx);
    }
    free(ctx.buf);
    close(ctx.fd_out);
    close(ctx.fd_in);
    hydro_memzero(&ctx, sizeof ctx);

    return 0;
}
