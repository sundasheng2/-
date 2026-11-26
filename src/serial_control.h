#ifndef STM32_SERIAL_CONTROL_H
#define STM32_SERIAL_CONTROL_H

#include <stdbool.h>
#include <windows.h>

#define SERIAL_CONTROL_REQUEST_CAPACITY 1200
#define SERIAL_CONTROL_REPLY_CAPACITY 16384

/* 单实例本机管道，仅允许当前 Windows 用户访问。各函数由 UI 线程调用。 */
DWORD serial_control_start(void);
bool serial_control_read(char request[SERIAL_CONTROL_REQUEST_CAPACITY]);
/* UTF-8 响应，长度小于 SERIAL_CONTROL_REPLY_CAPACITY；每次请求只回复一次。 */
void serial_control_reply(const char *reply);
void serial_control_close(void);

#endif
