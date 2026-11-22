#ifndef SERIAL_LOG_ARCHIVE_H
#define SERIAL_LOG_ARCHIVE_H

#include <stdio.h>
#include <windows.h>

typedef struct
{
    int port_number;
    unsigned int date;
} log_archive;

typedef enum
{
    LOG_ARCHIVE_UNCHANGED,
    LOG_ARCHIVE_OPENED,
    LOG_ARCHIVE_ERROR,
} log_archive_result;

/* 按本地日期追加到绝对路径 root 下的 COM 目录。时间由调用者提供，
   以便同一次接收的文件日期和正文时间戳使用同一个时刻。 */
log_archive_result log_archive_update(log_archive *archive, FILE **file, const wchar_t *root,
                                      int port_number, const SYSTEMTIME *time);

#endif
