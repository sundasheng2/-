/* Terminal: output with ANSI colors over an input line. Output is parsed as
 * it is written: line breaks, carriage returns and tabs act right away, color
 * sequences are kept in the text and read again when a line is drawn, and
 * other unsupported controls are dropped. Each line keeps its count of
 * terminal cells; only wrapped lines with wide characters need extra layout. */
#include "cui_internal.h"
#include <stdarg.h>


enum { CUI__TM_TEXT = 0, CUI__TM_ESC, CUI__TM_CSI, CUI__TM_OSC, CUI__TM_OSC_ESC };

/* ------------------------------------------------------------------------ */
/* Buffer                                                                   */
/* ------------------------------------------------------------------------ */

static cui_terminal_line *cui__tm_cur(cui_terminal_buffer *t) { return &t->lines[(t->first + t->count - 1) % t->line_cap]; }
static cui_terminal_line *cui__tm_line(const cui_terminal_buffer *t, int i) { return &t->lines[(t->first + i) % t->line_cap]; }

void cui_terminal_init(cui_terminal_buffer *t, char *text, size_t text_cap, cui_terminal_line *lines, int line_cap) {
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->text = text; t->text_cap = (uint32_t)CUI_MIN(text_cap, (size_t)0x7FFFFFFF);
    t->lines = lines; t->line_cap = line_cap;
    if (!text || !lines || line_cap < 2 || t->text_cap < 64) { t->line_cap = 0; return; }
    t->count = 1; t->total = 1;
    lines[0] = (cui_terminal_line){ 0, 0, 0 };
}

void cui_terminal_clear(cui_terminal_buffer *t) {
    if (!t || !t->line_cap) return;
    t->first = 0; t->count = 1; t->end = 0; t->total++;
    t->lines[0] = (cui_terminal_line){ 0, 0, 0 };
    t->cr = false; t->mode = CUI__TM_TEXT; t->screen_row = 1; t->utf8_len = 0;
    t->caret_col = 0; t->cursor_hidden = false;
}

static void cui__tm_new_line(cui_terminal_buffer *t) {
    if (t->count == t->line_cap) { t->first = (t->first + 1) % t->line_cap; t->count--; }
    t->lines[(t->first + t->count) % t->line_cap] = (cui_terminal_line){ t->end, 0, 0 };
    t->count++; t->total++;
    t->caret_col = 0;
}

/* room for n more bytes at the end of the current line, which stays in one piece */
static bool cui__tm_room(cui_terminal_buffer *t, uint32_t n) {
    cui_terminal_line *c = cui__tm_cur(t);
    if (c->len + n > t->text_cap) return false;   /* longer than the whole buffer */
    if (c->offset + c->len + n > t->text_cap) {
        /* wrap: the oldest lines live at the start; make way, then move the current line there */
        uint32_t need = c->len + n;
        while (t->count > 1) {
            cui_terminal_line *o = cui__tm_line(t, 0);
            if (o->offset < need) { t->first = (t->first + 1) % t->line_cap; t->count--; }
            else break;
        }
        memmove(t->text, t->text + c->offset, c->len);
        c->offset = 0;
        t->end = c->len;
    }
    /* older lines in the way of the bytes about to be written go */
    while (t->count > 1) {
        cui_terminal_line *o = cui__tm_line(t, 0);
        if (o->offset >= t->end && o->offset < t->end + n) { t->first = (t->first + 1) % t->line_cap; t->count--; }
        else break;
    }
    return true;
}

static void cui__tm_put(cui_terminal_buffer *t, const char *b, uint32_t n, uint32_t cols) {
    if (!cui__tm_room(t, n)) return;
    memcpy(t->text + t->end, b, n);
    t->end += n;
    cui_terminal_line *c = cui__tm_cur(t);
    c->len += n; c->cols += cols;
    if (cols) t->caret_col = c->cols;
}

/* \r then output: the line starts over */
static void cui__tm_restart(cui_terminal_buffer *t) {
    cui_terminal_line *c = cui__tm_cur(t);
    t->end = c->offset; c->len = 0; c->cols = 0; c->has_wide = false;
    t->caret_col = 0;
    t->cr = false;
}

/* Common East Asian wide and fullwidth ranges. Ambiguous-width glyphs keep one cell. */
static uint32_t cui__tm_cells(uint32_t cp) {
    return cp >= 0x1100 &&
           (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
            (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
            (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
            (cp >= 0xfe10 && cp <= 0xfe19) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
            (cp >= 0xff01 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
            (cp >= 0x20000 && cp <= 0x3fffd)) ? 2 : 1;
}

static uint32_t cui__tm_glyph(const char *s, uint32_t len, uint32_t *bytes) {
    uint32_t cp;
    *bytes = (uint32_t)cui__utf8_decode(s, (int)len, &cp);
    return cui__tm_cells(cp);
}

static bool cui__tm_utf8(cui_terminal_buffer *t, unsigned char byte) {
    if (t->utf8_len > 0) {
        if ((byte & 0xc0) == 0x80) {
            t->utf8[t->utf8_len++] = (char)byte;
            if (t->utf8_len == t->utf8_size) {
                uint32_t bytes, cells = cui__tm_glyph(t->utf8, t->utf8_len, &bytes);
                cui__tm_put(t, t->utf8, bytes, cells);
                if (cells == 2) cui__tm_cur(t)->has_wide = true;
                t->utf8_len = 0;
            }
            return true;
        }
        /* A control or another lead byte ends an invalid partial character. */
        cui__tm_put(t, "\xef\xbf\xbd", 3, 1);
        t->utf8_len = 0;
    }
    if (byte < 0x80) return false;
    if (byte >= 0xc2 && byte <= 0xf4) {
        t->utf8_size = byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        t->utf8[0] = (char)byte;
        t->utf8_len = 1;
    } else {
        cui__tm_put(t, "\xef\xbf\xbd", 3, 1);
    }
    return true;
}

void cui_terminal_set_screen_rows(cui_terminal_buffer *t, int rows) {
    t->screen_rows = CUI_CLAMP(rows, 0, 4096);
    t->screen_row = 1;
}

static int cui__tm_csi_number(const char **cursor, const char *end) {
    int value = 0;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        value = CUI_MIN(4096, value * 10 + (**cursor - '0'));
        (*cursor)++;
    }
    return value > 0 ? value : 1;
}

static void cui__tm_cursor(cui_terminal_buffer *t, unsigned char final) {
    if (t->screen_rows == 0) return;
    if ((final == 'h' || final == 'l') && t->seq_n == 6 && memcmp(t->seq + 2, "?25", 3) == 0) {
        t->cursor_hidden = final == 'l';
        return;
    }
    if (final != 'H' && final != 'f' && final != 'C' && final != 'D' && final != 'G') return;
    const char *cursor = t->seq + 2;
    const char *end = t->seq + t->seq_n - 1;
    int first = cui__tm_csi_number(&cursor, end);
    int col = first;
    int row = t->screen_row;
    if (final == 'H' || final == 'f') {
        row = CUI_MIN(first, t->screen_rows);
        col = 1;
        if (cursor < end && *cursor == ';') {
            cursor++;
            col = cui__tm_csi_number(&cursor, end);
        }
    } else if (final == 'C') {
        col += (int)t->caret_col + 1;
    } else if (final == 'D') {
        col = CUI_MAX(1, (int)t->caret_col + 1 - first);
    }
    if (cursor != end) return;
    if (final == 'D') {
        t->caret_col = (uint32_t)(col - 1);
        return;
    }
    if (row != t->screen_row) {
        /* A transcript keeps earlier lines when the host redraws above the cursor. */
        int lines = row > t->screen_row ? row - t->screen_row : 1;
        while (lines-- > 0) cui__tm_new_line(t);
        t->screen_row = row;
        t->cr = false;
    }
    if (col == 1) {
        t->cr = cui__tm_cur(t)->len != 0;
        t->caret_col = 0;
        return;
    }
    if (t->cr) cui__tm_restart(t);
    int spaces = col - 1 - (int)cui__tm_cur(t)->cols;
    while (spaces > 0) {
        uint32_t count = (uint32_t)CUI_MIN(spaces, 8);
        cui__tm_put(t, "        ", count, count);
        spaces -= (int)count;
    }
    t->caret_col = (uint32_t)(col - 1);
}

void cui_terminal_write(cui_terminal_buffer *t, const char *data, int len) {
    if (!t || !t->line_cap || !data) return;
    if (len < 0) len = (int)strlen(data);
    int run = -1;   /* start of a run of plain text */
#define CUI__TM_FLUSH(upto) do { \
        if (run >= 0) { \
            uint32_t bytes_ = (uint32_t)((upto) - run); \
            cui__tm_put(t, data + run, bytes_, bytes_); run = -1; } } while (0)
    for (int i = 0; i < len; i++) {
        unsigned char b = (unsigned char)data[i];
        switch (t->mode) {
        case CUI__TM_TEXT:
            if (b >= 0x80 || t->utf8_len > 0) {
                CUI__TM_FLUSH(i);
                if (t->cr && b >= 0x80) cui__tm_restart(t);
                if (cui__tm_utf8(t, b)) break;
            }
            if (b == 0x1b) { CUI__TM_FLUSH(i); t->mode = CUI__TM_ESC; t->seq_n = 0; t->seq[t->seq_n++] = (char)b; }
            else if (b == '\n') {
                CUI__TM_FLUSH(i); t->cr = false; cui__tm_new_line(t);
                if (t->screen_row < t->screen_rows) t->screen_row++;
            }
            else if (b == '\r') { CUI__TM_FLUSH(i); t->cr = true; t->caret_col = 0; }
            else if (b == '\t') {
                CUI__TM_FLUSH(i);
                if (t->cr) cui__tm_restart(t);
                uint32_t sp = 8 - cui__tm_cur(t)->cols % 8;
                cui__tm_put(t, "        ", sp, sp);
            } else if (b == '\b') {
                CUI__TM_FLUSH(i);
                cui_terminal_line *c = cui__tm_cur(t);
                if (c->len > 0 && c->len == c->cols) {
                    c->len--; c->cols--; t->end--;
                    t->caret_col = c->cols;
                }
            } else if (b < 0x20 || b == 0x7f) { CUI__TM_FLUSH(i); }   /* other controls: dropped */
            else {
                if (t->cr) { CUI__TM_FLUSH(i); cui__tm_restart(t); }
                if (run < 0) run = i;
            }
            break;
        case CUI__TM_ESC:
            if (b == '[') { t->mode = CUI__TM_CSI; if (t->seq_n < sizeof t->seq) t->seq[t->seq_n++] = (char)b; }
            else if (b == ']') t->mode = CUI__TM_OSC;
            else t->mode = CUI__TM_TEXT;   /* a two byte escape: dropped */
            break;
        case CUI__TM_CSI:
            if (t->seq_n < sizeof t->seq) t->seq[t->seq_n++] = (char)b;
            if (b >= 0x40 && b <= 0x7e) {
                t->mode = CUI__TM_TEXT;
                if (b == 'm' && t->seq[t->seq_n - 1] == 'm') {
                    if (t->cr) cui__tm_restart(t);
                    cui__tm_put(t, t->seq, t->seq_n, 0);
                } else if (b == 'K' && t->cr) cui__tm_restart(t);
                else if (b == 'J' && memchr(t->seq, '2', t->seq_n)) {
                    bool cursor_hidden = t->cursor_hidden;
                    cui_terminal_clear(t);
                    t->cursor_hidden = cursor_hidden;
                }
                else cui__tm_cursor(t, b);
            }
            break;
        case CUI__TM_OSC:
            if (b == 0x07) t->mode = CUI__TM_TEXT;
            else if (b == 0x1b) t->mode = CUI__TM_OSC_ESC;
            break;
        case CUI__TM_OSC_ESC:
            t->mode = b == '\\' ? CUI__TM_TEXT : CUI__TM_OSC;
            break;
        }
    }
    CUI__TM_FLUSH(len);
#undef CUI__TM_FLUSH
}

void cui_terminal_printf(cui_terminal_buffer *t, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) cui_terminal_write(t, buf, CUI_MIN(n, (int)sizeof buf - 1));
}

/* ------------------------------------------------------------------------ */
/* Colors                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct { cui_color fg, bg; bool has_fg, has_bg, bold, dim, italic, underline, inverse; } cui__tm_style;

static cui_color cui__tm_foreground(cui_color fg, cui_color bg, bool dim) {
    if (!bg.a) return dim ? cui_alpha(fg, 0.6f) : fg;
    if (dim) fg = cui_mix(bg, fg, 0.6f);
    /* ANSI colors that read well on the theme surface can clash with each other. */
    return cui__legible(fg, bg, 4.5f);
}

static cui_color cui__tm_palette(cui_ctx *ui, int k) {
    /* tuned per mode so every color reads on the panel */
    static const uint32_t light[16] = { 0x27272a, 0xc4302b, 0x2e7d32, 0x9a6700, 0x1f5fbf, 0x8e3fb0, 0x0f7c84, 0x71717a,
                                        0x52525b, 0xe5484d, 0x30a46c, 0xb08300, 0x3e7bfa, 0xab4aba, 0x12a4ad, 0x18181b };
    static const uint32_t dark[16] = { 0x52525b, 0xff6b6b, 0x5bd18b, 0xf5c451, 0x6aa8ff, 0xd38cf2, 0x4fd1d9, 0xd4d4d8,
                                       0x71717a, 0xff8f8f, 0x7ee2a8, 0xffd97a, 0x93c0ff, 0xe2adf7, 0x7fe3e9, 0xfafafa };
    uint32_t v = (ui->mode == CUI_THEME_DARK ? dark : light)[k & 15];
    return cui__rgba((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}

static cui_color cui__tm_256(cui_ctx *ui, int n) {
    if (n < 16) return cui__tm_palette(ui, n);
    if (n < 232) {
        n -= 16;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return cui__rgba(lv[n / 36], lv[n / 6 % 6], lv[n % 6], 255);
    }
    int g = 8 + (n - 232) * 10;
    return cui__rgba(g, g, g, 255);
}

/* applies "ESC [ p ; p ... m" to st */
static void cui__tm_sgr(cui_ctx *ui, cui__tm_style *st, const char *s, int n) {
    int p[16], np = 0, v = -1;
    for (int k = 2; k < n - 1 && np < 16; k++) {
        char c = s[k];
        if (c >= '0' && c <= '9') v = (v < 0 ? 0 : v) * 10 + (c - '0');
        else if (c == ';' || c == ':') { p[np++] = v < 0 ? 0 : v; v = -1; }
    }
    if (np < 16) p[np++] = v < 0 ? 0 : v;
    for (int k = 0; k < np; k++) {
        int c = p[k];
        if (c == 0) { memset(st, 0, sizeof *st); }
        else if (c == 1) st->bold = true;
        else if (c == 2) st->dim = true;
        else if (c == 3) st->italic = true;
        else if (c == 4) st->underline = true;
        else if (c == 7) st->inverse = true;
        else if (c == 22) st->bold = st->dim = false;
        else if (c == 23) st->italic = false;
        else if (c == 24) st->underline = false;
        else if (c == 27) st->inverse = false;
        else if (c >= 30 && c <= 37) { st->fg = cui__tm_palette(ui, c - 30); st->has_fg = true; }
        else if (c >= 90 && c <= 97) { st->fg = cui__tm_palette(ui, c - 90 + 8); st->has_fg = true; }
        else if (c >= 40 && c <= 47) { st->bg = cui__tm_palette(ui, c - 40); st->has_bg = true; }
        else if (c >= 100 && c <= 107) { st->bg = cui__tm_palette(ui, c - 100 + 8); st->has_bg = true; }
        else if (c == 39) st->has_fg = false;
        else if (c == 49) st->has_bg = false;
        else if ((c == 38 || c == 48) && k + 1 < np) {
            cui_color col;
            if (p[k + 1] == 5 && k + 2 < np) { col = cui__tm_256(ui, p[k + 2] & 255); k += 2; }
            else if (p[k + 1] == 2 && k + 4 < np) { col = cui__rgba(p[k + 2] & 255, p[k + 3] & 255, p[k + 4] & 255, 255); k += 4; }
            else continue;
            if (c == 38) { st->fg = col; st->has_fg = true; } else { st->bg = col; st->has_bg = true; }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* View                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct { uint64_t line; uint32_t col; } cui__tm_pos;   /* line number (total based) and visible column */

typedef struct {
    double top;                    /* first row in view */
    bool follow, init, selecting, has_sel, raw_input;
    cui__tm_pos a, b;              /* selection: anchor and moving end */
    char input[1024];
    char hist[32][256]; int hn, hpos; bool browsing;
    float press_x, press_y;
} cui__tm_state;

typedef struct {
    const cui_terminal_buffer *t;   /* read during the paint only (the lines in view are unchanged until then) */
    double top; int cols; float cw, lh, size;
    bool has_sel, show_caret; cui__tm_pos s0, s1;
    cui_color fg, sel, bar;
    float bar_t; double rows_total;
    cui_id wheel;
} cui__tm_paint;

static uint32_t cui__tm_skip_escape(const char *s, uint32_t i, uint32_t len) {
    uint32_t j = i + 2;
    while (j < len && ((unsigned char)s[j] < 0x40 || (unsigned char)s[j] > 0x7e)) j++;
    return j < len ? j + 1 : len;
}

/* Convert between a position in the line and a position in its wrapped grid. */
static uint32_t cui__tm_column(const cui_terminal_buffer *t, const cui_terminal_line *line,
                               uint32_t width, uint32_t target, bool to_grid) {
    if (!line->has_wide || line->cols <= width) {
        return to_grid ? target : CUI_MIN(target, line->cols);
    }
    const char *s = t->text + line->offset;
    uint32_t cell = 0, grid = 0;
    for (uint32_t i = 0; i < line->len;) {
        if (s[i] == '\x1b') {
            i = cui__tm_skip_escape(s, i, line->len);
            continue;
        }
        uint32_t bytes;
        uint32_t span = cui__tm_glyph(s + i, line->len - i, &bytes);
        if (span == 2 && grid % width == width - 1) grid++;
        if (to_grid && target < cell + span) return grid + target - cell;
        if (!to_grid && target < grid + span) return cell + (target > grid ? target - grid : 0);
        cell += span;
        grid += span;
        i += bytes;
    }
    return to_grid ? grid + target - cell : cell;
}

static int cui__tm_rows(const cui_terminal_buffer *t, int li, int width_cols) {
    const cui_terminal_line *line = cui__tm_line(t, li);
    uint32_t width = (uint32_t)width_cols;
    uint32_t cols = cui__tm_column(t, line, width, line->cols, true);
    return cols == 0 ? 1 : (int)((cols + width - 1) / width);
}

/* block elements U+2580..U+2593: half and eighth blocks, and the three shades */
static void cui__tm_block(cui_ctx *ui, int k, cui_rect c, cui_color col) {
    float s = ui->scale > 0 ? ui->scale : 1;
    /* whole device pixels, so neighbouring cells meet without seams */
    float x0 = floorf(c.x * s + 0.5f) / s, x1 = floorf((c.x + c.w) * s + 0.5f) / s;
    float y0 = floorf(c.y * s + 0.5f) / s, y1 = floorf((c.y + c.h) * s + 0.5f) / s;
    float w = x1 - x0, h = y1 - y0;
    cui_rect r = { x0, y0, w, h };
    if (k == 0) r.h = floorf(h * 0.5f * s) / s;                                       /* upper half */
    else if (k >= 1 && k <= 8) { float f = k / 8.0f; r.y = y1 - floorf(h * f * s + 0.5f) / s; r.h = y1 - r.y; }   /* lower eighths up to full */
    else if (k >= 9 && k <= 15) r.w = floorf(w * (16 - k) / 8.0f * s + 0.5f) / s;         /* left eighths */
    else if (k == 16) { r.x = x0 + floorf(w * 0.5f * s) / s; r.w = x1 - r.x; }           /* right half */
    else if (k >= 17 && k <= 19) col = cui_alpha(col, (k - 16) * 0.25f);                /* light, medium, dark shade */
    cui_draw_rect(ui, r, CUI_CORNERS(0), col);
}

static void cui__paint_terminal(cui_ctx *ui, cui_node *n, void *data) {
    cui__tm_paint *p = data;
    cui_rect r = n->rect;
    cui__paint_wheel_hit(ui, p->wheel, r);
    const cui_terminal_buffer *t = p->t;
    if (!t || !t->line_cap) return;
    float pad = 10, x0 = r.x + pad, y0 = r.y + 6;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    /* find the line holding the first row in view */
    double row = 0;
    int li = 0;
    long long want = (long long)floor(p->top);
    while (li < t->count) {
        int rr = cui__tm_rows(t, li, p->cols);
        if (row + rr > want) break;
        row += rr; li++;
    }
    float y = y0 + (float)(row - p->top) * p->lh;
    /* rows show whole: one cut by the top edge is left out, as a terminal does */
    float first_y = y0 + (float)(floor(p->top) - p->top) * p->lh;
    float clip_top = first_y < y0 - 0.5f ? first_y + p->lh : r.y + 3;
    cui_push_clip(ui, (cui_rect){ r.x, clip_top, r.w, r.y + r.h - 3 - clip_top });
    uint64_t first_no = t->total - (uint64_t)t->count;
    cui__tm_pos s0 = p->s0, s1 = p->s1;
    for (; li < t->count && y < r.y + r.h; li++) {
        const cui_terminal_line *L = cui__tm_line(t, li);
        uint64_t no = first_no + (uint64_t)li;
        int nrows = cui__tm_rows(t, li, p->cols);
        /* selection on this line, by visible column */
        if (p->has_sel && no >= s0.line && no <= s1.line) {
            uint32_t c0 = no == s0.line ? s0.col : 0, c1 = no == s1.line ? s1.col : L->cols + 1;
            c0 = cui__tm_column(t, L, (uint32_t)p->cols, c0, true);
            c1 = cui__tm_column(t, L, (uint32_t)p->cols, c1, true);
            for (uint32_t c = c0; c < c1;) {
                uint32_t rowi = c / (uint32_t)p->cols, end = CUI_MIN(c1, (rowi + 1) * (uint32_t)p->cols);
                float sy = y + rowi * p->lh;
                cui_draw_rect(ui, (cui_rect){ x0 + (c % (uint32_t)p->cols) * p->cw, sy, (end - c) * p->cw, p->lh }, CUI_CORNERS(0), p->sel);
                c = end;
            }
        }
        /* the text, run by run, wrapping at the column count */
        cui__tm_style st;
        memset(&st, 0, sizeof st);
        const char *s = t->text + L->offset;
        uint32_t i = 0, col = 0;
        while (i < L->len) {
            if ((unsigned char)s[i] == 0x1b) {
                uint32_t next = cui__tm_skip_escape(s, i, L->len);
                cui__tm_sgr(ui, &st, s + i, (int)(next - i));
                i = next;
                continue;
            }
            /* plain text up to the next escape or the end of the row */
            uint32_t width = (uint32_t)p->cols, bytes;
            if (col % width == width - 1 && cui__tm_glyph(s + i, L->len - i, &bytes) == 2) col++;
            uint32_t j = i, c = col, room = width - col % width;
            uint32_t taken = 0;
            while (j < L->len && (unsigned char)s[j] != 0x1b) {
                uint32_t span = cui__tm_glyph(s + j, L->len - j, &bytes);
                if (span > room - taken) break;
                taken += span;
                j += bytes;
            }
            cui_color fg = st.has_fg ? st.fg : p->fg, bg = st.has_bg ? st.bg : cui__rgba(0, 0, 0, 0);
            if (st.inverse) { cui_color tmp = fg; fg = st.has_bg ? st.bg : CUI_C(ui, SURFACE); bg = tmp; }
            fg = cui__tm_foreground(fg, bg, st.dim);
            float rx = x0 + (c % (uint32_t)p->cols) * p->cw, ry = y + (c / (uint32_t)p->cols) * p->lh;
            if (ry > r.y + r.h) break;
            if (bg.a) cui_draw_rect(ui, (cui_rect){ rx, ry, taken * p->cw, p->lh }, CUI_CORNERS(0), bg);
            if (ry + p->lh >= r.y) {
                /* the grid: each character at its cell, so a font that is not quite monospaced still lines up.
                 * A run with right-to-left script is drawn whole instead, so it is shaped and put in order. */
                uint32_t k = i;
                float cx = rx;
                bool rtl = false;
                for (uint32_t q = i; q < j && !rtl; q++) {
                    unsigned char ch = (unsigned char)s[q];
                    rtl = (ch >= 0xD6 && ch <= 0xDF) || (ch == 0xEF && q + 1 < j && (unsigned char)s[q + 1] >= 0xAC && (unsigned char)s[q + 1] <= 0xBB);
                }
                if (rtl) {
                    uint32_t q = i;
                    while (q < j && s[q] == ' ') q++;   /* spaces before it keep their cells; bidi would move them to the far end */
                    cui__draw_text_line(ui, CUI_FONT_MONO, p->size, rx + (q - i) * p->cw, ry + (p->lh - fh) * 0.5f, fg, s + q, (int)(j - q), true);
                    k = j;
                }
                while (k < j) {
                    uint32_t span = cui__tm_glyph(s + k, j - k, &bytes);
                    uint32_t e = k + bytes;
                    unsigned char b0 = (unsigned char)s[k];
                    if (e - k == 3 && b0 == 0xE2 && (unsigned char)s[k + 1] == 0x96 && (unsigned char)s[k + 2] >= 0x80 && (unsigned char)s[k + 2] <= 0x93) {
                        cui__tm_block(ui, (unsigned char)s[k + 2] - 0x80, (cui_rect){ cx, ry, p->cw, p->lh }, fg);   /* fonts rarely have these; drawn so bars join */
                    } else if (s[k] != ' ') {
                        cui__draw_text_line(ui, st.bold ? CUI_FONT_MONO : CUI_FONT_MONO, p->size, cx, ry + (p->lh - fh) * 0.5f, fg, s + k, (int)(e - k), true);
                        if (st.bold) cui__draw_text_line(ui, CUI_FONT_MONO, p->size, cx + 0.45f, ry + (p->lh - fh) * 0.5f, fg, s + k, (int)(e - k), true);
                    }
                    k = e; cx += span * p->cw;
                }
                if (st.underline) cui_draw_rect(ui, (cui_rect){ rx, ry + p->lh - 3, taken * p->cw, 1 }, CUI_CORNERS(0), fg);
            }
            col += taken; i = j;
        }
        if (p->show_caret && li == t->count - 1) {
            uint32_t width = (uint32_t)p->cols;
            uint32_t column = cui__tm_column(t, L, width, CUI_MIN(t->caret_col, L->cols), true);
            uint32_t row_offset = CUI_MIN(column / width, (uint32_t)(nrows - 1));
            float cx = x0 + (column - row_offset * width) * p->cw;
            float cy = y + row_offset * p->lh;
            cui_draw_rect(ui, (cui_rect){ cx, cy + 3, 2, p->lh - 6 }, CUI_CORNERS(0), p->fg);
        }
        y += nrows * p->lh;
    }
    cui_pop_clip(ui);
    /* a thin scrollbar while there is more above */
    if (p->bar_t > 0.01f && p->rows_total * p->lh > r.h) {
        float vh = r.h, th = CUI_MAX(24.0f, vh * vh / (float)(p->rows_total * p->lh));
        float maxtop = (float)(p->rows_total - (r.h - 12) / p->lh);
        float ty = r.y + (maxtop > 0 ? (float)(p->top / maxtop) : 1) * (vh - th);
        cui_draw_rect(ui, (cui_rect){ r.x + r.w - 6, ty, 3, th }, CUI_CORNERS(1.5f), cui_alpha(p->bar, p->bar_t));
    }
}

/* the visible text of line i between columns, escapes left out */
static size_t cui__tm_copy_line(const cui_terminal_buffer *t, int li, uint32_t c0, uint32_t c1, char *out, size_t cap) {
    const cui_terminal_line *L = cui__tm_line(t, li);
    const char *s = t->text + L->offset;
    size_t n = 0;
    uint32_t col = 0;
    for (uint32_t i = 0; i < L->len;) {
        if ((unsigned char)s[i] == 0x1b) {
            i = cui__tm_skip_escape(s, i, L->len);
            continue;
        }
        uint32_t bytes;
        uint32_t span = cui__tm_glyph(s + i, L->len - i, &bytes);
        if (c0 < c1 && col + span > c0 && col < c1 && n + bytes < cap) {
            memcpy(out + n, s + i, bytes);
            n += bytes;
        }
        col += span;
        i += bytes;
    }
    return n;
}

static cui_terminal_result cui__terminal_ex_body(cui_ctx *ui, const char *id_str, cui_terminal_buffer *t, const cui_terminal_opts *o);

static void cui__terminal_raw_input(cui_ctx *ui, cui_terminal_result *result) {
    static const struct { cui_key key; const char *bytes; } keys[] = {
        { CUI_KEY_ENTER, "\r" }, { CUI_KEY_TAB, "\t" }, { CUI_KEY_BACKSPACE, "\x7f" },
        { CUI_KEY_ESCAPE, "\x1b" }, { CUI_KEY_DELETE, "\x1b[3~" },
        { CUI_KEY_UP, "\x1b[A" }, { CUI_KEY_DOWN, "\x1b[B" },
        { CUI_KEY_RIGHT, "\x1b[C" }, { CUI_KEY_LEFT, "\x1b[D" },
        { CUI_KEY_HOME, "\x1b[H" }, { CUI_KEY_END, "\x1b[F" },
        { CUI_KEY_PAGE_UP, "\x1b[5~" }, { CUI_KEY_PAGE_DOWN, "\x1b[6~" },
    };
    char input[4096];
    size_t length = 0;
    int mods = cui_key_mods(ui);
    if ((mods & CUI_MOD_PRIMARY) && !(mods & CUI_MOD_ALT)) {
        if (cui_key_pressed(ui, CUI_KEY_V) && ui->platform.get_clipboard) {
            const char *paste = ui->platform.get_clipboard(ui->platform.user);
            /* A paste must not silently submit commands or inject terminal escapes. */
            if (paste) {
                size_t count = strcspn(paste, "\r\n\x1b");
                if (count < sizeof input - 128) {
                    for (size_t i = 0; i < count; i++) {
                        unsigned char byte = (unsigned char)paste[i];
                        if (byte >= 0x20 && byte != 0x7f) input[length++] = (char)byte;
                    }
                }
            }
        } else {
            for (int key = CUI_KEY_A; key <= CUI_KEY_Z; key++) {
                if (key != CUI_KEY_V && cui_key_pressed(ui, (cui_key)key)) input[length++] = (char)(key - CUI_KEY_A + 1);
            }
        }
    } else if (ui->text_in_len > 0) {
        length = (size_t)ui->text_in_len;
        memcpy(input, ui->text_in, length);
    }
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        size_t count = strlen(keys[k].bytes);
        for (int repeats = ui->kpressed[keys[k].key]; repeats > 0 && count <= sizeof input - length; repeats--) {
            if (keys[k].key == CUI_KEY_TAB && (mods & CUI_MOD_CTRL)) break;
            memcpy(input + length, keys[k].bytes, count);
            length += count;
        }
    }
    if (length != 0) {
        result->input = cui__frame_strdup(ui, input, (int)length);
        result->input_length = (int)length;
    }
}

/* left to right in any interface: see cui__keep_ltr */
cui_terminal_result cui_terminal_ex(cui_ctx *ui, const char *id_str, cui_terminal_buffer *t, const cui_terminal_opts *o) {
    int first = ui->node_count;
    cui_terminal_result r = cui__terminal_ex_body(ui, id_str, t, o);
    cui__keep_ltr(ui, first);
    return r;
}

static cui_terminal_result cui__terminal_ex_body(cui_ctx *ui, const char *id_str, cui_terminal_buffer *t, const cui_terminal_opts *o) {
    cui_terminal_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_terminal_result res = { 0 };
    cui_id id = cui_make_id(ui, id_str ? id_str : "terminal");
    cui_id vid = id ^ 0x7E2Du, wid = id ^ 0x7E2Eu;
    cui_id fid = cui__hash_str("#term-input", id);
    cui__tm_state *st = cui_state(ui, id, sizeof *st);
    if (!st->init) { st->follow = true; st->init = true; }
    if (o->raw_input && !st->raw_input) {
        ui->focus = vid;
        ui->caret_time = ui->time;
    }
    st->raw_input = o->raw_input;
    float size = cui__fs(ui, o->size > 0 ? o->size : 13);
    float cw = cui__text_width(ui, CUI_FONT_MONO, size, "0", 1, true);
    if (cw <= 0) cw = size * 0.6f;
    float lh = ceilf(size * 1.45f);
    cui_rect pr = cui__prev_rect(ui, vid);
    float vw = pr.w > 0 ? pr.w : 600, vh = pr.h > 0 ? pr.h : (o->height > 0 ? o->height : 320) - 40;
    int cols = CUI_MAX(8, (int)((vw - 20) / cw));
    /* rows of the whole buffer at this width */
    double rows_total = 0;
    bool ok = t && t->line_cap;
    if (ok) for (int i = 0; i < t->count; i++) rows_total += cui__tm_rows(t, i, cols);
    double page = (vh - 12) / lh, maxtop = CUI_MAX(0.0, rows_total - page);
    int input_flags = o->raw_input ? CUI_IF_FOCUSABLE | CUI_IF_TEXT : 0;
    cui_interact vr = cui__interact(ui, vid, CUI_IF_DRAG | input_flags);
    if (ui->scroll_target == wid && ui->scroll_dy != 0) {
        st->top -= ui->scroll_dy * 3;
        st->follow = st->top >= maxtop - 0.5;
        ui->scroll_dy = 0;
    }
    if (st->follow) st->top = maxtop;
    st->top = CUI_CLAMP(st->top, 0, maxtop);
    if (st->top >= maxtop - 0.01) st->follow = true;
    /* the pointer as a text position */
    cui__tm_pos at = { 0, 0 };
    bool at_ok = false;
    if (ok && pr.w > 0) {
        double rr = st->top + (ui->my - pr.y - 6) / lh;
        if (rr < 0) rr = 0;
        double row = 0;
        int li = 0;
        while (li < t->count - 1) {
            int nr = cui__tm_rows(t, li, cols);
            if (row + nr > rr) break;
            row += nr; li++;
        }
        int sub = (int)(rr - row);
        long cc = (long)floorf((ui->mx - pr.x - 10) / cw + 0.5f);
        cc = CUI_CLAMP(cc, 0, cols);
        uint32_t col = (uint32_t)(sub * cols + cc);
        col = cui__tm_column(t, cui__tm_line(t, li), (uint32_t)cols, col, false);
        at = (cui__tm_pos){ t->total - (uint64_t)t->count + (uint64_t)li, col };
        at_ok = true;
    }
    if (vr.active && ui->mpressed[0] && at_ok) { st->a = st->b = at; st->selecting = true; st->has_sel = false; st->press_x = ui->mx; st->press_y = ui->my; }
    if (st->selecting && ui->mdown[0] && at_ok) {
        st->b = at;
        if (fabsf(ui->mx - st->press_x) + fabsf(ui->my - st->press_y) > 3) st->has_sel = true;
        /* dragging past the edge scrolls */
        if (ui->my < pr.y) { st->top = CUI_MAX(0.0, st->top - 0.5); st->follow = false; ui->animating = true; }
        if (ui->my > pr.y + pr.h) { st->top = CUI_MIN(maxtop, st->top + 0.5); ui->animating = true; }
    }
    if (st->selecting && !ui->mdown[0]) {
        st->selecting = false;
        ui->focus = o->no_input || o->raw_input ? vid : fid;
        if (o->raw_input) ui->caret_time = ui->time;
    }
    if (vr.hovered) cui__set_cursor(ui, CUI_CURSOR_TEXT);
    cui__tm_pos s0 = st->a, s1 = st->b;
    if (s1.line < s0.line || (s1.line == s0.line && s1.col < s0.col)) { cui__tm_pos tmp = s0; s0 = s1; s1 = tmp; }
    /* the copy shortcut copies a selection; Ctrl+C without one interrupts; Ctrl+L clears */
    bool ours = ui->focus == fid || ui->focus == vid;
    bool keys_available = !cui__keys_covered(ui) &&
                          (!ui->prev_modal_layer || ui->prev_modal_layer == ui->current_layer);
    bool field_sel = false;
    if (ui->focus == fid) { cui_edit_state *e = cui_state(ui, fid, sizeof(cui_edit_state)); field_sel = e->cursor != e->anchor; }
    if (ours && keys_available && !field_sel && cui_key_pressed(ui, CUI_KEY_C) &&
        (ui->mods & (CUI_MOD_CTRL | CUI_MOD_PRIMARY))) {
        if (st->has_sel && (ui->mods & CUI_MOD_PRIMARY)) {
            if (ok && ui->platform.set_clipboard) {
            size_t cap = 256 * 1024, n = 0;
            char *buf = cui__alloc(ui, cap);
            if (buf) {
                uint64_t first_no = t->total - (uint64_t)t->count;
                for (uint64_t no = CUI_MAX(s0.line, first_no); no <= s1.line && no < t->total; no++) {
                    int li = (int)(no - first_no);
                    uint32_t c0 = no == s0.line ? s0.col : 0, c1 = no == s1.line ? s1.col : 0xFFFFFFFFu;
                    n += cui__tm_copy_line(t, li, c0, c1, buf + n, cap - n - 2);
                    if (no != s1.line && n < cap - 2) buf[n++] = '\n';
                }
                buf[n] = 0;
                ui->platform.set_clipboard(ui->platform.user, buf);
                cui__free(ui, buf);
            }
            }
            st->has_sel = false;
            ui->kpressed[CUI_KEY_C] = 0;
        } else if (ui->mods & CUI_MOD_CTRL) {
            res.interrupt = true;
            if (ok && !o->raw_input) cui_terminal_write(t, "^C\n", 3);
            st->input[0] = 0;
            ui->kpressed[CUI_KEY_C] = 0;
        }
    }
    bool raw_focused = o->raw_input && ui->focus == vid && keys_available && ui->window_focused;
    if (raw_focused) {
        ui->tab_owner = vid;
        cui__terminal_raw_input(ui, &res);
        if (res.input_length || res.interrupt) ui->caret_time = ui->time;
    }
    if (!o->raw_input && ours && cui_key_pressed(ui, CUI_KEY_L) && (ui->mods & CUI_MOD_CTRL) && ok) { cui_terminal_clear(t); st->top = 0; st->follow = true; st->has_sel = false; }
    /* history */
    if (ui->focus == fid && st->hn > 0) {
        int dir = cui_key_pressed(ui, CUI_KEY_UP) ? -1 : cui_key_pressed(ui, CUI_KEY_DOWN) ? 1 : 0;
        if (dir) {
            if (!st->browsing) { st->hpos = st->hn; st->browsing = true; }
            st->hpos = CUI_CLAMP(st->hpos + dir, 0, st->hn);
            snprintf(st->input, sizeof st->input, "%s", st->hpos < st->hn ? st->hist[st->hpos] : "");
            ui->tf_select_all = 0;
            cui_edit_state *e = cui_state(ui, fid, sizeof(cui_edit_state));
            e->cursor = e->anchor = (int)strlen(st->input);
            ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
        }
    }

    float h = o->height > 0 ? o->height : 320;
    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->w = o->width.kind ? o->width : CUI_FILL;
    cn->h = o->height < 0 ? CUI_FILL : CUI_PX(h);
    cn->self_align = CUI_ALIGN_STRETCH; cn->gap = 0;
    cn->bg = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.5f) : CUI_C(ui, SURFACE);
    cn->border = 1; cn->border_color = CUI_C(ui, BORDER);
    cn->radius = CUI_CORNERS(ui->theme.radius_control);
    cn->flags |= CUI_NF_CLIP;
    cui__open(ui, col);
    int v = cui__leaf(ui, vid, CUI_FILL, CUI_FILL);
    ui->nodes[v].self_align = CUI_ALIGN_STRETCH;
    cui__mark_interactive(ui, v, input_flags);
    cui__tm_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->t = ok ? t : NULL; p->top = st->top; p->cols = cols; p->cw = cw; p->lh = lh; p->size = size;
    p->has_sel = st->has_sel; p->s0 = s0; p->s1 = s1;
    if (raw_focused && ok && !t->cursor_hidden) {
        double phase = ui->time - ui->caret_time;
        p->show_caret = fmod(phase, 1.06) < 0.53;
        cui_request_wakeup(ui, 0.53f - (float)fmod(phase, 0.53));
    }
    p->fg = CUI_C(ui, FOREGROUND);
    p->sel = cui_alpha(cui__emphasis(ui), ui->mode == CUI_THEME_DARK ? 0.3f : 0.18f);
    p->bar = CUI_C(ui, BORDER_STRONG); p->bar_t = vr.hovered || !st->follow ? 1 : 0; p->rows_total = rows_total;
    p->wheel = wid;
    ui->nodes[v].paint = cui__paint_terminal;
    ui->nodes[v].paint_data = p;
    if (!o->no_input && !o->raw_input) {
        int line = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
        ui->nodes[line].bg = CUI_C(ui, BORDER_SUBTLE);
        int row = cui__node_new(ui, 0);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(34);
        rn->pad[1] = rn->pad[3] = 10; rn->gap = 0;
        cui__open(ui, row);
        const char *prompt = o->prompt ? o->prompt : "$ ";
        int pt = cui__text_node(ui, prompt, -1, CUI_FONT_MONO, size, ui->accent == CUI_ACCENT_NEUTRAL ? CUI_C(ui, TEXT_SECONDARY) : CUI_C(ui, ACCENT), false);
        ui->nodes[pt].flags |= CUI_NF_TABULAR;
        cui_input_opts io;
        memset(&io, 0, sizeof io);
        io.width = CUI_FILL;
        ui->tf_override = true; ui->tf_font = CUI_FONT_MONO; ui->tf_size = size;
        int f = cui__text_field(ui, fid, st->input, sizeof st->input, &io, false, 1, NULL);
        if (f & CUI_CHANGED) st->browsing = false;
        if ((f & CUI_SUBMITTED)) {
            if (ok) { cui_terminal_write(t, "\x1b[2m", 4); cui_terminal_write(t, prompt, -1); cui_terminal_write(t, "\x1b[0m", 4); cui_terminal_write(t, st->input, -1); cui_terminal_write(t, "\n", 1); }
            if (st->input[0] && (st->hn == 0 || strcmp(st->hist[st->hn - 1], st->input))) {
                if (st->hn == 32) { memmove(st->hist[0], st->hist[1], sizeof st->hist[0] * 31); st->hn--; }
                size_t n = strlen(st->input), cap = sizeof st->hist[0] - 1;
                if (n > cap) { n = cap; while (n > 0 && ((unsigned char)st->input[n] & 0xC0) == 0x80) n--; }   /* whole characters */
                memcpy(st->hist[st->hn], st->input, n); st->hist[st->hn++][n] = 0;
            }
            res.command = cui__frame_strdup(ui, st->input, -1);
            st->input[0] = 0;
            st->browsing = false; st->follow = true;
            cui_edit_state *e = cui_state(ui, fid, sizeof(cui_edit_state));
            e->cursor = e->anchor = 0;
        }
        cui__close(ui);
    }
    cui__close(ui);
    ui->last_id = o->no_input || o->raw_input ? vid : fid;
    return res;
}
