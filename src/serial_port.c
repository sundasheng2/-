#include "serial_port.h"

#include <stdio.h>
#include <string.h>

DWORD serial_open(serial_port *serial, const char *port_name, const serial_config *config)
{
    char path[32];
    DCB settings = {0};
    COMMTIMEOUTS timeouts = {0};
    DWORD error;

    serial_close(serial);
    snprintf(path, sizeof path, "\\\\.\\%s", port_name);
    serial->handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, NULL);
    if (serial->handle == INVALID_HANDLE_VALUE)
    {
        return GetLastError();
    }

    ZeroMemory(&serial->read_io, sizeof serial->read_io);
    ZeroMemory(&serial->write_io, sizeof serial->write_io);
    serial->read_io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (serial->read_io.hEvent == NULL)
    {
        goto failed;
    }
    serial->write_io.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (serial->write_io.hEvent == NULL)
    {
        goto failed;
    }

    settings.DCBlength = sizeof settings;
    if (!GetCommState(serial->handle, &settings))
    {
        goto failed;
    }
    settings.BaudRate = config->baud_rate;
    settings.ByteSize = config->data_bits;
    settings.Parity = config->parity;
    settings.StopBits = config->stop_bits;
    settings.fBinary = TRUE;
    settings.fParity = config->parity != NOPARITY;
    settings.fErrorChar = FALSE;
    settings.fNull = FALSE;
    settings.fAbortOnError = FALSE;
    settings.fOutX = FALSE;
    settings.fInX = FALSE;
    settings.fOutxDsrFlow = FALSE;
    settings.fDsrSensitivity = FALSE;
    settings.fOutxCtsFlow = config->rts_cts != FALSE;
    settings.fRtsControl = RTS_CONTROL_ENABLE;
    if (config->rts_cts)
    {
        settings.fRtsControl = RTS_CONTROL_HANDSHAKE;
    }
    settings.fDtrControl = DTR_CONTROL_ENABLE;

    /* 有数据即返回，空闲时最多留一个等待首字节的读取请求。 */
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutConstant = 1000;
    if (!SetCommState(serial->handle, &settings) || !SetCommTimeouts(serial->handle, &timeouts))
    {
        goto failed;
    }
    return ERROR_SUCCESS;

failed:
    error = GetLastError();
    serial_close(serial);
    return error;
}

void serial_close(serial_port *serial)
{
    DWORD ignored;

    if (serial_is_open(serial))
    {
        /* 取消后等待驱动归还 OVERLAPPED 和缓冲区，再关闭事件。 */
        CancelIoEx(serial->handle, NULL);
        if (serial->reading)
        {
            GetOverlappedResult(serial->handle, &serial->read_io, &ignored, TRUE);
        }
        if (serial->writing)
        {
            GetOverlappedResult(serial->handle, &serial->write_io, &ignored, TRUE);
        }
        CloseHandle(serial->handle);
        serial->handle = NULL;
    }
    if (serial->read_io.hEvent != NULL)
    {
        CloseHandle(serial->read_io.hEvent);
        serial->read_io.hEvent = NULL;
    }
    if (serial->write_io.hEvent != NULL)
    {
        CloseHandle(serial->write_io.hEvent);
        serial->write_io.hEvent = NULL;
    }
    serial->reading = FALSE;
    serial->writing = FALSE;
}

BOOL serial_is_open(const serial_port *serial)
{
    return serial->handle != NULL && serial->handle != INVALID_HANDLE_VALUE;
}

BOOL serial_is_sending(const serial_port *serial)
{
    return serial->writing;
}

DWORD serial_send(serial_port *serial, const void *data, DWORD length)
{
    DWORD written;
    DWORD error;

    if (!serial_is_open(serial))
    {
        return ERROR_INVALID_HANDLE;
    }
    if (serial->writing)
    {
        return ERROR_BUSY;
    }
    if (length == 0 || length > sizeof serial->write_buffer)
    {
        return ERROR_INVALID_PARAMETER;
    }

    memcpy(serial->write_buffer, data, length);
    ResetEvent(serial->write_io.hEvent);
    if (WriteFile(serial->handle, serial->write_buffer, length, &written, &serial->write_io))
    {
        if (written == length)
        {
            return ERROR_SUCCESS;
        }
        return ERROR_WRITE_FAULT;
    }
    error = GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        serial->writing = TRUE;
        serial->write_length = length;
        return ERROR_SUCCESS;
    }
    return error;
}

DWORD serial_poll(serial_port *serial, const unsigned char **data, DWORD *length)
{
    DWORD written;
    DWORD error;

    *data = serial->read_buffer;
    *length = 0;
    if (!serial_is_open(serial))
    {
        return ERROR_SUCCESS;
    }

    if (serial->writing)
    {
        if (GetOverlappedResult(serial->handle, &serial->write_io, &written, FALSE))
        {
            serial->writing = FALSE;
            if (written != serial->write_length)
            {
                return ERROR_WRITE_FAULT;
            }
        }
        else
        {
            error = GetLastError();
            if (error != ERROR_IO_INCOMPLETE)
            {
                serial->writing = FALSE;
                return error;
            }
        }
    }

    if (serial->reading)
    {
        if (GetOverlappedResult(serial->handle, &serial->read_io, length, FALSE))
        {
            serial->reading = FALSE;
            return ERROR_SUCCESS;
        }
        error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE)
        {
            return ERROR_SUCCESS;
        }
        serial->reading = FALSE;
        return error;
    }

    ResetEvent(serial->read_io.hEvent);
    if (ReadFile(serial->handle, serial->read_buffer, sizeof serial->read_buffer, length, &serial->read_io))
    {
        return ERROR_SUCCESS;
    }
    error = GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        serial->reading = TRUE;
        return ERROR_SUCCESS;
    }
    return error;
}
