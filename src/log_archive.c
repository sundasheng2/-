#include "log_archive.h"

#include <share.h>
#include <wchar.h>

static FILE *open_log_file(const wchar_t *root, int port_number, const SYSTEMTIME *time)
{
    wchar_t path[MAX_PATH + 32];
    size_t root_length = wcslen(root);
    if (root_length < 3 || root_length >= MAX_PATH || port_number <= 0)
    {
        return NULL;
    }

    /* COM1 到 COM9 是 DOS 设备名，用扩展路径创建真正的同名目录。 */
    const wchar_t *prefix;
    if (root[0] == L'\\' && root[1] == L'\\')
    {
        prefix = L"\\\\?\\UNC\\";
        root += 2;
        root_length -= 2;
    }
    else if (root[1] == L':' && root[2] == L'\\')
    {
        prefix = L"\\\\?\\";
    }
    else
    {
        return NULL;
    }
    const size_t capacity = sizeof path / sizeof path[0];
    const wchar_t *separator = root[root_length - 1] == L'\\' ? L"" : L"\\";
    int length = swprintf(path, capacity, L"%ls%ls%lsCOM%d", prefix, root, separator, port_number);
    if (length < 0 || (size_t)length >= capacity)
    {
        return NULL;
    }
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        return NULL;
    }
    int written = swprintf(path + length, capacity - (size_t)length, L"\\%04u-%02u-%02u.txt",
                           (unsigned int)time->wYear, (unsigned int)time->wMonth,
                           (unsigned int)time->wDay);
    if (written < 0 || (size_t)written >= capacity - (size_t)length)
    {
        return NULL;
    }

    FILE *file = _wfsopen(path, L"a+b", _SH_DENYWR);
    if (file == NULL)
    {
        return NULL;
    }
    /* 重新开始记录时，把上次未结束的行与新的时间戳隔开。 */
    if (_fseeki64(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return NULL;
    }
    __int64 size = _ftelli64(file);
    if (size < 0)
    {
        fclose(file);
        return NULL;
    }
    if (size > 0)
    {
        if (_fseeki64(file, -1, SEEK_END) != 0)
        {
            fclose(file);
            return NULL;
        }
        int last = fgetc(file);
        if (last == EOF || _fseeki64(file, 0, SEEK_END) != 0 ||
            (last != '\n' && fputs("\r\n", file) == EOF))
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

log_archive_result log_archive_update(log_archive *archive, FILE **file, const wchar_t *root,
                                      int port_number, const SYSTEMTIME *time)
{
    unsigned int date = time->wYear * 10000u + time->wMonth * 100u + time->wDay;
    if (*file != NULL && archive->port_number == port_number && archive->date == date)
    {
        return LOG_ARCHIVE_UNCHANGED;
    }

    FILE *next = open_log_file(root, port_number, time);
    if (next == NULL)
    {
        return LOG_ARCHIVE_ERROR;
    }
    if (*file != NULL && fclose(*file) != 0)
    {
        *file = NULL;
        fclose(next);
        return LOG_ARCHIVE_ERROR;
    }

    *file = next;
    archive->port_number = port_number;
    archive->date = date;
    return LOG_ARCHIVE_OPENED;
}
