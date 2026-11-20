#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include <windows.h>

#define SSH_HOST_CAPACITY 256
#define SSH_USER_CAPACITY 128
#define SSH_PORT_CAPACITY 6

typedef struct
{
    HANDLE process;
    char host[SSH_HOST_CAPACITY];
    char user[SSH_USER_CAPACITY];
    char port[SSH_PORT_CAPACITY];
} ssh_session;

/* 返回 Win32 错误码。连接信息由 OpenSSH 读取，密码由终端处理。 */
DWORD ssh_session_start(ssh_session *session);
/* 子进程结束时释放句柄，并通过 exit_code 返回退出状态。 */
BOOL ssh_session_poll(ssh_session *session, DWORD *exit_code);
/* 只关闭本会话启动的 OpenSSH 客户端。 */
DWORD ssh_session_stop(ssh_session *session);

#endif
