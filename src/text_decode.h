#ifndef SERIAL_TEXT_DECODE_H
#define SERIAL_TEXT_DECODE_H

#include <stdbool.h>
#include <stddef.h>

#define TEXT_DECODE_INPUT_CAPACITY 2048
#define TEXT_DECODE_UTF8_CAPACITY (TEXT_DECODE_INPUT_CAPACITY * 3 + 1)

typedef enum
{
    TEXT_ENCODING_AUTO = 0,
    TEXT_ENCODING_UTF8,
    TEXT_ENCODING_GBK,
} text_encoding;

/* 将一段接收文本转成以空字符结尾的 UTF-8，返回消费的输入字节数。
 * finish 为 false 时，末尾尚未收全的字符留给调用者和下一批字节一起处理。
 * length 最大为 TEXT_DECODE_INPUT_CAPACITY，output 需有 TEXT_DECODE_UTF8_CAPACITY 字节。 */
size_t text_decode_utf8(const char *input, size_t length, text_encoding encoding, bool finish,
                        char *output);

#endif
