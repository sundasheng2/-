#include "favorites.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char header_v1[] = "STM32SERIAL1\n";
static const char header_v2[] = "STM32SERIAL2\n";

static bool read_session(const char *line, favorites *items);
static bool read_command(const char *line, favorites *items, bool has_names);
static bool write_items(FILE *file, const favorites *items);

bool favorites_default_path(wchar_t *path, size_t capacity)
{
    static const wchar_t filename[] = L"串口收藏-v2.txt";
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)capacity);
    if (length == 0 || length >= capacity)
    {
        return false;
    }
    wchar_t *name = wcsrchr(path, L'\\');
    if (name == NULL || (size_t)(name + 1 - path) + sizeof filename / sizeof filename[0] > capacity)
    {
        return false;
    }
    memcpy(name + 1, filename, sizeof filename);
    return true;
}

bool favorites_load_default(wchar_t *path, favorites *items)
{
    static const wchar_t legacy_filename[] = L"串口收藏.txt";
    wchar_t legacy_path[MAX_PATH];
    if (!favorites_default_path(path, MAX_PATH))
    {
        path[0] = L'\0';
        return false;
    }
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        return favorites_load(path, items);
    }
    wcscpy(legacy_path, path);
    wcscpy(wcsrchr(legacy_path, L'\\') + 1, legacy_filename);
    return favorites_load(legacy_path, items);
}

bool favorites_load(const wchar_t *path, favorites *items)
{
    favorites loaded = {0};
    char line[SERIAL_COMMAND_CAPACITY + FAVORITE_COMMAND_NAME_CAPACITY + 16];
    FILE *file = _wfopen(path, L"rb");
    if (file == NULL)
    {
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            *items = loaded;
            return true;
        }
        return false;
    }
    bool has_header = fgets(line, sizeof line, file) != NULL;
    bool has_names = has_header && strcmp(line, header_v2) == 0;
    bool valid = has_names || (has_header && strcmp(line, header_v1) == 0);
    while (valid && fgets(line, sizeof line, file) != NULL)
    {
        char *end = strchr(line, '\n');
        if (end == NULL)
        {
            valid = false;
            break;
        }
        *end = '\0';
        valid = line[0] == 'S' ? read_session(line, &loaded) :
                                 read_command(line, &loaded, has_names);
    }
    valid = valid && !ferror(file);
    fclose(file);
    if (valid)
    {
        *items = loaded;
    }
    return valid;
}

bool favorites_save(const wchar_t *path, const favorites *items)
{
    wchar_t directory[MAX_PATH] = L".";
    wchar_t temp_path[MAX_PATH];
    const wchar_t *name = wcsrchr(path, L'\\');
    if (name != NULL)
    {
        size_t length = (size_t)(name + 1 - path);
        if (length >= MAX_PATH)
        {
            return false;
        }
        memcpy(directory, path, length * sizeof *directory);
        directory[length] = L'\0';
    }
    /* 临时文件与目标同目录，避免跨卷移动导致替换失败。 */
    if (!GetTempFileNameW(directory, L"scf", 0, temp_path))
    {
        return false;
    }
    FILE *file = _wfopen(temp_path, L"wb");
    bool written = false;
    if (file != NULL)
    {
        written = write_items(file, items);
        if (fclose(file) != 0)
        {
            written = false;
        }
    }
    if (written && MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return true;
    }
    DeleteFileW(temp_path);
    return false;
}

static bool read_session(const char *line, favorites *items)
{
    favorite_session session;
    unsigned int flow;
    int consumed = 0;
    if (items->session_count == FAVORITES_CAPACITY ||
        sscanf(line, "S\t%3d\t%8lu\t%1d\t%1d\t%1d\t%1u%n", &session.port_number,
               &session.baud, &session.data_bits_index, &session.parity_index,
               &session.stop_bits_index, &flow, &consumed) != 6 || line[consumed] != '\0' ||
        session.port_number < 1 || session.port_number > SERIAL_MAX_PORT ||
        session.baud < 1 || session.baud > SERIAL_MAX_BAUD ||
        session.data_bits_index < 0 || session.data_bits_index > 1 ||
        session.parity_index < 0 || session.parity_index > 2 ||
        session.stop_bits_index < 0 || session.stop_bits_index > 1 || flow > 1)
    {
        return false;
    }
    session.flow_control = flow != 0;
    items->sessions[items->session_count++] = session;
    return true;
}

static bool read_command(const char *line, favorites *items, bool has_names)
{
    if (items->command_count >= COMMAND_FAVORITES_CAPACITY || strlen(line) < 7 ||
        line[0] != 'C' || line[1] != '\t' || line[3] != '\t' || line[5] != '\t' ||
        (line[2] != '0' && line[2] != '1') ||
        line[4] < '0' || line[4] > '3' ||
        strchr(line + 6, '\r') != NULL)
    {
        return false;
    }
    favorite_command *command = &items->commands[items->command_count];
    const char *text = line + 6;
    if (has_names)
    {
        const char *end = strchr(text, '\t');
        if (end == NULL || (size_t)(end - text) >= sizeof command->name)
        {
            return false;
        }
        size_t name_length = (size_t)(end - text);
        memcpy(command->name, text, name_length);
        command->name[name_length] = '\0';
        text = end + 1;
    }
    size_t length = strlen(text);
    if (length == 0 || length >= sizeof command->text)
    {
        return false;
    }
    command->hex = line[2] == '1';
    command->ending_index = line[4] - '0';
    memcpy(command->text, text, length + 1);
    items->command_count++;
    return true;
}

static bool write_items(FILE *file, const favorites *items)
{
    if (fputs(header_v2, file) == EOF)
    {
        return false;
    }
    for (int i = 0; i < items->session_count; i++)
    {
        const favorite_session *session = &items->sessions[i];
        if (fprintf(file, "S\t%d\t%lu\t%d\t%d\t%d\t%d\n", session->port_number, session->baud,
                    session->data_bits_index, session->parity_index,
                    session->stop_bits_index, session->flow_control ? 1 : 0) < 0)
        {
            return false;
        }
    }
    for (int i = 0; i < items->command_count; i++)
    {
        const favorite_command *command = &items->commands[i];
        if (command->text[0] == '\0' || strpbrk(command->text, "\r\n") != NULL ||
            strpbrk(command->name, "\t\r\n") != NULL ||
            fprintf(file, "C\t%d\t%d\t%s\t%s\n", command->hex ? 1 : 0,
                    command->ending_index, command->name, command->text) < 0)
        {
            return false;
        }
    }
    return true;
}
