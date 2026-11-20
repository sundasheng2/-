#ifndef SERIAL_ANSI_H
#define SERIAL_ANSI_H

#include <cui/cui.h>

#include "text_decode.h"

#define SERIAL_ANSI_MAX_SPANS (TEXT_DECODE_INPUT_CAPACITY / 3 + 2)

typedef struct
{
    cui_color foreground, background;
    unsigned int mode;
    size_t sequence_length;
    char sequence[64];
} serial_ansi;

/* 原地移除接收文本里的控制序列，颜色变化使用包含 prefix_length 的 UTF-8 字节偏移。
 * 输入来自一次 text_decode_utf8，spans 至少有 SERIAL_ANSI_MAX_SPANS 项。
 * end_of_line 时丢弃未完整的控制序列，已生效的颜色保留到下一行。 */
uint16_t serial_ansi_strip(serial_ansi *state, char *text, size_t prefix_length,
                           bool end_of_line, cui_log_color_span *spans);

#endif
