#include "ssh_session.h"

#include <stdio.h>
#include <string.h>

static BOOL valid_token(const char *text, const char *extra)
{
    for (; *text != '\0'; text++)
    {
        char c = *text;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr(extra, c) != NULL))
        {
            return FALSE;
        }
    }
    return TRUE;
}

DWORD ssh_session_start(ssh_session *session)
{
    if (session->process != NULL)
    {
        return ERROR_BUSY;
    }

    unsigned int port = 0;
    for (const char *digit = session->port; *digit != '\0'; digit++)
    {
        if (*digit < '0' || *digit > '9')
        {
            return ERROR_INVALID_PARAMETER;
        }
        port = port * 10 + (unsigned int)(*digit - '0');
    }
    if (port == 0 || port > 65535 || !session->host[0] || session->host[0] == '-' ||
        !valid_token(session->host, "-._:") || !valid_token(session->user, "-._@"))
    {
        return ERROR_INVALID_PARAMETER;
    }

    wchar_t program[MAX_PATH];
    static const wchar_t suffix[] = L"\\OpenSSH\\ssh.exe";
    UINT length = GetSystemDirectoryW(program, MAX_PATH);
    if (length == 0 || length + sizeof suffix / sizeof suffix[0] > MAX_PATH)
    {
        return ERROR_PATH_NOT_FOUND;
    }
    wcscpy(program + length, suffix);
    if (GetFileAttributesW(program) == INVALID_FILE_ATTRIBUTES)
    {
        return GetLastError();
    }

    /* 直接启动系统 OpenSSH。双引号包住单个参数，输入不经过命令解释器。 */
    wchar_t host[SSH_HOST_CAPACITY];
    wchar_t user[SSH_USER_CAPACITY];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, session->host, -1, host, SSH_HOST_CAPACITY) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, session->user, -1, user, SSH_USER_CAPACITY))
    {
        return GetLastError();
    }
    wchar_t command[MAX_PATH + SSH_HOST_CAPACITY + SSH_USER_CAPACITY + 64];
    int written = swprintf(command, sizeof command / sizeof command[0],
                           L"\"%ls\" -p %u %ls%ls%ls -- \"%ls\"", program, port,
                           user[0] ? L"-l \"" : L"", user, user[0] ? L"\"" : L"", host);
    if (written < 0 || (size_t)written >= sizeof command / sizeof command[0])
    {
        return ERROR_INSUFFICIENT_BUFFER;
    }

    STARTUPINFOW startup = {.cb = sizeof startup, .lpTitle = L"SSH"};
    PROCESS_INFORMATION child;
    if (!CreateProcessW(program, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                        NULL, NULL, &startup, &child))
    {
        return GetLastError();
    }
    CloseHandle(child.hThread);
    session->process = child.hProcess;
    return ERROR_SUCCESS;
}

BOOL ssh_session_poll(ssh_session *session, DWORD *exit_code)
{
    if (session->process == NULL || WaitForSingleObject(session->process, 0) != WAIT_OBJECT_0)
    {
        return FALSE;
    }
    if (!GetExitCodeProcess(session->process, exit_code))
    {
        *exit_code = GetLastError();
    }
    CloseHandle(session->process);
    session->process = NULL;
    return TRUE;
}

DWORD ssh_session_stop(ssh_session *session)
{
    DWORD exit_code;
    if (session->process == NULL || ssh_session_poll(session, &exit_code))
    {
        return ERROR_SUCCESS;
    }
    if (!TerminateProcess(session->process, 0))
    {
        DWORD error = GetLastError();
        return ssh_session_poll(session, &exit_code) ? ERROR_SUCCESS : error;
    }
    CloseHandle(session->process);
    session->process = NULL;
    return ERROR_SUCCESS;
}
