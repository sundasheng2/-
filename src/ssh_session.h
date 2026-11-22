#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include <windows.h>

#define SSH_HOST_CAPACITY 256
#define SSH_USER_CAPACITY 128
#define SSH_PORT_CAPACITY 6
#define SSH_INPUT_CAPACITY 8192
#define SSH_TERMINAL_ROWS 32

typedef struct
{
    HANDLE process;
    HANDLE console;
    HANDLE input_pipe;
    HANDLE output_pipe;
    HANDLE close_thread;
    OVERLAPPED input_io;
    DWORD input_length;
    DWORD input_writing;
    char input[SSH_INPUT_CAPACITY];
    char host[SSH_HOST_CAPACITY];
    char user[SSH_USER_CAPACITY];
    char port[SSH_PORT_CAPACITY];
} ssh_session;

/* 返回 Win32 错误码。连接信息由 OpenSSH 读取，密码由终端处理。 */
DWORD ssh_session_start(ssh_session *session);
/* 非阻塞读写。写入成功表示已复制到本会话队列。 */
DWORD ssh_session_read(ssh_session *session, char *output, DWORD capacity);
DWORD ssh_session_write(ssh_session *session, const char *input, DWORD length);
/* 推进写入及退出清理。读完剩余输出后返回退出状态。 */
BOOL ssh_session_poll(ssh_session *session, DWORD *exit_code);
/* 只关闭本会话启动的 OpenSSH 客户端。 */
DWORD ssh_session_stop(ssh_session *session);

#endif
