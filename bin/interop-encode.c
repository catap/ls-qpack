/*
 * QPACK Interop -- encode to intermediate format
 *
 * https://github.com/quicwg/base-drafts/wiki/QPACK-Offline-Interop
 */

#include <assert.h>
#include <byteswap.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>

#include "lsqpack.h"
#include XXH_HEADER_NAME

static int s_verbose;

unsigned char *
lsqpack_enc_int (unsigned char *dst, unsigned char *const end, uint64_t value,
                                                        unsigned prefix_bits);

static void
usage (const char *name)
{
    fprintf(stderr,
"Usage: %s [options] [-i input] [-o output]\n"
"\n"
"Options:\n"
"   -i FILE     Input file.  If not specified or set to `-', the input is\n"
"                 read from stdin.\n"
"   -o FILE     Output file.  If not spepcified or set to `-', the output\n"
"                 is written to stdout.\n"
"   -s NUMBER   Maximum number of risked streams.  Defaults to %u.\n"
"   -t NUMBER   Dynamic table size.  Defaults to %u.\n"
"   -a MODE     Header acknowledgement mode.  0 means headers are never\n"
"                 acknowledged, non-zero means header blocks are acknowledged\n"
"                 immediately.  Default value is 0.\n"
"   -r FILE     Load Oracle from file.\n"
"   -n          Process annotations.\n"
"   -S          Server mode.\n"
"   -D          Do not emit \"Duplicate\" instructions.\n"
"   -A          Aggressive indexing.\n"
"   -v          Verbose: print various messages to stderr.\n"
"\n"
"   -h          Print this help screen and exit\n"
    , name, LSQPACK_DEF_MAX_RISKED_STREAMS, LSQPACK_DEF_DYN_TABLE_SIZE);
}


static unsigned s_oracle[0x1000];
static unsigned s_ora_nelem;

static int
load_oracle (const char *filename)
{
    FILE *file;
    char *line, *val;
    unsigned lineno;
    unsigned name_len, value_len, nameval_hash;
    XXH32_state_t hash_state;
    char line_buf[0x1000];

    file = fopen(filename, "r");
    if (!file)
        return -1;

    lineno = 0;
    while ((line = fgets(line_buf, sizeof(line_buf), file)))
    {
        ++lineno;
        if (s_ora_nelem >= sizeof(s_oracle) / sizeof(s_oracle[0]) - 1)
        {
            fprintf(stderr, "oracle out of room\n");
            return -1;
        }
        val = strchr(line, '\n');
        if (val)
            *val = '\0';
        val = strchr(line, '\t');
        if (!val)
        {
            fprintf(stderr, "No tab on %s:%u\n", filename, lineno);
            return -1;
        }
        name_len = val - line;
        ++val;
        value_len = strlen(val);

        XXH32_reset(&hash_state, 0);
        XXH32_update(&hash_state, &name_len, sizeof(name_len));
        XXH32_update(&hash_state, line, name_len);
        XXH32_update(&hash_state, &value_len, sizeof(value_len));
        XXH32_update(&hash_state, val, value_len);
        nameval_hash = XXH32_digest(&hash_state);
        s_oracle[ s_ora_nelem++ ] = nameval_hash;
    }

    fclose(file);
}


static void *
ora_hist_new (unsigned nelem)
{
    return (void *) 1;
}


static void
ora_hist_destroy (void *histp)
{
}


static void
ora_hist_add (void *histp, unsigned name_hash, unsigned nameval_hash)
{
}


static void
ora_hist_grow (void *histp)
{
}


static int
ora_hist_seen_nameval (void *histp, unsigned name_hash,
                                                    unsigned nameval_hash)
{
    unsigned *p;

    s_oracle[ s_ora_nelem ] = nameval_hash;
    for (p = s_oracle; *p != nameval_hash; ++p)
        ;

    return p != &s_oracle[ s_ora_nelem ];
}


static unsigned
ora_hist_size (void *histp)
{
    return 0;
}


static int
ora_hist_seen_name (void *histp, unsigned name_hash)
{
    return 0;
}


static const struct lsqpack_enc_hist_if oracle_hif =
{
    .hist_new           = ora_hist_new,
    .hist_grow          = ora_hist_grow,
    .hist_size          = ora_hist_size,
    .hist_add           = ora_hist_add,
    .hist_seen_nameval  = ora_hist_seen_nameval,
    .hist_seen_name     = ora_hist_seen_name,
    .hist_destroy       = ora_hist_destroy,
};

/* XXX For brevity, we assume that write(2) is successful. */
static void
write_enc_and_header_streams (int out, unsigned stream_id,
                              const unsigned char *enc_buf, size_t enc_sz,
                              const unsigned char *pref_buf, size_t pref_sz,
                              const unsigned char *hea_buf, size_t hea_sz)
{
    uint64_t stream_id_enc;
    uint32_t length_enc;

    if (enc_sz)
    {
        stream_id_enc = 0;
        length_enc = enc_sz;
#if __BYTE_ORDER == __LITTLE_ENDIAN
        stream_id_enc = bswap_64(stream_id_enc);
        length_enc = bswap_32(length_enc);
#endif
        write(out, &stream_id_enc, sizeof(stream_id_enc));
        write(out, &length_enc, sizeof(length_enc));
        write(out, enc_buf, enc_sz);
    }

    stream_id_enc = stream_id;
    length_enc = pref_sz + hea_sz;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    stream_id_enc = bswap_64(stream_id_enc);
    length_enc = bswap_32(length_enc);
#endif
    write(out, &stream_id_enc, sizeof(stream_id_enc));
    write(out, &length_enc, sizeof(length_enc));
    write(out, pref_buf, pref_sz);
    write(out, hea_buf, hea_sz);
}


static unsigned s_saved_ins_count;
static int
ack_last_entry_id (struct lsqpack_enc *encoder)
{
    unsigned char *end_cmd;
    unsigned char cmd[80];
    unsigned val;

    if (s_verbose)
        fprintf(stderr, "ACK entry ID %u\n", encoder->qpe_ins_count);

    cmd[0] = 0x00;
    val = encoder->qpe_ins_count - s_saved_ins_count;
    s_saved_ins_count = encoder->qpe_ins_count;
    end_cmd = lsqpack_enc_int(cmd, cmd + sizeof(cmd), val, 6);
    assert(end_cmd > cmd);
    return lsqpack_enc_decoder_in(encoder, cmd, end_cmd - cmd);
}


static int
ack_stream (struct lsqpack_enc *encoder, uint64_t stream_id)
{
    unsigned char *end_cmd;
    unsigned char cmd[80];

    if (s_verbose)
        fprintf(stderr, "ACK stream ID %"PRIu64"\n", stream_id);

    cmd[0] = 0x80;
    end_cmd = lsqpack_enc_int(cmd, cmd + sizeof(cmd), stream_id, 7);
    assert(end_cmd > cmd);
    return lsqpack_enc_decoder_in(encoder, cmd, end_cmd - cmd);
}


int
main (int argc, char **argv)
{
    FILE *in = stdin;
    int out = STDOUT_FILENO;
    int opt;
    unsigned dyn_table_size     = LSQPACK_DEF_DYN_TABLE_SIZE,
             max_risked_streams = LSQPACK_DEF_MAX_RISKED_STREAMS;
    unsigned lineno, stream_id;
    struct lsqpack_enc encoder;
    char *line, *end, *tab;
    ssize_t pref_sz;
    enum lsqpack_enc_status st;
    enum lsqpack_enc_opts enc_opts = LSQPACK_ENC_OPT_DUP;
    const struct lsqpack_enc_hist_if *hif = NULL;
    size_t enc_sz, hea_sz, enc_off, hea_off;
    int header_opened, r;
    unsigned arg;
    enum { ACK_NEVER, ACK_IMMEDIATE, } ack_mode = ACK_NEVER;
    int process_annotations = 0;
    char line_buf[0x1000];
    unsigned char enc_buf[0x1000], hea_buf[0x1000], pref_buf[0x20];

    while (-1 != (opt = getopt(argc, argv, "ADSa:i:no:r:s:t:hv")))
    {
        switch (opt)
        {
        case 'S':
            enc_opts |= LSQPACK_ENC_OPT_SERVER;
            break;
        case 'D':
            enc_opts &= ~LSQPACK_ENC_OPT_DUP;
            break;
        case 'A':
            hif = &lsqpack_enc_hist_aggr;
            break;
        case 'r':
            if (0 != load_oracle(optarg))
                exit(EXIT_FAILURE);
            hif = &oracle_hif;
            break;
        case 'n':
            ++process_annotations;
            break;
        case 'a':
            ack_mode = atoi(optarg) ? ACK_IMMEDIATE : ACK_NEVER;
            break;
        case 'i':
            if (0 != strcmp(optarg, "-"))
            {
                in = fopen(optarg, "r");
                if (!in)
                {
                    fprintf(stderr, "cannot open `%s' for reading: %s\n",
                                                optarg, strerror(errno));
                    exit(EXIT_FAILURE);
                }
            }
            break;
        case 'o':
            if (0 != strcmp(optarg, "-"))
            {
                out = open(optarg, O_WRONLY|O_CREAT|O_TRUNC, 0644);
                if (out < 0)
                {
                    fprintf(stderr, "cannot open `%s' for writing: %s\n",
                                                optarg, strerror(errno));
                    exit(EXIT_FAILURE);
                }
            }
            break;
        case 's':
            max_risked_streams = atoi(optarg);
            break;
        case 't':
            dyn_table_size = atoi(optarg);
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        case 'v':
            ++s_verbose;
            break;
        default:
            exit(EXIT_FAILURE);
        }
    }

    if (0 != lsqpack_enc_init(&encoder, dyn_table_size, max_risked_streams,
                                                                enc_opts, hif))
    {
        perror("lsqpack_enc_init");
        exit(EXIT_FAILURE);
    }

#if LSQPACK_DEVEL_MODE
    lsqpack_enc_log(&encoder, stderr);
#endif

    lineno = 0;
    stream_id = 0;
    enc_off = 0;
    hea_off = 0;
    header_opened = 0;

    while ((line = fgets(line_buf, sizeof(line_buf), in)))
    {
        ++lineno;
        end = strchr(line, '\n');
        if (!end)
        {
            fprintf(stderr, "no newline on line %u\n", lineno);
            exit(EXIT_FAILURE);
        }
        *end = '\0';

        if (end == line)
        {
            if (header_opened)
            {
                pref_sz = lsqpack_enc_end_header(&encoder, pref_buf,
                                                                sizeof(pref_buf));
                if (pref_sz < 0)
                {
                    fprintf(stderr, "end_header failed: %s", strerror(errno));
                    exit(EXIT_FAILURE);
                }
                if (ack_mode == ACK_IMMEDIATE)
                {
                    if (!(2 == pref_sz && pref_buf[0] == 0 && pref_buf[1] == 0))
                        r = ack_stream(&encoder, stream_id);
                    else if (encoder.qpe_ins_count > s_saved_ins_count)
                        r = ack_last_entry_id(&encoder);
                    else
                        r = 0;
                    if (r != 0)
                    {
                        fprintf(stderr, "acking stream %u failed: %s", stream_id,
                                                                    strerror(errno));
                        exit(EXIT_FAILURE);
                    }
                }
                if (s_verbose)
                    fprintf(stderr, "compression ratio: %.3f\n",
                        lsqpack_enc_ratio(&encoder));
                write_enc_and_header_streams(out, stream_id, enc_buf, enc_off,
                                             pref_buf, pref_sz, hea_buf, hea_off);
                enc_off = 0;
                hea_off = 0;
                header_opened = 0;
            }
            continue;
        }

        if (*line == '#')
        {
            if (!process_annotations)
                continue;

            /* Lines starting with ## are potential annotations */
            if (ack_mode != ACK_IMMEDIATE
                /* Ignore ACK annotations in immediate ACK mode, as we do
                 * not tolerate duplicate ACKs.
                 */
                                && 1 == sscanf(line, "## %*[a] %u ", &arg))
            {
                if (0 != ack_stream(&encoder, arg))
                {
                    fprintf(stderr, "ACKing stream ID %u failed\n", arg);
                    exit(EXIT_FAILURE);
                }
            }
            continue;
        }

        tab = strchr(line, '\t');
        if (!tab)
        {
            fprintf(stderr, "no TAB on line %u\n", lineno);
            exit(EXIT_FAILURE);
        }

        if (!header_opened)
        {
            ++stream_id;
            if (0 != lsqpack_enc_start_header(&encoder, stream_id, 0))
            {
                fprintf(stderr, "start_header failed: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
            header_opened = 1;
        }

        enc_sz = sizeof(enc_buf) - enc_off;
        hea_sz = sizeof(hea_buf) - hea_off;
        st = lsqpack_enc_encode(&encoder, enc_buf + enc_off, &enc_sz,
                    hea_buf + hea_off, &hea_sz, line, tab - line,
                    tab + 1, end - tab - 1, 0);
        if (st != LQES_OK)
        {
            /* It could only run of of output space, so it's not really an
             * error, but we make no provision in the interop encoder to
             * grow the buffers.
             */
            fprintf(stderr, "Could not encode header on line %u: %u\n",
                                                                lineno, st);
            exit(EXIT_FAILURE);
        }
        enc_off += enc_sz;
        hea_off += hea_sz;
    }

    (void) fclose(in);

    if (header_opened)
    {
        pref_sz = lsqpack_enc_end_header(&encoder, pref_buf, sizeof(pref_buf));
        if (pref_sz < 0)
        {
            fprintf(stderr, "end_header failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (ack_mode == ACK_IMMEDIATE
            && !(2 == pref_sz && pref_buf[0] == 0 && pref_buf[1] == 0)
            && 0 != ack_stream(&encoder, stream_id))
        {
            fprintf(stderr, "acking stream %u failed: %s", stream_id,
                                                                strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (s_verbose)
            fprintf(stderr, "compression ratio: %.3f\n",
                lsqpack_enc_ratio(&encoder));
        write_enc_and_header_streams(out, stream_id, enc_buf, enc_off, pref_buf,
                                     pref_sz, hea_buf, hea_off);
    }

    lsqpack_enc_cleanup(&encoder);

    exit(EXIT_SUCCESS);
}
