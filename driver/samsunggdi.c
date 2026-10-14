/*
 * samsunggdi.c — CUPS filter for Samsung SmartGDI printers
 * (ML-1000/ML-1200 series: ML-1010, ML-1020, ML-1210, ML-1220)
 *
 * Port of Samsung's gdevgdi.c Ghostscript device driver to a standalone
 * CUPS filter. Emits the Samsung "SMART" GDI stream: PJL wrapper plus
 * MODITIFF-compressed bands of 128 lines (600dpi) or 256 lines (300dpi).
 *
 * Accepted input (CONTENT_TYPE env, else sniffed):
 *   application/vnd.cups-raster  — cups raster v2/v3
 *   application/pdf, application/postscript — via ghostscript (pbmraw)
 *   image/x-portable-bitmap (pbmraw "P4") — direct
 *
 * Filter argv: job user title copies options [file]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cups/raster.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#define GDI_BAND_HEIGHT   128
#define GDI_MAX_BAND      66
#define GDI_COMP_MODITIFF 6
#define MAXBAND           0xffff

/* Band width in bits, indexed by paper type: 0=A4, 1=Letter/Legal */
static const int GDI_BAND_WIDTH[2] = {4768, 4928};

/* ---------- MODITIFF compressor (verbatim port from gdevgdi.c) ------- */

static int
frame_tiff_comp(unsigned char *dst, const unsigned char *src, unsigned int n)
{
    unsigned char *d = dst;
    const unsigned char *s = src, *org;
    unsigned int count = n, endcnt, cnt, control;
    unsigned char first, second, miscnt;

    for (;;) {
        if (!count)
            break;
        if (count == 1) {
            *d++ = 0x00;
            *d++ = *s++;
            break;
        }
        org = s;
        first = *s++;
        second = *s++;

        if (first == second) {
            endcnt = count - 2;
            while (endcnt--) {
                if (first != *s++) {
                    s--;
                    break;
                }
            }
            cnt = (unsigned int)(s - org);
            count -= cnt;
            cnt -= 2;
            if (cnt >= 64) {
                cnt = (~cnt & 0x3fff) | 0xc000;
                *d++ = (unsigned char)(cnt >> 8);
                *d++ = (unsigned char)(cnt & 0xff);
                *d++ = first;
            } else {
                *d++ = (unsigned char)(~cnt & 0x7f);
                *d++ = first;
            }
        } else {
            miscnt = 0;
            if (count > 2) {
                endcnt = count - 2;
                while (endcnt--) {
                    first = second;
                    second = *s++;
                    if (first == second) {
                        if (endcnt <= 1) {
                            miscnt = 2;
                            break;
                        } else {
                            second = *s++;
                            endcnt--;
                            if (first == second) {
                                miscnt = 3;
                                break;
                            }
                        }
                    }
                }
            }
            control = (unsigned int)(s - org) - miscnt;
            if (control > count)
                count = control;
            count -= control;
            cnt = control - 1;
            if (cnt >= 64) {
                cnt = (cnt & 0xbfff) | 0x8000;
                *d++ = (unsigned char)(cnt >> 8);
                *d++ = (unsigned char)(cnt & 0xff);
            } else {
                *d++ = (unsigned char)(cnt & 0x3f);
            }
            s = org;
            while (control--)
                *d++ = *s++;
        }
    }
    return (int)(d - dst);
}

static size_t
frame_tiff_comp_band(unsigned char *dst, const unsigned char *src,
                     unsigned int lines, unsigned int bytes_per_line)
{
    size_t total = 0;
    unsigned int i;

    for (i = 0; i < lines; i++)
        total += frame_tiff_comp(dst + total, src + i * bytes_per_line,
                                 bytes_per_line);
    while (total % 4)
        dst[total++] = 0;
    return total;
}

static unsigned char
fudge_byte(int fudge, unsigned char x)
{
    switch (fudge) {
    case 2:
        return (unsigned char)(((((x) >> 6 & 3) ? 3 : 0) << 6) |
                               ((((x) >> 4 & 3) ? 3 : 0) << 4) |
                               ((((x) >> 2 & 3) ? 3 : 0) << 2) |
                               ((((x)      & 3) ? 3 : 0)));
    case 3:
        return (unsigned char)((((x) >> 4 & 0xf) ? 0xf0 : 0) |
                               (((x)      & 0xf) ? 0x0f : 0));
    case 4:
        return x ? 0x0f : 0;
    default:
        return 0;
    }
}

/* ----------------------- GDI stream writer -------------------------- */

static void
write_pjl_header(int dpi, int paper_type, int legal, int copies)
{
    char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
                 "\033%%-12345X"
                 "@PJL SET PAPERTYPE = NORMAL ON\015\012"
                 "@PJL SET DENSITY = 1\015\012"
                 "@PJL SET TONERSAVE = OFF\015\012"
                 "@PJL ENTER LANGUAGE = SMART\015\012"
                 "$PJL JOB START\015\012"
                 "$PJL RESOLUTION = %d\015\012"
                 "$PJL COPIES = %d\015\012"
                 "$PJL PAGE %s AUTO\015\012"
                 "$PJL BITMAP START\015\012",
                 dpi, 1,
                 paper_type == 0 ? "A4" : legal ? "LEGAL" : "LETTER");
    fwrite(buf, 1, n, stdout);
}

static void
write_band_header(unsigned int band_no, unsigned char comp,
                  unsigned int width_bits, unsigned int height,
                  size_t data_size)
{
    unsigned char buf[12];
    size_t size = data_size + 8;

    buf[0] = (unsigned char)(size >> 24);
    buf[1] = (unsigned char)(size >> 16);
    buf[2] = (unsigned char)(size >> 8);
    buf[3] = (unsigned char)(size);
    buf[4] = (unsigned char)(band_no >> 8);
    buf[5] = (unsigned char)(band_no);
    buf[6] = comp;
    buf[7] = 0;
    buf[8] = (unsigned char)(height >> 8);
    buf[9] = (unsigned char)(height);
    buf[10] = (unsigned char)(width_bits >> 8);
    buf[11] = (unsigned char)(width_bits);
    fwrite(buf, 1, 12, stdout);
}

static void
write_trailer(void)
{
    fputs("$PJL PRINT 4\015\012"
          "$PJL EOJ\015\012"
          "$PJL SYNC\015\012"
          "$PJL RELEASE 0 2047\015\012"
          "$PJL GARBAGE\015\012"
          "\033%-12345X\015\012", stdout);
}

/* fill one output band row (band_width_bytes) from source line */
typedef int (*line_getter)(void *ctx, unsigned char *row,
                           unsigned int band_width_bytes);

static int
emit_page(int dpi, int paper_type, int legal, int copies,
          unsigned int num_lines, line_getter get, void *ctx)
{
    int band_width_bits, band_width_bytes, band_height, nbands, band;
    size_t band_size, comp_size;
    unsigned char *ibp, *obp, *fudge_buf, *use_band;

    if (dpi == 600) {
        band_width_bits = GDI_BAND_WIDTH[paper_type];
        band_height = GDI_BAND_HEIGHT;
    } else {
        band_width_bits = GDI_BAND_WIDTH[paper_type] / 2;
        band_height = GDI_BAND_HEIGHT * 2;
    }
    band_width_bytes = (band_width_bits + 31) / 32 * 4;
    band_size = (size_t)band_width_bytes * band_height;

    ibp = calloc(1, band_size);
    obp = malloc(band_size * 13 / 10 + 16);
    fudge_buf = malloc(band_size);
    if (!ibp || !obp || !fudge_buf) {
        fputs("samsunggdi: out of memory\n", stderr);
        return -1;
    }

    write_pjl_header(dpi, paper_type, legal, copies);

    nbands = (int)((num_lines + band_height - 1) / band_height);
    for (band = 0; band < nbands && band < GDI_MAX_BAND; band++) {
        unsigned int lines = num_lines - (unsigned)band * band_height;
        unsigned int y;

        if (lines > (unsigned)band_height)
            lines = band_height;
        memset(ibp, 0, band_size);
        for (y = 0; y < lines; y++) {
            if (get(ctx, ibp + (size_t)y * band_width_bytes,
                    band_width_bytes) <= 0)
                break;
        }

        use_band = ibp;
        {
            int fudge = 1;
            do {
                comp_size = frame_tiff_comp_band(obp, use_band, lines,
                                                 band_width_bytes);
                if (comp_size > MAXBAND - 8 && fudge < 4) {
                    size_t i;
                    fudge++;
                    for (i = 0; i < band_size; i++)
                        fudge_buf[i] = fudge_byte(fudge, ibp[i]);
                    use_band = fudge_buf;
                }
            } while (comp_size > MAXBAND - 8);
        }

        write_band_header(band, GDI_COMP_MODITIFF,
                          (unsigned)band_width_bytes * 8, lines, comp_size);
        fwrite(obp, 1, comp_size, stdout);
    }
    write_trailer();
    fflush(stdout);

    free(ibp);
    free(obp);
    free(fudge_buf);
    return 0;
}

/* -------------------- CUPS raster input ----------------------------- */

struct raster_ctx {
    cups_raster_t *ras;
    cups_page_header2_t h;
    unsigned char *linebuf;
    int invert;
};

static int
raster_getline(void *vctx, unsigned char *row, unsigned int bwb)
{
    struct raster_ctx *c = vctx;
    unsigned int copy = c->h.cupsBytesPerLine < bwb
                        ? c->h.cupsBytesPerLine : bwb;
    unsigned int i;

    if (!cupsRasterReadPixels(c->ras, c->linebuf, c->h.cupsBytesPerLine))
        return 0;
    memset(row, 0, bwb);
    if (c->h.cupsBitsPerColor == 1) {
        if (c->invert) {
            for (i = 0; i < copy; i++)
                row[i] = ~c->linebuf[i];
        } else {
            memcpy(row, c->linebuf, copy);
        }
    } else {
        unsigned char acc = 0;
        int nb = 0;
        for (i = 0; i < copy * 8 && i < c->h.cupsBytesPerLine; i++) {
            unsigned char v = c->linebuf[i];
            int black = c->invert ? v < 128 : v > 127;
            acc = (acc << 1) | (unsigned char)black;
            if (++nb == 8) {
                row[i / 8] = acc;
                acc = 0;
                nb = 0;
            }
        }
        if (nb)
            row[i / 8] = (unsigned char)(acc << (8 - nb));
    }
    return 1;
}

static int
do_raster(int fd, const char *options, int copies)
{
    struct raster_ctx c;
    int pages = 0, err = 0;

    c.ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!c.ras) {
        fputs("samsunggdi: cannot open raster stream\n", stderr);
        return 1;
    }
    while (cupsRasterReadHeader2(c.ras, &c.h)) {
        int dpi = (int)c.h.HWResolution[0];
        int paper_type, legal = 0;

        dpi = (dpi >= 450 || dpi == 0) ? 600 : 300;
        if (c.h.PageSize[0] > 560 && c.h.PageSize[0] < 601)
            paper_type = 0;
        else if (c.h.PageSize[0] >= 601)
            paper_type = 1;
        else {
            const char *media = strstr(options, "media=");
            paper_type = (media && strstr(media, "a4")) ? 0 : 1;
            legal = media && (strstr(media, "legal") != NULL);
        }
        if (c.h.PageSize[1] >= 990)
            legal = 1;
        c.invert = (c.h.cupsColorSpace == CUPS_CSPACE_W ||
                    c.h.cupsColorSpace == CUPS_CSPACE_RGB ||
                    c.h.cupsColorSpace == CUPS_CSPACE_RGBA ||
                    c.h.cupsColorSpace == CUPS_CSPACE_SW ||
                    c.h.cupsColorSpace == CUPS_CSPACE_SRGB ||
                    c.h.cupsColorSpace == CUPS_CSPACE_ADOBERGB);
        c.linebuf = malloc(c.h.cupsBytesPerLine ? c.h.cupsBytesPerLine : 1);
        if (!c.linebuf)
            return 1;
        if (emit_page(dpi, paper_type, legal, copies, c.h.cupsHeight,
                      raster_getline, &c))
            err = 1;
        free(c.linebuf);
        if (err)
            break;
        pages++;
    }
    cupsRasterClose(c.ras);
    if (!pages && !err) {
        fputs("samsunggdi: no pages in raster stream\n", stderr);
        return 1;
    }
    return err;
}

/* ------------------------- PBM input -------------------------------- */

struct pbm_ctx {
    FILE *fp;
    unsigned int bpl;
};

static int
pbm_getline(void *vctx, unsigned char *row, unsigned int bwb)
{
    struct pbm_ctx *c = vctx;
    unsigned char buf[8192];
    unsigned int copy = c->bpl < bwb ? c->bpl : bwb;

    if (c->bpl > sizeof(buf) ||
        fread(buf, 1, c->bpl, c->fp) != c->bpl)
        return 0;
    memset(row, 0, bwb);
    memcpy(row, buf, copy);
    return 1;
}

/* read "P4\n<w> <h>\n" (comments allowed); returns 1 if header ok */
static int
pbm_read_header(struct pbm_ctx *c, FILE *fp, unsigned *w, unsigned *h)
{
    int ch, val;
    unsigned *fields[2];
    int i = 0;

    fields[0] = w;
    fields[1] = h;
    while (i < 2) {
        /* skip whitespace and comments */
        do {
            ch = fgetc(fp);
            if (ch == '#') {
                while (ch != '\n' && ch != EOF)
                    ch = fgetc(fp);
            }
        } while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
        if (ch == EOF)
            return 0;
        val = 0;
        do {
            if (ch < '0' || ch > '9')
                return 0;
            val = val * 10 + ch - '0';
            ch = fgetc(fp);
        } while (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n');
        *fields[i++] = (unsigned)val;
    }
    return 1;
}

static int
do_pbm(FILE *fp, const char *options, int copies, int had_p4)
{
    struct pbm_ctx c;
    unsigned w, h;
    int pages = 0, err = 0;

    c.fp = fp;
    for (;;) {
        if (!had_p4) {
            int m1 = fgetc(fp), m2 = fgetc(fp);
            if (m1 != 'P' || m2 != '4')
                break;
        } else {
            had_p4 = 0;
        }
        if (!pbm_read_header(&c, fp, &w, &h))
            break;
        c.bpl = (w + 7) / 8;
        /* pbm raw: width px -> dpi/paper (Letter 5100, A4 4960 @600dpi;
           half at 300dpi). legal if very tall page. */
        {
            int dpi = w > 3000 ? 600 : 300;
            int paper_type = w > (dpi == 600 ? 5030 : 2515);
            int legal = h > (unsigned)(dpi == 600 ? 8000 : 4000);
            (void)options;
            if (emit_page(dpi, paper_type, legal, copies, h,
                          pbm_getline, &c))
                err = 1;
        }
        if (err)
            break;
        pages++;
    }
    if (!pages) {
        fputs("samsunggdi: no pbm pages\n", stderr);
        return 1;
    }
    return err;
}

/* -------------------- PDF via CoreGraphics -------------------------- */
/*
 * The CUPS filter sandbox (com.apple.printtool.daemon.sb) only permits
 * exec'ing binaries inside /usr/libexec/cups/filter, so rendering must
 * happen in-process. Render each PDF page into an 8-bit gray
 * CGContext at 600dpi, threshold to 1bpp on the fly.
 */

struct cg_ctx {
    CGContextRef bm;
    unsigned char *buf;   /* 8bpp gray, row = width bytes */
    unsigned int width, height;
    unsigned int next;    /* next line to hand out */
};

static int
cg_getline(void *vctx, unsigned char *row, unsigned int bwb)
{
    struct cg_ctx *c = vctx;
    unsigned int i, nbits = c->width < bwb * 8 ? c->width : bwb * 8;
    const unsigned char *s;

    if (c->next >= c->height)
        return 0;
    s = c->buf + (size_t)c->next * c->width;
    c->next++;
    memset(row, 0, bwb);
    for (i = 0; i < nbits; i++) {
        /* gray 0=black..255=white; threshold at midpoint */
        if (s[i] < 128)
            row[i >> 3] |= (unsigned char)(0x80 >> (i & 7));
    }
    return 1;
}

static int
do_pdf(int fd, const char *options, int copies)
{
    CGPDFDocumentRef doc;
    CGDataProviderRef prov;
    unsigned char *tmp;
    size_t cap = 0, len = 0;
    ssize_t r;
    int pages = 0, err = 0;
    (void)options;

    /* slurp the input */
    tmp = NULL;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 1 << 20;
            tmp = realloc(tmp, cap);
            if (!tmp)
                return 1;
        }
        r = read(fd, tmp + len, cap - len);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    if (!len)
        return 1;

    prov = CGDataProviderCreateWithData(NULL, tmp, len, NULL);
    if (!prov)
        return 1;
    doc = CGPDFDocumentCreateWithProvider(prov);
    CGDataProviderRelease(prov);
    if (!doc) {
        fputs("samsunggdi: CGPDFDocument open failed\n", stderr);
        return 1;
    }

    {
        size_t npages = CGPDFDocumentGetNumberOfPages(doc);
        size_t pno;

        for (pno = 1; pno <= npages; pno++) {
            CGPDFPageRef page = CGPDFDocumentGetPage(doc, pno);
            CGRect box;
            struct cg_ctx c;
            double wpt, hpt, scale = 600.0 / 72.0;
            int paper_type, legal;

            if (!page)
                continue;
            box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
            wpt = box.size.width;
            hpt = box.size.height;
            c.width = (unsigned)(wpt * scale + 0.5);
            c.height = (unsigned)(hpt * scale + 0.5);
            /* paper: 595x842=A4, 612x792=Letter, 612x1008=Legal */
            paper_type = (wpt > 560 && wpt < 601) ? 0 : 1;
            legal = hpt >= 990;

            c.buf = malloc((size_t)c.width * c.height);
            if (!c.buf) {
                err = 1;
                break;
            }
            memset(c.buf, 0xff, (size_t)c.width * c.height);

            {
                CGColorSpaceRef cs =
                    CGColorSpaceCreateDeviceGray();
                c.bm = CGBitmapContextCreate(
                    c.buf, c.width, c.height, 8, c.width, cs,
                    kCGImageAlphaNone);
                CGColorSpaceRelease(cs);
            }
            if (!c.bm) {
                free(c.buf);
                err = 1;
                break;
            }
            CGContextSetGrayFillColor(c.bm, 1.0, 1.0);
            CGContextFillRect(c.bm,
                CGRectMake(0, 0, c.width, c.height));
            CGContextScaleCTM(c.bm, scale, scale);
            CGContextDrawPDFPage(c.bm, page);
            c.next = 0;

            if (emit_page(600, paper_type, legal, copies, c.height,
                          cg_getline, &c))
                err = 1;
            CGContextRelease(c.bm);
            free(c.buf);
            if (err)
                break;
            pages++;
        }
    }
    CGPDFDocumentRelease(doc);
    free(tmp);
    if (!pages) {
        fputs("samsunggdi: no pages rendered\n", stderr);
        return 1;
    }
    return err;
}

/* ------------------------------ main -------------------------------- */

int
main(int argc, char *argv[])
{
    const char *ct;
    int fd = 0, copies;
    unsigned char magic[4];
    int n, rc;

    if (argc < 6 || argc > 7) {
        fputs("Usage: samsunggdi job user title copies options [file]\n",
              stderr);
        return 1;
    }
    copies = atoi(argv[4]);
    if (argc == 7) {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0) {
            perror("samsunggdi: open input");
            return 1;
        }
    }

    ct = getenv("CONTENT_TYPE");

    /* sniff first bytes */
    n = (int)read(fd, magic, 4);
    if (n < 4) {
        fputs("samsunggdi: empty input\n", stderr);
        return 1;
    }
    if (fd != 0)
        lseek(fd, 0, SEEK_SET);
    else {
        /* stdin: use a pipe replacement via temp spool */
        int tfd;
        char tmpl[] = "/tmp/samsunggdi-in-XXXXXX";
        tfd = mkstemp(tmpl);
        unlink(tmpl);
        write(tfd, magic, n);
        {
            char buf[8192];
            ssize_t r;
            while ((r = read(fd, buf, sizeof(buf))) > 0)
                write(tfd, buf, r);
        }
        lseek(tfd, 0, SEEK_SET);
        fd = tfd;
    }

    if ((ct && strstr(ct, "pdf")) ||
        (n >= 4 && magic[0] == '%' && magic[1] == 'P')) {
        rc = do_pdf(fd, argv[5], copies);
    } else if (magic[0] == 'P' && magic[1] == '4') {
        FILE *fp = fdopen(fd, "r");
        rc = do_pbm(fp, argv[5], copies, 1);
        fclose(fp);
    } else {
        rc = do_raster(fd, argv[5], copies);
    }
    if (fd != 0)
        close(fd);
    return rc;
}
