#include "serial_control.h"

#include <string.h>

#define CLIENT_IDLE_TIMEOUT_MS 5000

static HANDLE pipe_handle = INVALID_HANDLE_VALUE;
static ULONGLONG client_since;
static bool client_connected;
static bool replied;

DWORD serial_control_start(void)
{
    HANDLE token;
    union
    {
        TOKEN_USER user;
        BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    } identity;
    DWORD bytes;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return GetLastError();
    }
    BOOL found = GetTokenInformation(token, TokenUser, &identity, sizeof identity, &bytes);
    DWORD error = found ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!found)
    {
        return error;
    }

    union
    {
        ACL acl;
        BYTE storage[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    } access;
    SECURITY_DESCRIPTOR descriptor;
    if (!InitializeAcl(&access.acl, sizeof access, ACL_REVISION) ||
        !AddAccessAllowedAce(&access.acl, ACL_REVISION, GENERIC_READ | GENERIC_WRITE,
                            identity.user.User.Sid) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, &access.acl, FALSE))
    {
        return GetLastError();
    }
    SECURITY_ATTRIBUTES security = {sizeof security, &descriptor, FALSE};
    pipe_handle = CreateNamedPipeW(L"\\\\.\\pipe\\stm32-serial-console-ai",
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT |
                                   PIPE_REJECT_REMOTE_CLIENTS, 1, SERIAL_CONTROL_REPLY_CAPACITY,
                                   SERIAL_CONTROL_REQUEST_CAPACITY, 0, &security);
    return pipe_handle != INVALID_HANDLE_VALUE ? ERROR_SUCCESS : GetLastError();
}

static void disconnect_client(void)
{
    DisconnectNamedPipe(pipe_handle);
    client_connected = false;
    replied = false;
}

bool serial_control_read(char request[SERIAL_CONTROL_REQUEST_CAPACITY])
{
    if (pipe_handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    if (!client_connected)
    {
        if (!ConnectNamedPipe(pipe_handle, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            if (GetLastError() == ERROR_NO_DATA)
            {
                disconnect_client();
            }
            return false;
        }
        client_connected = true;
        client_since = GetTickCount64();
    }

    if (replied)
    {
        /* 客户端读完响应后关闭。过早断开会丢弃尚未读取的响应。 */
        if (!PeekNamedPipe(pipe_handle, NULL, 0, NULL, NULL, NULL) ||
            GetTickCount64() - client_since >= CLIENT_IDLE_TIMEOUT_MS)
        {
            disconnect_client();
        }
        return false;
    }
    DWORD length;
    if (!ReadFile(pipe_handle, request, SERIAL_CONTROL_REQUEST_CAPACITY - 1, &length, NULL))
    {
        DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA)
        {
            serial_control_reply("ERR Request too large\n");
        }
        else if (error != ERROR_NO_DATA || GetTickCount64() - client_since >= CLIENT_IDLE_TIMEOUT_MS)
        {
            disconnect_client();
        }
        return false;
    }
    if (length == 0 || memchr(request, '\0', length) != NULL)
    {
        serial_control_reply("ERR Invalid request\n");
        return false;
    }
    request[length] = '\0';
    return true;
}

void serial_control_reply(const char *reply)
{
    DWORD length = (DWORD)strlen(reply);
    DWORD written;
    if (!WriteFile(pipe_handle, reply, length, &written, NULL) || written != length)
    {
        disconnect_client();
        return;
    }
    replied = true;
    client_since = GetTickCount64();
}

void serial_control_close(void)
{
    if (pipe_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe_handle);
        pipe_handle = INVALID_HANDLE_VALUE;
    }
    client_connected = false;
    replied = false;
}
