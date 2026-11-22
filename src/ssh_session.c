#include "ssh_session.h"

#include <stdio.h>
#include <string.h>

/* 旧版 SDK 的目标版本宏可能隐藏这个 Win32 属性声明。 */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

typedef HRESULT (WINAPI *create_pseudoconsole_fn)(COORD, HANDLE, HANDLE, DWORD, HANDLE *);
typedef void (WINAPI *close_pseudoconsole_fn)(HANDLE);

static close_pseudoconsole_fn close_pseudoconsole;

static void close_pipe(HANDLE *pipe)
{
    if (*pipe != NULL)
    {
        CloseHandle(*pipe);
        *pipe = NULL;
    }
}

static void close_session_handles(ssh_session *session)
{
    if (session->input_writing != 0)
    {
        DWORD transferred;
        CancelIoEx(session->input_pipe, &session->input_io);
        GetOverlappedResult(session->input_pipe, &session->input_io, &transferred, TRUE);
        session->input_writing = 0;
    }
    SecureZeroMemory(session->input, sizeof session->input);
    session->input_length = 0;
    close_pipe(&session->input_io.hEvent);
    close_pipe(&session->input_pipe);
    /* 先关闭输出读端，旧版 Windows 的伪控制台关闭时不会等界面读取。 */
    close_pipe(&session->output_pipe);
    if (session->console != NULL)
    {
        close_pseudoconsole(session->console);
        session->console = NULL;
    }
    if (session->close_thread != NULL)
    {
        WaitForSingleObject(session->close_thread, INFINITE);
        close_pipe(&session->close_thread);
    }
    close_pipe(&session->process);
}

static DWORD create_input_pipe(HANDLE *reader, HANDLE *writer)
{
    static unsigned long pipe_id;
    wchar_t name[80];
    swprintf(name, sizeof name / sizeof name[0], L"\\\\.\\pipe\\stm32-ssh-%lu-%lu",
             (unsigned long)GetCurrentProcessId(), ++pipe_id);
    HANDLE server = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 0,
                                     SSH_INPUT_CAPACITY, 0, NULL);
    if (server == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }
    HANDLE client = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, NULL);
    if (client == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        CloseHandle(server);
        return error;
    }
    if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
    {
        DWORD error = GetLastError();
        CloseHandle(client);
        CloseHandle(server);
        return error;
    }
    *reader = server;
    *writer = client;
    return ERROR_SUCCESS;
}

static DWORD start_pseudoconsole(ssh_session *session, const wchar_t *program, wchar_t *command)
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    create_pseudoconsole_fn create_pseudoconsole =
        (create_pseudoconsole_fn)(void *)GetProcAddress(kernel, "CreatePseudoConsole");
    if (close_pseudoconsole == NULL)
    {
        close_pseudoconsole = (close_pseudoconsole_fn)(void *)GetProcAddress(kernel, "ClosePseudoConsole");
    }
    if (create_pseudoconsole == NULL || close_pseudoconsole == NULL)
    {
        return ERROR_NOT_SUPPORTED;
    }

    HANDLE input_reader = NULL;
    HANDLE output_writer = NULL;
    DWORD error = create_input_pipe(&input_reader, &session->input_pipe);
    if (error != ERROR_SUCCESS)
    {
        return error;
    }
    STARTUPINFOEXW startup = {0};
    BOOL attributes_initialized = FALSE;
    if (!CreatePipe(&session->output_pipe, &output_writer, NULL, 16384))
    {
        error = GetLastError();
        goto cleanup;
    }
    session->input_io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (session->input_io.hEvent == NULL)
    {
        error = GetLastError();
        goto cleanup;
    }
    HRESULT result = create_pseudoconsole((COORD){120, SSH_TERMINAL_ROWS}, input_reader, output_writer, 0,
                                         &session->console);
    if (FAILED(result))
    {
        error = HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result) : ERROR_GEN_FAILURE;
        goto cleanup;
    }
    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attributes_size);
    if (startup.lpAttributeList == NULL)
    {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    attributes_initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                                               &attributes_size);
    if (!attributes_initialized ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   session->console, sizeof session->console, NULL, NULL))
    {
        error = GetLastError();
        goto cleanup;
    }
    PROCESS_INFORMATION child;
    startup.StartupInfo.cb = sizeof startup;
    /* 空的标准句柄由伪控制台提供，避免继承宿主重定向的句柄。 */
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessW(program, command, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                        NULL, NULL, &startup.StartupInfo, &child))
    {
        error = GetLastError();
        goto cleanup;
    }
    CloseHandle(child.hThread);
    session->process = child.hProcess;
    error = ERROR_SUCCESS;

cleanup:
    if (attributes_initialized)
    {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    if (startup.lpAttributeList != NULL)
    {
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    close_pipe(&input_reader);
    close_pipe(&output_writer);
    if (error != ERROR_SUCCESS)
    {
        close_session_handles(session);
    }
    return error;
}

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

    return start_pseudoconsole(session, program, command);
}

DWORD ssh_session_read(ssh_session *session, char *output, DWORD capacity)
{
    DWORD available;
    if (session->output_pipe == NULL ||
        !PeekNamedPipe(session->output_pipe, NULL, 0, NULL, &available, NULL) || available == 0)
    {
        return 0;
    }
    DWORD length;
    return ReadFile(session->output_pipe, output, available < capacity ? available : capacity,
                    &length, NULL) ? length : 0;
}

static void remove_written_input(ssh_session *session, DWORD written)
{
    session->input_length -= written;
    memmove(session->input, session->input + written, session->input_length);
    SecureZeroMemory(session->input + session->input_length, written);
}

static DWORD pump_input(ssh_session *session)
{
    DWORD written;
    if (session->input_writing != 0)
    {
        if (!GetOverlappedResult(session->input_pipe, &session->input_io, &written, FALSE))
        {
            DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE)
            {
                return ERROR_SUCCESS;
            }
            session->input_writing = 0;
            return error;
        }
        session->input_writing = 0;
        remove_written_input(session, written);
    }
    if (session->input_length == 0)
    {
        return ERROR_SUCCESS;
    }
    ResetEvent(session->input_io.hEvent);
    if (WriteFile(session->input_pipe, session->input, session->input_length, &written,
                  &session->input_io))
    {
        remove_written_input(session, written);
        return ERROR_SUCCESS;
    }
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        session->input_writing = session->input_length;
        return ERROR_SUCCESS;
    }
    return error;
}

DWORD ssh_session_write(ssh_session *session, const char *input, DWORD length)
{
    if (session->process == NULL || session->close_thread != NULL)
    {
        return ERROR_BROKEN_PIPE;
    }
    if (length > sizeof session->input - session->input_length)
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    memcpy(session->input + session->input_length, input, length);
    session->input_length += length;
    return pump_input(session);
}

static DWORD WINAPI finish_pseudoconsole(LPVOID console)
{
    close_pseudoconsole(console);
    return 0;
}

BOOL ssh_session_poll(ssh_session *session, DWORD *exit_code)
{
    if (session->process == NULL)
    {
        return FALSE;
    }
    if (WaitForSingleObject(session->process, 0) != WAIT_OBJECT_0)
    {
        pump_input(session);
        return FALSE;
    }
    if (session->console != NULL)
    {
        /* 退出还会产生尾部输出，关闭线程运行时 UI 继续读取。 */
        session->close_thread = CreateThread(NULL, 0, finish_pseudoconsole, session->console, 0, NULL);
        if (session->close_thread != NULL)
        {
            session->console = NULL;
        }
    }
    if (session->close_thread != NULL &&
        (WaitForSingleObject(session->close_thread, 0) != WAIT_OBJECT_0 ||
         PeekNamedPipe(session->output_pipe, NULL, 0, NULL, NULL, NULL)))
    {
        return FALSE;
    }
    if (!GetExitCodeProcess(session->process, exit_code))
    {
        *exit_code = GetLastError();
    }
    close_session_handles(session);
    return TRUE;
}

DWORD ssh_session_stop(ssh_session *session)
{
    if (session->process != NULL && WaitForSingleObject(session->process, 0) != WAIT_OBJECT_0 &&
        !TerminateProcess(session->process, 0))
    {
        DWORD error = GetLastError();
        if (WaitForSingleObject(session->process, 0) != WAIT_OBJECT_0)
        {
            return error;
        }
    }
    close_session_handles(session);
    return ERROR_SUCCESS;
}
