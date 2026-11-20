#include "text_decode.h"

#include <windows.h>

#define CODE_PAGE_GBK 936

static size_t utf8_complete_prefix(const unsigned char *data, size_t length);
static size_t gbk_complete_prefix(const unsigned char *data, size_t length);
static int to_utf16(UINT code_page, DWORD flags, const char *input, size_t length, WCHAR *wide);

size_t text_decode_utf8(const char *input, size_t length, text_encoding encoding, bool finish,
                        char *output)
{
    WCHAR wide[TEXT_DECODE_INPUT_CAPACITY];
    const unsigned char *bytes = (const unsigned char *)input;
    size_t used = length;
    int wide_count = -1;
    output[0] = '\0';
    if (length > TEXT_DECODE_INPUT_CAPACITY)
    {
        return 0;
    }

    if (encoding != TEXT_ENCODING_GBK)
    {
        if (!finish)
        {
            used = utf8_complete_prefix(bytes, length);
        }
        wide_count = to_utf16(CP_UTF8, MB_ERR_INVALID_CHARS, input, used, wide);
    }
    if (wide_count < 0 && encoding != TEXT_ENCODING_UTF8)
    {
        size_t gbk_length = finish ? length : gbk_complete_prefix(bytes, length);
        DWORD flags = encoding == TEXT_ENCODING_GBK ? 0 : MB_ERR_INVALID_CHARS;
        wide_count = to_utf16(CODE_PAGE_GBK, flags, input, gbk_length, wide);
        if (wide_count >= 0)
        {
            used = gbk_length;
        }
    }
    if (wide_count < 0)
    {
        /* 无效字节使用 Windows 的替换字符，保留同段中有效的 UTF-8。 */
        wide_count = to_utf16(CP_UTF8, 0, input, used, wide);
    }
    if (wide_count > 0)
    {
        int count = WideCharToMultiByte(CP_UTF8, 0, wide, wide_count, output,
                                        TEXT_DECODE_UTF8_CAPACITY - 1, NULL, NULL);
        output[count] = '\0';
    }
    return used;
}

static int to_utf16(UINT code_page, DWORD flags, const char *input, size_t length, WCHAR *wide)
{
    if (length == 0)
    {
        return 0;
    }
    int result = MultiByteToWideChar(code_page, flags, input, (int)length,
                                     wide, TEXT_DECODE_INPUT_CAPACITY);
    return result > 0 ? result : -1;
}

static size_t utf8_complete_prefix(const unsigned char *data, size_t length)
{
    if (length == 0)
    {
        return 0;
    }
    size_t start = length - 1;
    while (start > 0 && length - start < 4 && (data[start] & 0xc0) == 0x80)
    {
        start--;
    }
    unsigned char lead = data[start];
    size_t available = length - start;
    if ((lead >= 0xc2 && lead <= 0xdf && available < 2) ||
        (lead >= 0xe0 && lead <= 0xef && available < 3) ||
        (lead >= 0xf0 && lead <= 0xf4 && available < 4))
    {
        return start;
    }
    return length;
}

static size_t gbk_complete_prefix(const unsigned char *data, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (data[i] < 0x81 || data[i] > 0xfe)
        {
            continue;
        }
        if (i + 1 == length)
        {
            return i;
        }
        unsigned char trail = data[i + 1];
        if (trail >= 0x40 && trail <= 0xfe && trail != 0x7f)
        {
            i++;
        }
    }
    return length;
}
