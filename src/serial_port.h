#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <windows.h>

typedef struct
{
    DWORD baud_rate;
    BYTE data_bits;
    BYTE parity;
    BYTE stop_bits;
    BOOL rts_cts;
} serial_config;

#define SERIAL_READ_CAPACITY 4096
#define SERIAL_WRITE_CAPACITY 2048

/* 每个实例起始为零。打开到关闭期间不得复制或移动，驱动会持有缓冲区地址。 */
typedef struct
{
    HANDLE handle;
    OVERLAPPED read_io;
    OVERLAPPED write_io;
    BOOL reading;
    BOOL writing;
    DWORD write_length;
    unsigned char read_buffer[SERIAL_READ_CAPACITY];
    unsigned char write_buffer[SERIAL_WRITE_CAPACITY];
} serial_port;

/* 所有调用在 UI 线程完成。驱动异步收发，轮询接口不会等待数据。 */
DWORD serial_open(serial_port *serial, const char *port_name, const serial_config *config);
void serial_close(serial_port *serial);
BOOL serial_is_open(const serial_port *serial);
BOOL serial_is_sending(const serial_port *serial);
/* 成功表示提交到驱动。发送数据由模块复制，调用者可立即复用缓冲区。 */
DWORD serial_send(serial_port *serial, const void *data, DWORD length);
/* 成功时 length 为接收长度，零表示暂无数据。指针在实例下次轮询前有效。 */
DWORD serial_poll(serial_port *serial, const unsigned char **data, DWORD *length);

#endif
