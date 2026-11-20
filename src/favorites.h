#ifndef SERIAL_FAVORITES_H
#define SERIAL_FAVORITES_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#define FAVORITES_CAPACITY 12
#define COMMAND_FAVORITES_CAPACITY 32
#define FAVORITE_COMMAND_NAME_CAPACITY 64
#define SERIAL_COMMAND_CAPACITY 1024
#define SERIAL_MAX_PORT 256
#define SERIAL_MAX_BAUD 12000000UL

typedef struct
{
    int port_number;
    unsigned long baud;
    int data_bits_index;
    int parity_index;
    int stop_bits_index;
    bool flow_control;
} favorite_session;

typedef struct
{
    bool hex;
    int ending_index;
    char text[SERIAL_COMMAND_CAPACITY];
    char name[FAVORITE_COMMAND_NAME_CAPACITY];
} favorite_command;

typedef struct
{
    int session_count;
    int command_count;
    favorite_session sessions[FAVORITES_CAPACITY];
    favorite_command commands[COMMAND_FAVORITES_CAPACITY];
} favorites;

/* 返回 EXE 同目录的收藏文件路径。路径不足时返回 false。 */
bool favorites_default_path(wchar_t *path, size_t capacity);
/* 读取 EXE 同目录的收藏，首次兼容旧版文件。path 返回新版保存路径，容量为 MAX_PATH。 */
bool favorites_load_default(wchar_t *path, favorites *items);
/* 文件不存在时返回空收藏。读取失败时保留调用者原有数据。 */
bool favorites_load(const wchar_t *path, favorites *items);
/* 整份写完后才替换原文件，写入失败时保留原文件。 */
bool favorites_save(const wchar_t *path, const favorites *items);

#endif
