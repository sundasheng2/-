#include "serial_ansi.h"

#include <string.h>

enum
{
    ANSI_TEXT,
    ANSI_ESCAPE,
    ANSI_CSI,
    ANSI_CSI_DISCARD,
};

static cui_color palette_color(int index)
{
    static const uint32_t basic[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
        0x666666, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
    };
    if (index < 16)
    {
        return CUI_RGB(basic[index]);
    }
    if (index < 232)
    {
        static const uint8_t channels[] = {0, 95, 135, 175, 215, 255};
        int cube = index - 16;
        return (cui_color){channels[cube / 36], channels[cube / 6 % 6], channels[cube % 6], 255};
    }
    uint8_t gray = (uint8_t)(8 + (index - 232) * 10);
    return (cui_color){gray, gray, gray, 255};
}

static void apply_colors(serial_ansi *state)
{
    int args[sizeof state->sequence + 1];
    size_t count = 0;
    int value = 0;
    for (size_t i = 0; i < state->sequence_length; i++)
    {
        unsigned char byte = (unsigned char)state->sequence[i];
        if (byte == ';')
        {
            args[count++] = value;
            value = 0;
        }
        else if (byte >= '0' && byte <= '9')
        {
            if (value <= 255)
            {
                value = value * 10 + byte - '0';
            }
        }
        else
        {
            return;
        }
    }
    args[count++] = value;

    for (size_t i = 0; i < count; i++)
    {
        int code = args[i];
        if (code == 0 || code == 39) state->foreground = (cui_color){0};
        if (code == 0 || code == 49) state->background = (cui_color){0};
        if (code >= 30 && code <= 37) state->foreground = palette_color(code - 30);
        if (code >= 40 && code <= 47) state->background = palette_color(code - 40);
        if (code >= 90 && code <= 97) state->foreground = palette_color(code - 90 + 8);
        if (code >= 100 && code <= 107) state->background = palette_color(code - 100 + 8);
        if (code == 38 || code == 48 || code == 58)
        {
            if (++i == count) break;
            cui_color color = {0};
            if (args[i] == 5)
            {
                if (++i == count) break;
                if (args[i] <= 255) color = palette_color(args[i]);
            }
            else if (args[i] == 2)
            {
                if (count - i <= 3) break;
                if (args[i + 1] <= 255 && args[i + 2] <= 255 && args[i + 3] <= 255)
                {
                    color = (cui_color){(uint8_t)args[i + 1], (uint8_t)args[i + 2],
                                        (uint8_t)args[i + 3], 255};
                }
                i += 3;
            }
            if (color.a && code == 38) state->foreground = color;
            if (color.a && code == 48) state->background = color;
        }
    }
}

uint16_t serial_ansi_strip(serial_ansi *state, char *text, size_t prefix_length,
                           bool end_of_line, cui_log_color_span *spans)
{
    size_t target = 0;
    uint16_t count = 0;
    cui_color foreground = {0}, background = {0};
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        unsigned char byte = (unsigned char)text[i];
        if (byte == 0x1b)
        {
            state->mode = ANSI_ESCAPE;
            continue;
        }
        if (state->mode == ANSI_ESCAPE)
        {
            state->mode = byte == '[' ? ANSI_CSI : ANSI_TEXT;
            state->sequence_length = 0;
            if (state->mode == ANSI_CSI) continue;
        }
        if (state->mode == ANSI_CSI || state->mode == ANSI_CSI_DISCARD)
        {
            if (byte >= 0x40 && byte <= 0x7e)
            {
                if (byte == 'm' && state->mode == ANSI_CSI) apply_colors(state);
                state->mode = ANSI_TEXT;
                continue;
            }
            if (byte >= 0x20 && byte <= 0x3f)
            {
                if (state->sequence_length == sizeof state->sequence) state->mode = ANSI_CSI_DISCARD;
                if (state->mode == ANSI_CSI) state->sequence[state->sequence_length++] = (char)byte;
                continue;
            }
            state->mode = ANSI_TEXT;
        }

        if (count < SERIAL_ANSI_MAX_SPANS &&
            (memcmp(&foreground, &state->foreground, sizeof foreground) != 0 ||
             memcmp(&background, &state->background, sizeof background) != 0))
        {
            foreground = state->foreground;
            background = state->background;
            spans[count++] = (cui_log_color_span){(uint32_t)(prefix_length + target), foreground, background};
        }
        text[target++] = (char)byte;
    }
    text[target] = '\0';
    if (end_of_line) state->mode = ANSI_TEXT;
    return count;
}
