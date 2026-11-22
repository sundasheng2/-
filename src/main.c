#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_icons.h>

#include "serial_port.h"
#include "ssh_session.h"
#include "favorites.h"
#include "text_decode.h"
#include "serial_ansi.h"
#include "log_archive.h"

#include <commdlg.h>
#include <ctype.h>
#include <dwmapi.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_LINES 8192
#define LOG_BYTES (512 * 1024)
#define RX_BUDGET_BYTES (64 * 1024)
#define PARTIAL_LINE_TIMEOUT_MS 150
#define HEX_COLUMNS 16
#define CAPTURE_TIMESTAMP_LENGTH (sizeof "[00:00:00.000] " - 1)

#define MAX_SESSIONS 8
#define LOG_FONT_CHOICES 4

typedef struct
{
    DWORD id;
    const char *name;
    cui_theme_mode mode;
    cui_accent accent;
    cui_color control_surface;
    cui_color control_border;
    cui_color log_surface;
    cui_color log_text;
} appearance;

/* ID 保存到当前用户的配置中。2 和 3 曾用于已移除的主题，不再复用。 */
enum { THEME_DARK, THEME_LIGHT, THEME_OLIVE = 4 };

static const appearance themes[] = {
    {THEME_DARK, "经典深色", CUI_THEME_DARK, CUI_ACCENT_BLUE, CUI_RGB(0x25282d), CUI_RGB(0x3a3e46),
     CUI_RGB(0x202227), CUI_RGB(0x91dda8)},
    {THEME_LIGHT, "明亮", CUI_THEME_LIGHT, CUI_ACCENT_BLUE, CUI_RGB(0xf1f2f3), CUI_RGB(0xd7dade),
     CUI_RGB(0xf1f2f3), CUI_RGB(0x24282d)},
    {THEME_OLIVE, "米白橄榄", CUI_THEME_LIGHT, CUI_ACCENT_GREEN, CUI_RGB(0xece7d9), CUI_RGB(0xd8d0bc),
     CUI_RGB(0xf0ebdf), CUI_RGB(0x475233)},
};

static const wchar_t app_settings_key[] = L"Software\\STM32SerialConsole";
static DWORD theme_index;
static bool theme_save_failed;

/* 数值对应保存对话框中文件类型的一基索引。 */
typedef enum
{
    CAPTURE_TEXT = 1,
    CAPTURE_HEX,
    CAPTURE_RAW,
} capture_format;

typedef enum
{
    SESSION_SERIAL,
    SESSION_SSH,
} session_kind;

typedef struct
{
    int id;
    session_kind kind;
    ssh_session ssh;
    cui_terminal_buffer ssh_terminal;
    serial_port serial;
    char port_names[SERIAL_MAX_PORT][16];
    const char *port_items[SERIAL_MAX_PORT];
    int port_count;
    int port_index;
    char baud_input[16];
    char tx_input[SERIAL_COMMAND_CAPACITY];
    char status[192];
    bool status_error_pending;
    char access_error[128];
    int data_bits_index;
    int parity_index;
    int stop_bits_index;
    bool flow_control;
    bool tx_hex;
    int ending_index;
    int view_index;
    int encoding_index;
    unsigned long long received_bytes;
    unsigned long long submitted_bytes;
    FILE *capture;
    capture_format capture_mode;
    wchar_t capture_root[MAX_PATH];
    log_archive capture_archive;
    bool capture_after_cr;
    bool capture_line_started;
    ULONGLONG last_rx_ms;
    double last_rx_time;
    char pending_text[TEXT_DECODE_INPUT_CAPACITY];
    size_t pending_length;
    serial_ansi rx_ansi;
    cui_log_buffer text_log;
    cui_log_buffer hex_log;
    union
    {
        cui_log_line text_lines[LOG_LINES];
        cui_terminal_line ssh_lines[LOG_LINES];
    };
    cui_log_line hex_lines[LOG_LINES];
    char text_storage[LOG_BYTES];
    char hex_storage[LOG_BYTES];
} serial_session;

/* 串口驱动和日志缓冲区引用固定槽位。切换或删除标签只移动指针。 */
static serial_session session_slots[MAX_SESSIONS];
static serial_session *tabs[MAX_SESSIONS];
static int tab_count;
static int selected_tab;
static int next_session_id;
static int menu_session_id;
static bool show_settings;
static bool show_log_recording_settings;
static wchar_t log_folder[MAX_PATH];
static const char *log_folder_error;
static bool show_new_session;
static int new_session_kind;
static bool show_log_tools;
static bool show_command_editor;
static int editing_command_index;
static favorite_command command_draft;
static char command_error[128];
static int settings_page;
static cui_font log_fonts[LOG_FONT_CHOICES] = {CUI_FONT_MONO};
static const char *log_font_names[LOG_FONT_CHOICES] = {"Geist Mono"};
static int log_font_count = 1;
static int log_font_index;
static double log_font_size = 16.0;
static favorites saved;
static wchar_t favorites_path[MAX_PATH];
static int saved_session_index = -1;
static int saved_command_index = -1;

static void refresh_ports(serial_session *current);
static unsigned long parse_baud_rate(serial_session *current);
static void toggle_connection(serial_session *current);
static void poll_serial(serial_session *current);
static void send_input(serial_session *current);
static bool send_command(serial_session *current, const char *text, bool hex, int ending_index);
static void toggle_capture(serial_session *current);
static bool update_daily_capture(serial_session *current, const SYSTEMTIME *time);
static const char *capture_label(const serial_session *current);
static bool write_capture(serial_session *current, const unsigned char *data, size_t length);
static bool write_text_capture(serial_session *current, const unsigned char *data, size_t length,
                               const char *timestamp);
static size_t format_hex_row(char *row, const unsigned char *data, size_t length);
static UINT_PTR CALLBACK capture_save_hook(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam);
static serial_session *create_session(session_kind kind);
static void close_session(serial_session *current);
static void close_tab(int index);
static void poll_sessions(void);
static void draw_workspace(cui_ctx *ui);
static void draw_app_settings(cui_ctx *ui);
static void apply_theme_palette(cui_ctx *ui, DWORD theme_id);
static void draw_log_recording_settings(cui_app *app, cui_ctx *ui);
static void save_log_folder(const wchar_t *folder);
static void draw_session_menu(cui_ctx *ui, bool requested);
static void draw_new_session(cui_ctx *ui);
static void draw_ssh_session(serial_session *current, cui_ctx *ui);
static void poll_ssh_session(serial_session *current);
static void stop_ssh_session(serial_session *current);
static void draw_session(serial_session *current, cui_ctx *ui);
static void report_session_error(serial_session *current, const char *format, ...);
static void draw_serial_access_error(const serial_session *current, cui_ctx *ui);
static void draw_quick_commands(serial_session *current, cui_ctx *ui, bool connected);
static void draw_command_editor(serial_session *current, cui_ctx *ui);
static void open_command_editor(const serial_session *current, int index);
static bool save_command_draft(serial_session *current);
static void remove_command(serial_session *current, int index);
static void draw_settings(serial_session *current, cui_ctx *ui);
static void draw_serial_settings(serial_session *current, cui_ctx *ui, bool connected);
static void draw_log_settings(serial_session *current, cui_ctx *ui);
static void draw_favorites(serial_session *current, cui_ctx *ui, bool connected);
static void save_current_session(serial_session *current);
static void load_selected_session(serial_session *current);
static void save_current_command(serial_session *current);
static void load_selected_command(serial_session *current);
static void remove_favorite(serial_session *current, bool session);
static bool save_favorites(serial_session *current, const favorites *updated, const char *message);
static void flush_text(serial_session *current, bool finish);
static double local_clock_seconds(void);
static double clock_seconds(const SYSTEMTIME *time);
static void append_hex(serial_session *current, const char *direction,
                       const unsigned char *data, size_t length);
static bool parse_hex(const char *input, unsigned char *output, size_t *length);
static bool load_chinese_font(cui_ctx *ui);
static cui_font load_system_font(cui_ctx *ui, const char *directory, const char *filename);

int main(void)
{
    DWORD preferred_theme;
    DWORD theme_size = sizeof preferred_theme;
    if (RegGetValueW(HKEY_CURRENT_USER, app_settings_key, L"Theme", RRF_RT_REG_DWORD,
                     NULL, &preferred_theme, &theme_size) == ERROR_SUCCESS)
    {
        for (DWORD i = 0; i < sizeof themes / sizeof themes[0]; i++)
        {
            if (themes[i].id == preferred_theme)
            {
                theme_index = i;
                break;
            }
        }
    }
    DWORD folder_size = sizeof log_folder;
    RegGetValueW(HKEY_CURRENT_USER, app_settings_key, L"LogFolder", RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                 NULL, log_folder, &folder_size);
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "STM32 串口调试助手",
        .width = 1100,
        .height = 720,
        .min_width = 800,
        .min_height = 540,
        .continuous = true,
        .theme = themes[theme_index].mode,
        .accent = themes[theme_index].accent,
    });

    if (app == NULL)
    {
        MessageBoxW(NULL, L"无法打开应用窗口。", L"STM32 串口调试助手",
                    MB_ICONERROR);
        return 1;
    }
    if (!load_chinese_font(cui_app_ui(app)))
    {
        MessageBoxW(NULL, L"找不到可用的中文字体。请安装 Windows 简体中文补充字体。",
                    L"STM32 串口调试助手", MB_ICONERROR);
        cui_app_destroy(app);
        return 1;
    }
    apply_theme_palette(cui_app_ui(app), themes[theme_index].id);

    serial_session *initial = create_session(SESSION_SERIAL);
    if (!favorites_load_default(favorites_path, &saved))
    {
        snprintf(initial->status, sizeof initial->status, "%s",
                 favorites_path[0] ? "无法读取串口收藏文件" : "无法定位串口收藏文件");
    }
    while (cui_app_frame(app))
    {
        poll_sessions();
        cui_ctx *ui = cui_app_ui(app);
        DWORD previous_theme = theme_index;
        /* CUI 在帧开始时推进主题过渡，随后应用自定义色板。 */
        apply_theme_palette(ui, themes[theme_index].id);
        draw_workspace(ui);
        draw_log_recording_settings(app, ui);
        if (theme_index != previous_theme)
        {
            const appearance *theme = &themes[theme_index];
            cui_set_theme(ui, theme->mode);
            cui_set_accent(ui, theme->accent);

            HWND window = cui_app_native_window(app);
            BOOL dark = theme->mode == CUI_THEME_DARK;
            if (FAILED(DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                             &dark, sizeof dark)))
            {
                /* 旧版 Windows 10 使用属性 19。 */
                DwmSetWindowAttribute(window, 19, &dark, sizeof dark);
            }
        }
        /* CUI 空闲时也要处理串口完成事件，限制软件渲染时的轮询频率。 */
        Sleep(8);
    }
    for (int i = 0; i < tab_count; i++)
    {
        close_session(tabs[i]);
    }
    cui_app_destroy(app);
    return 0;
}

static serial_session *create_session(session_kind kind)
{
    if (tab_count >= MAX_SESSIONS)
    {
        return NULL;
    }

    for (int i = 0; i < MAX_SESSIONS; i++)
    {
        serial_session *current = &session_slots[i];
        if (current->id != 0)
        {
            continue;
        }
        memset(current, 0, sizeof *current);
        current->id = ++next_session_id;
        current->kind = kind;
        current->port_index = -1;
        if (kind == SESSION_SERIAL)
        {
            current->data_bits_index = 1;
            current->ending_index = 3;
            snprintf(current->baud_input, sizeof current->baud_input, "115200");
            cui_log_init(&current->text_log, current->text_lines, LOG_LINES,
                         current->text_storage, sizeof current->text_storage);
            cui_log_init(&current->hex_log, current->hex_lines, LOG_LINES,
                         current->hex_storage, sizeof current->hex_storage);
            refresh_ports(current);
            if (tab_count > 0)
            {
                current->port_index = -1;
            }
        }
        else
        {
            snprintf(current->ssh.port, sizeof current->ssh.port, "22");
            snprintf(current->status, sizeof current->status, "未连接");
            cui_terminal_init(&current->ssh_terminal, current->text_storage, sizeof current->text_storage,
                              current->ssh_lines, LOG_LINES);
            cui_terminal_set_screen_rows(&current->ssh_terminal, SSH_TERMINAL_ROWS);
        }
        selected_tab = tab_count;
        tabs[tab_count++] = current;
        return current;
    }
    return NULL;
}

static void close_session(serial_session *current)
{
    if (current->kind == SESSION_SSH)
    {
        ssh_session_stop(&current->ssh);
    }
    serial_close(&current->serial);
    if (current->capture != NULL)
    {
        fclose(current->capture);
        current->capture = NULL;
    }
    current->id = 0;
}

static void close_tab(int index)
{
    close_session(tabs[index]);
    tab_count--;
    if (index < tab_count)
    {
        memmove(&tabs[index], &tabs[index + 1], (size_t)(tab_count - index) * sizeof tabs[0]);
    }
    if (selected_tab > index || selected_tab >= tab_count)
    {
        selected_tab--;
    }
    if (tab_count == 0)
    {
        create_session(SESSION_SERIAL);
    }
}

static void poll_sessions(void)
{
    for (int i = 0; i < tab_count; i++)
    {
        serial_session *current = tabs[i];
        if (current->kind == SESSION_SERIAL)
        {
            poll_serial(current);
        }
        else
        {
            poll_ssh_session(current);
        }
    }
}

static void draw_workspace(cui_ctx *ui)
{
    cui_doc_tab items[MAX_SESSIONS];
    char titles[MAX_SESSIONS][64];
    bool new_requested = false;
    cui_doc_tabs_result action = {.close = -1, .context = -1, .icon = -1};
    for (int i = 0; i < tab_count; i++)
    {
        const serial_session *current = tabs[i];
        bool connected = serial_is_open(&current->serial) != FALSE;
        if (current->kind == SESSION_SSH)
        {
            snprintf(titles[i], sizeof titles[i], "SSH %.38s##会话%d",
                     current->ssh.host[0] ? current->ssh.host : "会话", current->id);
            items[i] = (cui_doc_tab){
                .title = titles[i], .icon = CUI_ICON_TERMINAL,
                .tooltip = current->ssh.process ? "SSH 终端已启动" : "SSH 未连接",
            };
            continue;
        }
        if (current->port_index >= 0)
        {
            snprintf(titles[i], sizeof titles[i], "%s##会话%d",
                     current->port_names[current->port_index], current->id);
        }
        else
        {
            snprintf(titles[i], sizeof titles[i], "会话 %d##会话%d", current->id, current->id);
        }
        bool recording = current->capture != NULL;
        bool access_denied = current->access_error[0] != '\0';
        const char *icon_tooltip = connected ? "已连接 · 点击断开" :
                                   (current->port_index >= 0 ? "未连接 · 点击连接" : "点击选择串口");
        if (access_denied)
        {
            /* 当前会话已有横幅，避免悬停提示挡住错误详情。 */
            icon_tooltip = i == selected_tab ? NULL : "串口被占用或访问被拒绝 · 点击重试";
        }
        items[i] = (cui_doc_tab){
            .title = titles[i],
            .icon = access_denied ? CUI_ICON_CIRCLE_ALERT : (recording ? CUI_ICON_CIRCLE_DOT :
                    (connected ? CUI_ICON_CIRCLE_CHECK : CUI_ICON_CIRCLE)),
            .icon_action = true,
            .icon_color = connected ? CUI_COLOR_SUCCESS : CUI_COLOR_DANGER,
            .icon_tooltip = icon_tooltip,
            .tooltip = recording ? capture_label(current) :
                                   (connected ? "已连接 · 右键打开会话菜单" :
                                                "未连接 · 右键打开会话菜单"),
        };
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .gap = 8)
    {
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER)
        {
            /* 标签较多时让标签栏内部滚动，给两个图标留出位置。 */
            cui_column(ui, .max_width = cui_viewport_size(ui).x - 120)
            {
                /* SSH 中的 Ctrl+W 交给远端行编辑，其他会话快捷键照常。 */
                bool ssh_word_delete = tabs[selected_tab]->ssh.process != NULL &&
                                       cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_W);
                action = cui_doc_tabs(ui, "serial-sessions", items, tab_count, &selected_tab,
                                      .shortcuts = !show_settings && !show_command_editor &&
                                                   !show_new_session && !ssh_word_delete,
                                      .no_reorder = true);
            }
            new_requested = cui_icon_button(ui, CUI_ICON_PLUS,
                                           .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST,
                                           .disabled = tab_count == MAX_SESSIONS,
                                           .tooltip = tab_count == MAX_SESSIONS ?
                                                      "最多打开 8 个会话" : "新建会话");
            cui_spacer(ui);
            draw_app_settings(ui);
        }
        cui_separator(ui);
        bool menu_requested = action.context >= 0 && !show_settings && !show_new_session;
        if (menu_requested)
        {
            /* 槽位会复用，菜单用会话编号追踪目标。 */
            menu_session_id = tabs[action.context]->id;
        }
        if (action.close >= 0)
        {
            close_tab(action.close);
        }
        else if (action.icon >= 0 && !show_settings && !show_new_session && !show_command_editor)
        {
            serial_session *target = tabs[action.icon];
            if (target->port_index < 0)
            {
                selected_tab = action.icon;
                settings_page = 0;
                show_settings = true;
            }
            else
            {
                bool was_connected = serial_is_open(&target->serial) != FALSE;
                toggle_connection(target);
                if (!was_connected && !serial_is_open(&target->serial))
                {
                    selected_tab = action.icon;
                }
            }
        }
        if (new_requested)
        {
            new_session_kind = SESSION_SERIAL;
            show_new_session = true;
        }
        draw_session_menu(ui, menu_requested);
        serial_session *current = tabs[selected_tab];
        cui_push_id_int(ui, current->id);
        if (current->kind == SESSION_SSH)
        {
            draw_ssh_session(current, ui);
        }
        else
        {
            draw_serial_access_error(current, ui);
            draw_session(current, ui);
            draw_settings(current, ui);
            draw_command_editor(current, ui);
        }
        cui_pop_id(ui);
        draw_new_session(ui);
    }
}

static void draw_app_settings(cui_ctx *ui)
{
    cui_menu(ui, "软件设置", .icon = CUI_ICON_SETTINGS, .icon_only = true,
             .variant = CUI_BUTTON_GHOST, .placement = CUI_PLACE_BOTTOM_END)
    {
        cui_menu(ui, "主题", .icon = CUI_ICON_PALETTE, .variant = CUI_BUTTON_GHOST,
                 .placement = CUI_PLACE_LEFT_START)
        {
            for (DWORD i = 0; i < sizeof themes / sizeof themes[0]; i++)
            {
                if (cui_menu_item(ui, themes[i].name, .checked = i == theme_index))
                {
                    theme_index = i;
                    theme_save_failed = RegSetKeyValueW(HKEY_CURRENT_USER, app_settings_key, L"Theme",
                                                        REG_DWORD, &themes[i].id,
                                                        sizeof themes[i].id) != ERROR_SUCCESS;
                }
            }
        }
        if (theme_save_failed)
        {
            cui_menu_label(ui, "无法保存主题选择");
        }
        if (cui_menu_item(ui, "日志记录设置", .icon = CUI_ICON_FILE_TEXT))
        {
            log_folder_error = NULL;
            show_log_recording_settings = true;
        }
    }
}

static void apply_theme_palette(cui_ctx *ui, DWORD theme_id)
{
    static const cui_color light_palette[CUI_TOKEN_COUNT] = {
        [CUI_TOKEN_BACKGROUND] = CUI_RGB(0xf2f2f3),
        [CUI_TOKEN_SURFACE] = CUI_RGB(0xfdfdfd),
        [CUI_TOKEN_SURFACE_RAISED] = CUI_RGB(0xfdfdfd),
        [CUI_TOKEN_SURFACE_MUTED] = CUI_RGB(0xf8f8f9),
        [CUI_TOKEN_FOREGROUND] = CUI_RGB(0x24282d),
        [CUI_TOKEN_TEXT_SECONDARY] = CUI_RGB(0x505962),
        [CUI_TOKEN_TEXT_MUTED] = CUI_RGB(0x626b73),
        [CUI_TOKEN_BORDER] = CUI_RGB(0xd7dade),
        [CUI_TOKEN_BORDER_SUBTLE] = CUI_RGB(0xe5e7ea),
        [CUI_TOKEN_BORDER_STRONG] = CUI_RGB(0x9ba6ac),
        [CUI_TOKEN_ACCENT] = CUI_RGB(0x007d6a),
        [CUI_TOKEN_ACCENT_STRONG] = CUI_RGB(0x006653),
        [CUI_TOKEN_ACCENT_SUBTLE] = CUI_RGBA(0x007d6a22),
        [CUI_TOKEN_ACCENT_FOREGROUND] = CUI_RGB(0xffffff),
        [CUI_TOKEN_CONTROL_ON] = CUI_RGB(0x007d6a),
        [CUI_TOKEN_CONTROL_GLYPH] = CUI_RGB(0xffffff),
        [CUI_TOKEN_CONTROL_TRACK] = CUI_RGB(0xdfe3e6),
        [CUI_TOKEN_CONTROL_TRACK_HOVER] = CUI_RGB(0xd2d9dc),
        [CUI_TOKEN_FOCUS_RING] = CUI_RGB(0x318d7d),
        [CUI_TOKEN_INFO] = CUI_RGB(0x007d6a),
    };
    static const cui_color olive_palette[CUI_TOKEN_COUNT] = {
        [CUI_TOKEN_BACKGROUND] = CUI_RGB(0xf5f1e8),
        [CUI_TOKEN_SURFACE] = CUI_RGB(0xfbf8f1),
        [CUI_TOKEN_SURFACE_RAISED] = CUI_RGB(0xfcf9f2),
        [CUI_TOKEN_SURFACE_MUTED] = CUI_RGB(0xfaf7ef),
        [CUI_TOKEN_FOREGROUND] = CUI_RGB(0x34362b),
        [CUI_TOKEN_TEXT_SECONDARY] = CUI_RGB(0x62664e),
        [CUI_TOKEN_TEXT_MUTED] = CUI_RGB(0x6d705b),
        [CUI_TOKEN_BORDER] = CUI_RGB(0xddd5c4),
        [CUI_TOKEN_BORDER_SUBTLE] = CUI_RGB(0xe9e2d5),
        [CUI_TOKEN_BORDER_STRONG] = CUI_RGB(0xa49d89),
        [CUI_TOKEN_ACCENT] = CUI_RGB(0x546139),
        [CUI_TOKEN_ACCENT_STRONG] = CUI_RGB(0x414d2b),
        [CUI_TOKEN_ACCENT_SUBTLE] = CUI_RGBA(0x54613928),
        [CUI_TOKEN_ACCENT_FOREGROUND] = CUI_RGB(0xfffbef),
        [CUI_TOKEN_SUCCESS] = CUI_RGB(0x546139),
        [CUI_TOKEN_CONTROL_ON] = CUI_RGB(0x546139),
        [CUI_TOKEN_CONTROL_GLYPH] = CUI_RGB(0xfffbef),
        [CUI_TOKEN_CONTROL_TRACK] = CUI_RGB(0xe1dacb),
        [CUI_TOKEN_CONTROL_TRACK_HOVER] = CUI_RGB(0xd8d1c0),
        [CUI_TOKEN_FOCUS_RING] = CUI_RGB(0x858e68),
    };
    const cui_color *palette;
    switch (theme_id)
    {
    case THEME_LIGHT:
        palette = light_palette;
        break;
    case THEME_OLIVE:
        palette = olive_palette;
        break;
    default:
        return;
    }

    cui_color *colors = cui_theme_get(ui)->colors;
    for (int token = 1; token < CUI_TOKEN_COUNT; token++)
    {
        if (palette[token].a != 0)
        {
            colors[token] = palette[token];
        }
    }
}

static void draw_log_recording_settings(cui_app *app, cui_ctx *ui)
{
    cui_dialog(ui, "日志记录设置", &show_log_recording_settings, .width = 560)
    {
        char path[MAX_PATH * 4];
        cui_text(ui, "默认日志文件夹");
        if (log_folder[0] == L'\0')
        {
            cui_secondary(ui, "未设置，开始记录时手动选择文件。");
        }
        else if (WideCharToMultiByte(CP_UTF8, 0, log_folder, -1, path, sizeof path, NULL, NULL))
        {
            cui_text(ui, path, .width = CUI_FILL);
        }
        cui_secondary(ui, "按 COM 口和本机日期保存 TXT，例如 COM3\\2026-11-22.txt。");
        if (log_folder_error != NULL)
        {
            cui_text(ui, log_folder_error, .color = CUI_COLOR_DANGER);
        }
        cui_dialog_footer(ui)
        {
            if (cui_button(ui, "选择文件夹…"))
            {
                wchar_t selected[MAX_PATH];
                if (cui_app_file_dialog(app, &(cui_file_dialog_opts){
                        .kind = CUI_FILE_FOLDER, .title = "选择默认日志文件夹"}, path, sizeof path))
                {
                    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, selected, MAX_PATH))
                    {
                        save_log_folder(selected);
                    }
                    else
                    {
                        log_folder_error = "无法读取所选路径，请选择较短的文件夹路径。";
                    }
                }
            }
            if (cui_button(ui, "清除默认文件夹", .disabled = log_folder[0] == L'\0'))
            {
                save_log_folder(L"");
            }
            if (cui_button(ui, "完成", .variant = CUI_BUTTON_PRIMARY))
            {
                show_log_recording_settings = false;
            }
        }
    }
}

static void save_log_folder(const wchar_t *folder)
{
    DWORD size = (DWORD)((wcslen(folder) + 1) * sizeof folder[0]);
    LSTATUS error = RegSetKeyValueW(HKEY_CURRENT_USER, app_settings_key, L"LogFolder",
                                    REG_SZ, folder, size);
    if (error == ERROR_SUCCESS)
    {
        wcscpy(log_folder, folder);
        log_folder_error = NULL;
    }
    else
    {
        log_folder_error = "无法保存默认日志文件夹，请检查当前用户的设置权限。";
    }
}

static void draw_session_menu(cui_ctx *ui, bool requested)
{
    if (!cui_context_menu_begin_at_mouse(ui, "session-menu", requested))
    {
        return;
    }
    for (int i = 0; i < tab_count; i++)
    {
        serial_session *target = tabs[i];
        if (target->id != menu_session_id)
        {
            continue;
        }

        if (target->kind == SESSION_SSH)
        {
            cui_menu_label(ui, "SSH 会话");
            if (cui_menu_item(ui, "断开 SSH", .icon = CUI_ICON_SQUARE,
                              .disabled = target->ssh.process == NULL))
            {
                stop_ssh_session(target);
            }
            break;
        }

        char title[32];
        if (target->port_index >= 0)
        {
            snprintf(title, sizeof title, "%s", target->port_names[target->port_index]);
        }
        else
        {
            snprintf(title, sizeof title, "会话 %d", target->id);
        }
        cui_menu_label(ui, title);
        if (cui_menu_item(ui, "设置", .icon = CUI_ICON_SETTINGS))
        {
            selected_tab = i;
            show_settings = true;
        }
        break;
    }
    cui_context_menu_end(ui);
}

static void draw_new_session(cui_ctx *ui)
{
    static const char *types[] = {"串口", "SSH"};
    cui_dialog(ui, "新建会话", &show_new_session, .width = 360)
    {
        cui_tabs(ui, "new-session-type", types, 2, &new_session_kind, .full_width = true);
        cui_dialog_footer(ui)
        {
            if (cui_button(ui, "取消"))
            {
                show_new_session = false;
            }
            if (cui_button(ui, "创建", .variant = CUI_BUTTON_PRIMARY) &&
                create_session((session_kind)new_session_kind) != NULL)
            {
                show_new_session = false;
                if (new_session_kind == SESSION_SERIAL)
                {
                    settings_page = 0;
                    show_settings = true;
                }
            }
        }
    }
}

static void draw_ssh_session(serial_session *current, cui_ctx *ui)
{
    ssh_session *ssh = &current->ssh;
    bool running = ssh->process != NULL;
    cui_row(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 10, .gap = 10,
            .align = CUI_ALIGN_END, .bg = themes[theme_index].control_surface, .border = 1,
            .border_color = themes[theme_index].control_border, .radius = 9)
    {
        cui_input(ui, "ssh-host", ssh->host, sizeof ssh->host, .label = "远程主机",
                  .placeholder = "主机名或 IP 地址", .width = CUI_PX(248), .disabled = running);
        cui_input(ui, "ssh-user", ssh->user, sizeof ssh->user, .label = "用户名",
                  .placeholder = "可留空", .width = CUI_PX(136), .disabled = running);
        cui_input(ui, "ssh-port", ssh->port, sizeof ssh->port, .label = "端口",
                  .width = CUI_PX(84), .disabled = running);
        cui_row(ui, .width = CUI_FILL, .min_height = cui_theme_get(ui)->control_md,
                .gap = 8, .align = CUI_ALIGN_CENTER)
        {
            if (cui_button(ui, running ? "断开" : "连接", .variant = CUI_BUTTON_PRIMARY))
            {
                if (running)
                {
                    stop_ssh_session(current);
                }
                else
                {
                    DWORD error = ssh_session_start(ssh);
                    switch (error)
                    {
                    case ERROR_SUCCESS:
                        cui_terminal_clear(&current->ssh_terminal);
                        snprintf(current->status, sizeof current->status, "SSH 客户端运行中");
                        break;
                    case ERROR_INVALID_PARAMETER:
                        snprintf(current->status, sizeof current->status,
                                 "请检查主机名、用户名和端口（1–65535）");
                        break;
                    case ERROR_FILE_NOT_FOUND:
                    case ERROR_PATH_NOT_FOUND:
                        snprintf(current->status, sizeof current->status,
                                 "找不到 Windows OpenSSH 客户端");
                        break;
                    case ERROR_NOT_SUPPORTED:
                        snprintf(current->status, sizeof current->status,
                                 "内嵌 SSH 需要 Windows 10 1809 或更新版本");
                        break;
                    default:
                        snprintf(current->status, sizeof current->status,
                                 "无法启动 SSH（错误码 %lu）", (unsigned long)error);
                        break;
                    }
                }
            }
            cui_text(ui, current->status, .width = CUI_FILL);
        }
    }
    cui_terminal_result terminal = cui_terminal(ui, "ssh-terminal", &current->ssh_terminal,
                                                .height = -1, .size = 14, .no_input = true,
                                                .raw_input = ssh->process != NULL);
    if (ssh->process != NULL && (terminal.input_length != 0 || terminal.interrupt))
    {
        DWORD error = terminal.interrupt ? ssh_session_write(ssh, "\x03", 1) :
                      ssh_session_write(ssh, terminal.input, (DWORD)terminal.input_length);
        if (error != ERROR_SUCCESS)
        {
            snprintf(current->status, sizeof current->status, "无法输入 SSH 终端（错误码 %lu）",
                     (unsigned long)error);
        }
    }
}

static void poll_ssh_session(serial_session *current)
{
    char output[4096];
    for (DWORD total = 0; total < RX_BUDGET_BYTES;)
    {
        DWORD count = ssh_session_read(&current->ssh, output, sizeof output);
        if (count == 0)
        {
            break;
        }
        cui_terminal_write(&current->ssh_terminal, output, (int)count);
        total += count;
    }
    DWORD exit_code;
    if (ssh_session_poll(&current->ssh, &exit_code))
    {
        snprintf(current->status, sizeof current->status,
                 exit_code == 0 ? "SSH 终端已关闭" : "SSH 已退出（退出码 %lu）",
                 (unsigned long)exit_code);
    }
}

static void stop_ssh_session(serial_session *current)
{
    DWORD error = ssh_session_stop(&current->ssh);
    if (error == ERROR_SUCCESS)
    {
        snprintf(current->status, sizeof current->status, "SSH 已断开");
    }
    else
    {
        snprintf(current->status, sizeof current->status, "无法断开 SSH（错误码 %lu）",
                 (unsigned long)error);
    }
}

static bool load_chinese_font(cui_ctx *ui)
{
    static const struct
    {
        const char *name;
        const char *filename;
    } candidates[] = {
        {"微软雅黑", "msyh.ttc"},
        {"黑体", "simhei.ttf"},
        {"宋体", "simsun.ttc"},
        {"等线", "Deng.ttf"},
    };
    static const char *const mono_names[] = {"Consolas", "Courier New"};
    static const char *const mono_files[] = {"consola.ttf", "cour.ttf"};
    char system_directory[MAX_PATH];
    UINT length = GetWindowsDirectoryA(system_directory, sizeof system_directory);

    if (length == 0 || length >= sizeof system_directory)
    {
        return false;
    }
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++)
    {
        cui_font chinese = load_system_font(ui, system_directory, candidates[i].filename);
        if (chinese < 0)
        {
            continue;
        }
        cui_font_add_fallback(ui, CUI_FONT_BODY, chinese);
        cui_font_add_fallback(ui, CUI_FONT_MEDIUM, chinese);
        cui_font_add_fallback(ui, CUI_FONT_DISPLAY, chinese);
        cui_font_add_fallback(ui, CUI_FONT_MONO, chinese);

        log_font_count = 1;
        for (size_t mono = 0; mono < sizeof mono_files / sizeof mono_files[0]; mono++)
        {
            cui_font font = load_system_font(ui, system_directory, mono_files[mono]);
            if (font >= 0)
            {
                /* 可选等宽字体缺少汉字时，使用同一系统中文字体补齐。 */
                cui_font_add_fallback(ui, font, chinese);
                log_fonts[log_font_count] = font;
                log_font_names[log_font_count++] = mono_names[mono];
            }
        }
        log_fonts[log_font_count] = chinese;
        log_font_names[log_font_count++] = candidates[i].name;
        return true;
    }
    return false;
}

static cui_font load_system_font(cui_ctx *ui, const char *directory, const char *filename)
{
    char path[MAX_PATH];
    int written = snprintf(path, sizeof path, "%s\\Fonts\\%s", directory, filename);
    if (written < 0 || (size_t)written >= sizeof path)
    {
        return -1;
    }
    return cui_font_load_file(ui, path);
}

static void draw_serial_access_error(const serial_session *current, cui_ctx *ui)
{
    if (current->access_error[0] == '\0')
    {
        return;
    }
    cui_color danger = cui_color_of(ui, CUI_COLOR_DANGER);
    cui_row(ui, .width = CUI_FILL, .pad_x = 10, .pad_y = 6, .gap = 8, .radius = 7,
            .bg = cui_alpha(danger, 0.13f), .border = 1, .border_color = cui_alpha(danger, 0.65f))
    {
        cui_text(ui, CUI_ICON_CIRCLE_ALERT, .font = CUI_FONT_ICONS, .color = danger);
        cui_text(ui, current->access_error, .font = CUI_FONT_MEDIUM, .color = danger);
    }
}

static void draw_session(serial_session *current, cui_ctx *ui)
{
    const char *endings[] = {"无换行", "回车（CR）", "换行（LF）", "回车换行"};
    bool connected = serial_is_open(&current->serial) != FALSE;
    bool text_view = current->view_index == 0;
    const appearance *theme = &themes[theme_index];

    if (current->status_error_pending)
    {
        cui_toast(ui, current->status, .tone = CUI_TONE_DANGER);
        current->status_error_pending = false;
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 6)
    {
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
        {
            cui_row(ui, .width = CUI_FILL, .pad_x = 8, .gap = 9,
                    .bg = theme->log_surface, .radius_tl = 5, .radius_tr = 5)
            {
                cui_text(ui, CUI_ICON_TERMINAL, .font = CUI_FONT_ICONS, .color = CUI_COLOR_ACCENT);
                cui_text(ui, "串口日志", .font = CUI_FONT_MEDIUM, .size = 16, .nowrap = true);
                cui_secondary(ui, text_view ? "文本" : "十六进制");
                cui_spacer(ui);
                bool recording = current->capture != NULL;
                if (cui_button(ui, recording ? "停止记录" : "记录接收数据",
                               .icon = recording ? CUI_ICON_SQUARE : CUI_ICON_CIRCLE_DOT,
                               .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST, .selected = recording,
                               .tooltip = recording ? capture_label(current) : "将当前会话收到的数据保存到文件"))
                {
                    toggle_capture(current);
                }
                if (cui_button(ui, "日志工具", .icon = CUI_ICON_SEARCH, .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST, .selected = show_log_tools,
                               .tooltip = "搜索、筛选、滚动、复制和清空"))
                {
                    show_log_tools = !show_log_tools;
                }
            }
            if (show_log_tools)
            {
                char counters[128];
                snprintf(counters, sizeof counters, "已接收 %llu 字节   已提交发送 %llu 字节",
                         current->received_bytes, current->submitted_bytes);
                cui_row(ui, .width = CUI_FILL, .pad_x = 8, .pad_y = 3, .gap = 8)
                {
                    cui_text(ui, current->status, .width = CUI_FILL, .nowrap = true,
                             .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_secondary(ui, counters);
                }
            }
            cui_log_view(ui, text_view ? "text-log" : "hex-log",
                         text_view ? &current->text_log : &current->hex_log,
                         .height = -1, .timestamps = true, .no_toolbar = !show_log_tools,
                         .no_level_column = true, .info_color = theme->log_text,
                         .copy_body_prefix = "接收  ",
                         .size = (float)log_font_size, .font = log_fonts[log_font_index]);
        }
        cui_column(ui, .width = CUI_FILL, .gap = 6)
        {
            cui_column(ui, .width = CUI_FILL, .pad_x = 8,
                       .bg = theme->control_surface, .radius = 5)
            {
                draw_quick_commands(current, ui, connected);
            }
            cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_START)
            {
                bool can_send = connected && !serial_is_sending(&current->serial);
                bool clicked = cui_button(ui, "发送", .variant = CUI_BUTTON_PRIMARY,
                                          .disabled = !can_send,
                                          .tooltip = "回车发送，Shift+Enter 换行");
                int submitted = cui_textarea(ui, "send-input", current->tx_input,
                                             sizeof current->tx_input,
                                             .placeholder = current->tx_hex ? "例如 01 03 00 00" :
                                                                              "输入要发送的命令",
                                             .width = CUI_FILL, .height = 80, .enter_submits = true);
                cui_column(ui, .width = CUI_PX(126), .gap = 6, .align = CUI_ALIGN_START)
                {
                    cui_switch(ui, "十六进制", &current->tx_hex);
                    cui_select(ui, "line-ending", endings, 4, &current->ending_index,
                               .width = CUI_FILL, .disabled = current->tx_hex);
                }
                if (can_send && (clicked || (submitted & CUI_SUBMITTED)))
                {
                    send_input(current);
                }
            }
        }
    }
}

static void report_session_error(serial_session *current, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(current->status, sizeof current->status, format, args);
    va_end(args);
    current->status_error_pending = true;
}

static void draw_quick_commands(serial_session *current, cui_ctx *ui, bool connected)
{
    int remove_index = -1;
    cui_row(ui, .width = CUI_FILL, .gap = 8)
    {
        cui_text(ui, "常用命令");
        if (cui_icon_button(ui, CUI_ICON_PLUS, .size = CUI_SIZE_SM,
                            .tooltip = "新增常用命令",
                            .disabled = saved.command_count >= COMMAND_FAVORITES_CAPACITY))
        {
            open_command_editor(current, -1);
        }
        cui_scroll(ui, .id = "quick-command-strip", .width = CUI_FILL, .height = CUI_PX(30),
                   .horizontal = true, .fade_edges = true)
        {
            cui_row(ui, .gap = 6)
            {
                for (int i = 0; i < saved.command_count; i++)
                {
                    const favorite_command *command = &saved.commands[i];
                    const char *name = command->name[0] ? command->name : command->text;
                    char label[40];
                    size_t length = strlen(name);
                    if (length > 32)
                    {
                        length = 32;
                        while (length > 0 && ((unsigned char)name[length] & 0xc0) == 0x80)
                        {
                            length--;
                        }
                        snprintf(label, sizeof label, "%.*s…", (int)length, name);
                    }
                    else
                    {
                        snprintf(label, sizeof label, "%s", name);
                    }

                    cui_push_id_int(ui, i);
                    if (cui_button(ui, label, .size = CUI_SIZE_SM,
                                   .tooltip = "发送到当前会话，右键编辑或删除"))
                    {
                        if (connected)
                        {
                            send_command(current, command->text, command->hex,
                                         command->ending_index);
                        }
                        else
                        {
                            snprintf(current->status, sizeof current->status,
                                     "请连接串口后再发送命令");
                        }
                    }
                    cui_context_menu(ui, "quick-command-menu")
                    {
                        if (cui_menu_item(ui, "编辑", .icon = CUI_ICON_PENCIL))
                        {
                            open_command_editor(current, i);
                        }
                        if (cui_menu_item(ui, "删除", .icon = CUI_ICON_TRASH_2, .danger = true))
                        {
                            remove_index = i;
                        }
                    }
                    cui_pop_id(ui);
                }
            }
        }
    }
    if (remove_index >= 0)
    {
        remove_command(current, remove_index);
    }
}

static void open_command_editor(const serial_session *current, int index)
{
    editing_command_index = index;
    if (index < 0)
    {
        command_draft = (favorite_command){
            .hex = current->tx_hex,
            .ending_index = current->ending_index,
        };
    }
    else
    {
        command_draft = saved.commands[index];
    }
    command_error[0] = '\0';
    show_command_editor = true;
}

static void draw_command_editor(serial_session *current, cui_ctx *ui)
{
    const char *endings[] = {"无换行", "回车（CR）", "换行（LF）", "回车换行"};
    const char *title = editing_command_index < 0 ? "新增常用命令" : "编辑常用命令";
    cui_dialog(ui, title, &show_command_editor, .width = 550)
    {
        cui_input(ui, "command-name", command_draft.name, sizeof command_draft.name,
                  .label = "按钮名称", .placeholder = "例如 读取状态", .autofocus = true);
        cui_input(ui, "command-content", command_draft.text, sizeof command_draft.text,
                  .label = "发送内容",
                  .placeholder = command_draft.hex ? "例如 01 03 FF" : "输入要发送的文本");
        cui_row(ui, .gap = 12)
        {
            cui_switch(ui, "十六进制", &command_draft.hex);
            cui_select(ui, "command-line-ending", endings, 4, &command_draft.ending_index,
                       .width = CUI_PX(140), .disabled = command_draft.hex);
        }
        if (command_error[0] != '\0')
        {
            cui_text(ui, command_error, .color = CUI_COLOR_DANGER);
        }
        cui_dialog_footer(ui)
        {
            if (cui_button(ui, "取消"))
            {
                show_command_editor = false;
            }
            if (cui_button(ui, "保存", .variant = CUI_BUTTON_PRIMARY) &&
                save_command_draft(current))
            {
                show_command_editor = false;
            }
        }
    }
}

static bool save_command_draft(serial_session *current)
{
    unsigned char hex_bytes[SERIAL_COMMAND_CAPACITY / 2];
    size_t length;
    if (command_draft.name[0] == '\0' || strpbrk(command_draft.name, "\t\r\n") != NULL ||
        strstr(command_draft.name, "##") != NULL)
    {
        snprintf(command_error, sizeof command_error, "请输入有效的按钮名称");
        return false;
    }
    if (command_draft.text[0] == '\0' || strpbrk(command_draft.text, "\r\n") != NULL)
    {
        snprintf(command_error, sizeof command_error, "请输入单行发送内容");
        return false;
    }
    if (command_draft.hex && (!parse_hex(command_draft.text, hex_bytes, &length) || length == 0))
    {
        snprintf(command_error, sizeof command_error, "请输入成对的十六进制数字，例如 01 03 FF");
        return false;
    }

    favorites updated = saved;
    int index = editing_command_index;
    if (index < 0)
    {
        if (updated.command_count >= COMMAND_FAVORITES_CAPACITY)
        {
            snprintf(command_error, sizeof command_error, "常用命令已满");
            return false;
        }
        index = updated.command_count++;
    }
    updated.commands[index] = command_draft;
    if (!save_favorites(current, &updated, "已保存常用命令"))
    {
        snprintf(command_error, sizeof command_error, "无法保存收藏，请检查程序目录");
        return false;
    }
    saved_command_index = index;
    return true;
}

static void draw_settings(serial_session *current, cui_ctx *ui)
{
    const char *pages[] = {"串口", "日志", "会话与命令"};
    bool connected = serial_is_open(&current->serial) != FALSE;

    cui_dialog(ui, "设置", &show_settings, .width = 728)
    {
        cui_tabs(ui, "settings-pages", pages, 3, &settings_page);
        switch (settings_page)
        {
        case 0:
            draw_serial_settings(current, ui, connected);
            break;
        case 1:
            draw_log_settings(current, ui);
            break;
        case 2:
            draw_favorites(current, ui, connected);
            cui_secondary(ui, current->status);
            break;
        }
        cui_dialog_footer(ui)
        {
            if (cui_button(ui, "完成", .variant = CUI_BUTTON_PRIMARY))
            {
                show_settings = false;
            }
        }
    }
}

static void draw_serial_settings(serial_session *current, cui_ctx *ui, bool connected)
{
    const char *data_bits[] = {"7", "8"};
    const char *parities[] = {"无校验", "奇校验", "偶校验"};
    const char *stop_bits[] = {"1", "2"};

    cui_row(ui, .width = CUI_FILL, .gap = 8)
    {
        if (cui_select(ui, "port", current->port_items, current->port_count, &current->port_index,
                       .placeholder = "选择串口", .width = CUI_PX(124), .disabled = connected))
        {
            current->access_error[0] = '\0';
        }
        if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .tooltip = "刷新串口", .disabled = connected))
        {
            refresh_ports(current);
        }
        cui_input(ui, "baud", current->baud_input, sizeof current->baud_input,
                  .placeholder = "波特率", .width = CUI_PX(118),
                  .suffix = "波特率", .disabled = connected);
        if (cui_button(ui, connected ? "断开" : "连接", .variant = CUI_BUTTON_PRIMARY,
                       .disabled = !connected && current->port_index < 0))
        {
            toggle_connection(current);
        }
    }
    cui_separator(ui);
    cui_row(ui, .gap = 12, .align = CUI_ALIGN_START)
    {
        cui_select(ui, "data-bits", data_bits, 2, &current->data_bits_index,
                   .label = "数据位", .width = CUI_PX(100), .disabled = connected);
        cui_select(ui, "parity", parities, 3, &current->parity_index,
                   .label = "校验方式", .width = CUI_PX(140), .disabled = connected);
        cui_select(ui, "stop-bits", stop_bits, 2, &current->stop_bits_index,
                   .label = "停止位", .width = CUI_PX(100), .disabled = connected);
    }
    cui_switch(ui, "硬件流控（RTS/CTS）", &current->flow_control, .disabled = connected);
    if (current->access_error[0] != '\0')
    {
        draw_serial_access_error(current, ui);
    }
    else
    {
        cui_secondary(ui, current->status);
    }
    if (connected)
    {
        cui_secondary(ui, "断开连接后可修改串口参数");
    }
}

static void draw_log_settings(serial_session *current, cui_ctx *ui)
{
    const char *views[] = {"文本", "十六进制"};
    const char *encodings[] = {"自动", "UTF-8", "GBK"};

    cui_row(ui, .gap = 12, .align = CUI_ALIGN_START)
    {
        cui_select(ui, "view-mode", views, 2, &current->view_index,
                   .label = "显示模式", .width = CUI_PX(164));
        cui_select(ui, "rx-encoding", encodings, 3, &current->encoding_index,
                   .label = "接收编码", .width = CUI_PX(164),
                   .disabled = current->view_index != 0);
    }
    cui_row(ui, .gap = 12, .align = CUI_ALIGN_START)
    {
        cui_select(ui, "log-font", log_font_names, log_font_count, &log_font_index,
                   .label = "日志字体", .width = CUI_PX(164));
        cui_number(ui, "log-font-size", &log_font_size, 10, 32,
                   .label = "字号", .step = 1, .width = CUI_PX(164));
    }
}

static void draw_favorites(serial_session *current, cui_ctx *ui, bool connected)
{
    const char *sessions[FAVORITES_CAPACITY];
    const char *commands[COMMAND_FAVORITES_CAPACITY];
    char labels[FAVORITES_CAPACITY][72];
    static const char parities[] = "NOE";

    for (int i = 0; i < saved.session_count; i++)
    {
        const favorite_session *session = &saved.sessions[i];
        snprintf(labels[i], sizeof labels[i], "COM%d / %lu / %d%c%d%s",
                 session->port_number, session->baud, session->data_bits_index + 7,
                 parities[session->parity_index], session->stop_bits_index + 1,
                 session->flow_control ? " / 硬件流控" : "");
        sessions[i] = labels[i];
    }
    for (int i = 0; i < saved.command_count; i++)
    {
        commands[i] = saved.commands[i].name[0] ? saved.commands[i].name : saved.commands[i].text;
    }

    cui_row(ui, .gap = 8)
    {
        cui_text(ui, "会话收藏");
        cui_select(ui, "saved-session", sessions, saved.session_count, &saved_session_index,
                   .placeholder = "选择已保存的串口参数", .width = CUI_PX(290));
        if (cui_button(ui, "应用会话", .disabled = connected || saved_session_index < 0))
        {
            load_selected_session(current);
        }
        if (cui_button(ui, "收藏当前会话", .disabled = current->port_index < 0))
        {
            save_current_session(current);
        }
        if (cui_button(ui, "移除会话", .disabled = saved_session_index < 0))
        {
            remove_favorite(current, true);
        }
    }
    cui_row(ui, .gap = 8)
    {
        cui_text(ui, "常用命令");
        cui_select(ui, "saved-command", commands, saved.command_count, &saved_command_index,
                   .placeholder = "选择已保存的命令", .width = CUI_PX(290));
        if (cui_button(ui, "填入发送框", .disabled = saved_command_index < 0))
        {
            load_selected_command(current);
        }
        if (cui_button(ui, "收藏发送框内容", .disabled = current->tx_input[0] == '\0'))
        {
            save_current_command(current);
        }
        if (cui_button(ui, "移除命令", .disabled = saved_command_index < 0))
        {
            remove_favorite(current, false);
        }
    }
}

static void refresh_ports(serial_session *current)
{
    char previous[16] = "";
    char target[1024];
    current->access_error[0] = '\0';

    if (current->port_index >= 0)
    {
        snprintf(previous, sizeof previous, "%s", current->port_names[current->port_index]);
    }
    current->port_count = 0;
    current->port_index = -1;
    for (int number = 1; number <= SERIAL_MAX_PORT; number++)
    {
        snprintf(current->port_names[current->port_count], sizeof current->port_names[current->port_count], "COM%d", number);
        if (QueryDosDeviceA(current->port_names[current->port_count], target, sizeof target))
        {
            current->port_items[current->port_count] = current->port_names[current->port_count];
            if (strcmp(previous, current->port_names[current->port_count]) == 0)
            {
                current->port_index = current->port_count;
            }
            current->port_count++;
        }
    }
    if (current->port_index < 0 && current->port_count > 0)
    {
        current->port_index = 0;
    }
    snprintf(current->status, sizeof current->status, "未连接%s",
             current->port_count == 0 ? " | 未找到串口" : "");
}

static unsigned long parse_baud_rate(serial_session *current)
{
    char *end;
    unsigned long baud = strtoul(current->baud_input, &end, 10);
    if (current->baud_input[0] < '1' || current->baud_input[0] > '9' || *end != '\0' || baud > SERIAL_MAX_BAUD)
    {
        return 0;
    }
    return baud;
}

static void save_current_session(serial_session *current)
{
    unsigned long baud = parse_baud_rate(current);
    if (baud == 0)
    {
        snprintf(current->status, sizeof current->status, "请输入 1 到 %lu 之间的波特率", SERIAL_MAX_BAUD);
        return;
    }
    favorite_session session = {
        .port_number = atoi(current->port_names[current->port_index] + 3),
        .baud = baud,
        .data_bits_index = current->data_bits_index,
        .parity_index = current->parity_index,
        .stop_bits_index = current->stop_bits_index,
        .flow_control = current->flow_control,
    };
    for (int i = 0; i < saved.session_count; i++)
    {
        const favorite_session *item = &saved.sessions[i];
        if (item->port_number == session.port_number && item->baud == session.baud &&
            item->data_bits_index == session.data_bits_index &&
            item->parity_index == session.parity_index &&
            item->stop_bits_index == session.stop_bits_index &&
            item->flow_control == session.flow_control)
        {
            saved_session_index = i;
            snprintf(current->status, sizeof current->status, "当前会话已收藏");
            return;
        }
    }
    if (saved.session_count == FAVORITES_CAPACITY)
    {
        snprintf(current->status, sizeof current->status, "会话收藏已满（%d 条）", FAVORITES_CAPACITY);
        return;
    }
    favorites updated = saved;
    updated.sessions[updated.session_count++] = session;
    if (save_favorites(current, &updated, "已收藏当前会话"))
    {
        saved_session_index = saved.session_count - 1;
    }
}

static void load_selected_session(serial_session *current)
{
    const favorite_session *session = &saved.sessions[saved_session_index];
    char port[16];
    snprintf(port, sizeof port, "COM%d", session->port_number);
    refresh_ports(current);
    current->port_index = -1;
    for (int i = 0; i < current->port_count; i++)
    {
        if (strcmp(current->port_names[i], port) == 0)
        {
            current->port_index = i;
            break;
        }
    }
    if (current->port_index < 0)
    {
        snprintf(current->status, sizeof current->status, "未找到收藏的串口 %s", port);
        return;
    }
    snprintf(current->baud_input, sizeof current->baud_input, "%lu", session->baud);
    current->data_bits_index = session->data_bits_index;
    current->parity_index = session->parity_index;
    current->stop_bits_index = session->stop_bits_index;
    current->flow_control = session->flow_control;
    snprintf(current->status, sizeof current->status, "已应用会话 %s，波特率 %lu", port, session->baud);
}

static void save_current_command(serial_session *current)
{
    unsigned char hex_bytes[SERIAL_COMMAND_CAPACITY / 2];
    size_t length;
    if (strpbrk(current->tx_input, "\r\n") != NULL)
    {
        snprintf(current->status, sizeof current->status, "常用命令仅支持单行内容");
        return;
    }
    if (current->tx_hex && (!parse_hex(current->tx_input, hex_bytes, &length) || length == 0))
    {
        snprintf(current->status, sizeof current->status, "请输入成对的十六进制数字，例如 01 03 FF");
        return;
    }
    for (int i = 0; i < saved.command_count; i++)
    {
        const favorite_command *command = &saved.commands[i];
        if (command->hex == current->tx_hex && command->ending_index == current->ending_index &&
            strcmp(command->text, current->tx_input) == 0)
        {
            saved_command_index = i;
            snprintf(current->status, sizeof current->status, "当前命令已收藏");
            return;
        }
    }
    if (saved.command_count >= COMMAND_FAVORITES_CAPACITY)
    {
        snprintf(current->status, sizeof current->status, "常用命令已满（%d 条）",
                 COMMAND_FAVORITES_CAPACITY);
        return;
    }
    favorites updated = saved;
    favorite_command *command = &updated.commands[updated.command_count++];
    command->hex = current->tx_hex;
    command->ending_index = current->ending_index;
    command->name[0] = '\0';
    snprintf(command->text, sizeof command->text, "%s", current->tx_input);
    if (save_favorites(current, &updated, "已收藏发送框内容"))
    {
        saved_command_index = saved.command_count - 1;
    }
}

static void load_selected_command(serial_session *current)
{
    const favorite_command *command = &saved.commands[saved_command_index];
    snprintf(current->tx_input, sizeof current->tx_input, "%s", command->text);
    current->tx_hex = command->hex;
    current->ending_index = command->ending_index;
    snprintf(current->status, sizeof current->status, "已填入常用命令");
}

static void remove_favorite(serial_session *current, bool session)
{
    if (!session)
    {
        remove_command(current, saved_command_index);
        return;
    }
    favorites updated = saved;
    int index = saved_session_index;
    updated.session_count--;
    if (index < updated.session_count)
    {
        memmove(&updated.sessions[index], &updated.sessions[index + 1],
                (size_t)(updated.session_count - index) * sizeof updated.sessions[0]);
    }
    if (save_favorites(current, &updated, "已移除会话"))
    {
        saved_session_index = -1;
    }
}

static void remove_command(serial_session *current, int index)
{
    favorites updated = saved;
    updated.command_count--;
    if (index < updated.command_count)
    {
        memmove(&updated.commands[index], &updated.commands[index + 1],
                (size_t)(updated.command_count - index) * sizeof updated.commands[0]);
    }
    if (save_favorites(current, &updated, "已移除命令"))
    {
        saved_command_index = -1;
    }
}

static bool save_favorites(serial_session *current, const favorites *updated, const char *message)
{
    if (favorites_path[0] == L'\0' || !favorites_save(favorites_path, updated))
    {
        snprintf(current->status, sizeof current->status, "无法保存串口收藏，请确认程序目录可写");
        return false;
    }
    saved = *updated;
    snprintf(current->status, sizeof current->status, "%s", message);
    return true;
}

static void toggle_connection(serial_session *current)
{
    static const BYTE parities[] = {NOPARITY, ODDPARITY, EVENPARITY};
    static const BYTE stop_bits[] = {ONESTOPBIT, TWOSTOPBITS};
    DWORD error;
    unsigned long baud;
    serial_config config;
    current->access_error[0] = '\0';

    if (serial_is_open(&current->serial))
    {
        flush_text(current, true);
        current->rx_ansi = (serial_ansi){0};
        serial_close(&current->serial);
        snprintf(current->status, sizeof current->status, "已手动断开");
        return;
    }
    if (current->port_index < 0)
    {
        return;
    }
    baud = parse_baud_rate(current);
    if (baud == 0)
    {
        snprintf(current->status, sizeof current->status, "请输入 1 到 %lu 之间的波特率", SERIAL_MAX_BAUD);
        return;
    }
    config = (serial_config){
        .baud_rate = (DWORD)baud,
        .data_bits = (BYTE)(7 + current->data_bits_index),
        .parity = parities[current->parity_index],
        .stop_bits = stop_bits[current->stop_bits_index],
        .rts_cts = current->flow_control,
    };
    error = serial_open(&current->serial, current->port_names[current->port_index], &config);
    if (error != ERROR_SUCCESS)
    {
        const char *reason = "无法打开或配置串口";
        if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
        {
            snprintf(current->access_error, sizeof current->access_error,
                     "%s | 串口被占用或访问被拒绝（错误码 %lu）",
                     current->port_names[current->port_index], (unsigned long)error);
            snprintf(current->status, sizeof current->status, "未连接");
            return;
        }
        else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_DEVICE_NOT_CONNECTED)
        {
            reason = "串口不可用";
        }
        report_session_error(current, "%s | %s（错误码 %lu）",
                             current->port_names[current->port_index], reason, (unsigned long)error);
        return;
    }
    snprintf(current->status, sizeof current->status, "已连接 | %s，波特率 %lu", current->port_names[current->port_index], baud);
}

static void poll_serial(serial_session *current)
{
    const unsigned char *data;
    DWORD length;
    ULONGLONG now = GetTickCount64();
    SYSTEMTIME local_time;

    if (current->capture != NULL && current->capture_root[0] != L'\0')
    {
        GetLocalTime(&local_time);
        update_daily_capture(current, &local_time);
    }

    for (DWORD total = 0; serial_is_open(&current->serial) && total < RX_BUDGET_BYTES; total += length)
    {
        DWORD error = serial_poll(&current->serial, &data, &length);
        if (error != ERROR_SUCCESS)
        {
            serial_close(&current->serial);
            flush_text(current, true);
            current->rx_ansi = (serial_ansi){0};
            report_session_error(current, "已断开 | 串口读写失败（错误码 %lu）",
                                 (unsigned long)error);
            break;
        }
        if (length == 0)
        {
            break;
        }
        GetLocalTime(&local_time);
        current->last_rx_time = clock_seconds(&local_time);
        if (current->capture != NULL &&
            (current->capture_root[0] == L'\0' || update_daily_capture(current, &local_time)) &&
            !write_capture(current, data, length))
        {
            fclose(current->capture);
            current->capture = NULL;
            current->capture_root[0] = L'\0';
            report_session_error(current, "记录已停止 | 写入文件失败");
        }
        current->received_bytes += length;
        current->last_rx_ms = now;
        current->hex_log.now = current->last_rx_time;
        append_hex(current, "接收", data, length);
        for (DWORD offset = 0; offset < length; offset++)
        {
            unsigned char byte = data[offset];
            bool line_end = byte == '\n' || byte == '\r';
            if (line_end || current->pending_length == sizeof current->pending_text)
            {
                flush_text(current, line_end);
            }
            if (!line_end)
            {
                if (byte < 32 && byte != '\t' && byte != 0x1b)
                {
                    byte = '.';
                }
                current->pending_text[current->pending_length++] = (char)byte;
            }
        }
    }
    if (now - current->last_rx_ms >= PARTIAL_LINE_TIMEOUT_MS)
    {
        flush_text(current, false);
    }
}

static void flush_text(serial_session *current, bool finish)
{
    if (current->pending_length == 0 && !finish)
    {
        return;
    }

    static const char prefix[] = "接收  ";
    char message[sizeof prefix - 1 + TEXT_DECODE_UTF8_CAPACITY];
    char *utf8 = message + sizeof prefix - 1;
    size_t used = text_decode_utf8(current->pending_text, current->pending_length,
                                  (text_encoding)current->encoding_index, finish, utf8);
    cui_log_color_span colors[SERIAL_ANSI_MAX_SPANS];
    uint16_t count = serial_ansi_strip(&current->rx_ansi, utf8, sizeof prefix - 1, finish, colors);
    if (utf8[0] != '\0')
    {
        memcpy(message, prefix, sizeof prefix - 1);
        current->text_log.now = current->last_rx_time;
        cui_log_append_colored(&current->text_log, CUI_LOG_INFO, current->text_log.now,
                               message, (int)strlen(message), colors, count);
    }
    /* 超时或长行分段时，未收全的字节留在缓冲区开头。 */
    current->pending_length -= used;
    if (used > 0 && current->pending_length > 0)
    {
        memmove(current->pending_text, current->pending_text + used, current->pending_length);
    }
}

static double local_clock_seconds(void)
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    return clock_seconds(&time);
}

static double clock_seconds(const SYSTEMTIME *time)
{
    /* 日志控件接收秒数并显示三位毫秒，超时仍使用单调计时。 */
    return time->wHour * 3600 + time->wMinute * 60 + time->wSecond + time->wMilliseconds / 1000.0;
}

static void append_hex(serial_session *current, const char *direction,
                       const unsigned char *data, size_t length)
{
    char row[HEX_COLUMNS * 3];

    for (size_t offset = 0; offset < length; offset += HEX_COLUMNS)
    {
        format_hex_row(row, data + offset, length - offset);
        cui_log_append(&current->hex_log, CUI_LOG_INFO, "%s  %s", direction, row);
    }
}

static size_t format_hex_row(char *row, const unsigned char *data, size_t length)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t column = 0; column < HEX_COLUMNS && column < length; column++)
    {
        unsigned char byte = data[column];
        if (column > 0)
        {
            row[used++] = ' ';
        }
        row[used++] = digits[byte >> 4];
        row[used++] = digits[byte & 15];
    }
    row[used] = '\0';
    return used;
}

static void send_input(serial_session *current)
{
    if (send_command(current, current->tx_input, current->tx_hex, current->ending_index))
    {
        current->tx_input[0] = '\0';
    }
}

static bool send_command(serial_session *current, const char *text, bool hex, int ending_index)
{
    static const char *endings[] = {"", "\r", "\n", "\r\n"};
    unsigned char data[sizeof current->tx_input + 2];
    size_t length;
    DWORD error;

    if (hex)
    {
        if (!parse_hex(text, data, &length))
        {
            report_session_error(current, "请输入成对的十六进制数字，例如 01 03 FF");
            return false;
        }
    }
    else
    {
        size_t ending_length = strlen(endings[ending_index]);
        length = strlen(text);
        memcpy(data, text, length);
        memcpy(data + length, endings[ending_index], ending_length);
        length += ending_length;
    }
    if (length == 0)
    {
        return false;
    }
    error = serial_send(&current->serial, data, (DWORD)length);
    if (error != ERROR_SUCCESS)
    {
        report_session_error(current, "发送失败（错误码 %lu）", (unsigned long)error);
        return false;
    }
    current->submitted_bytes += length;
    current->text_log.now = local_clock_seconds();
    current->hex_log.now = current->text_log.now;
    append_hex(current, "发送", data, length);
    /* 文本日志每条只显示一行，分别记录编辑框里的各行。 */
    const char *line = text;
    do
    {
        size_t line_length = strcspn(line, "\r\n");
        cui_log_append(&current->text_log, CUI_LOG_INFO, "发送  %.*s", (int)line_length, line);
        line += line_length;
        if (line[0] == '\r' && line[1] == '\n')
        {
            line++;
        }
        if (*line != '\0')
        {
            line++;
        }
    } while (*line != '\0');
    snprintf(current->status, sizeof current->status, "已连接 | %s，波特率 %s",
             current->port_names[current->port_index], current->baud_input);
    return true;
}

static bool parse_hex(const char *input, unsigned char *output, size_t *length)
{
    static const char digits[] = "0123456789ABCDEF";
    int high = -1;

    *length = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; cursor++)
    {
        if (isspace(*cursor) && high < 0)
        {
            continue;
        }
        const char *digit = strchr(digits, toupper(*cursor));
        if (digit == NULL)
        {
            return false;
        }
        int value = (int)(digit - digits);
        if (high < 0)
        {
            high = value;
        }
        else
        {
            output[(*length)++] = (unsigned char)((high << 4) | value);
            high = -1;
        }
    }
    return high < 0;
}

static void toggle_capture(serial_session *current)
{
    static const WCHAR filter[] = L"文本会话日志（带时间） (*.txt)\0*.txt\0"
                                  L"十六进制日志（带时间） (*.txt)\0*.txt\0"
                                  L"串口原始数据 (*.bin)\0*.bin\0";
    WCHAR path[1024] = L"串口会话记录";
    OPENFILENAMEW dialog = {0};

    if (current->capture != NULL)
    {
        int result = fclose(current->capture);
        current->capture = NULL;
        current->capture_root[0] = L'\0';
        if (result != 0)
        {
            report_session_error(current, "记录已停止 | 写入文件失败");
        }
        return;
    }
    if (log_folder[0] != L'\0')
    {
        if (current->port_index < 0)
        {
            report_session_error(current, "记录失败 | 请先选择串口");
            return;
        }
        wcscpy(current->capture_root, log_folder);
        current->capture_archive = (log_archive){
            .port_number = atoi(current->port_names[current->port_index] + 3),
        };
        current->capture_mode = CAPTURE_TEXT;
        current->capture_after_cr = false;
        current->capture_line_started = false;
        SYSTEMTIME time;
        GetLocalTime(&time);
        update_daily_capture(current, &time);
        return;
    }
    WCHAR port_label[16] = L"串口";
    if (current->port_index >= 0)
    {
        int port_number = atoi(current->port_names[current->port_index] + 3);
        swprintf(port_label, sizeof port_label / sizeof port_label[0], L"COM%d", port_number);
    }
    SYSTEMTIME time;
    GetLocalTime(&time);
    swprintf(path, sizeof path / sizeof path[0], L"%ls-%04u%02u%02u-%02u%02u%02u", port_label,
             (unsigned int)time.wYear, (unsigned int)time.wMonth, (unsigned int)time.wDay,
             (unsigned int)time.wHour, (unsigned int)time.wMinute, (unsigned int)time.wSecond);
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrTitle = L"保存串口会话记录";
    dialog.lpstrFilter = filter;
    dialog.nFilterIndex = current->view_index == 0 ? CAPTURE_TEXT : CAPTURE_HEX;
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)(sizeof path / sizeof path[0]);
    dialog.lpstrDefExt = L"txt";
    dialog.lpfnHook = capture_save_hook;
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR |
                   OFN_EXPLORER | OFN_ENABLEHOOK | OFN_ENABLESIZING;
    if (GetSaveFileNameW(&dialog))
    {
        /* 记录文件独占写入，避免不同会话同时覆盖同一文件。 */
        current->capture = _wfsopen(path, L"wb", _SH_DENYWR);
        if (current->capture == NULL)
        {
            report_session_error(current, "记录失败 | 文件被占用或无法写入");
        }
        current->capture_mode = (capture_format)dialog.nFilterIndex;
        current->capture_after_cr = false;
        current->capture_line_started = false;
    }
}

static bool update_daily_capture(serial_session *current, const SYSTEMTIME *time)
{
    int previous_port = current->capture_archive.port_number;
    int port = previous_port;
    if (serial_is_open(&current->serial))
    {
        port = atoi(current->port_names[current->port_index] + 3);
    }
    log_archive_result result = log_archive_update(&current->capture_archive, &current->capture,
                                                   current->capture_root, port, time);
    if (result == LOG_ARCHIVE_OPENED)
    {
        current->capture_line_started = false;
        /* 保留同一串口跨午夜拆开的 CRLF，换串口时重新识别行尾。 */
        if (port != previous_port)
        {
            current->capture_after_cr = false;
        }
    }
    if (result == LOG_ARCHIVE_ERROR)
    {
        if (current->capture != NULL)
        {
            fclose(current->capture);
            current->capture = NULL;
        }
        current->capture_root[0] = L'\0';
        report_session_error(current, "记录已停止 | 无法写入 COM%d 日志，请检查文件夹或文件占用", port);
        return false;
    }
    return true;
}

static UINT_PTR CALLBACK capture_save_hook(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam;
    if (message == WM_NOTIFY)
    {
        const OFNOTIFYW *event = (const OFNOTIFYW *)lparam;
        if (event->hdr.code == CDN_TYPECHANGE)
        {
            const WCHAR *extension = event->lpOFN->nFilterIndex == CAPTURE_RAW ? L"bin" : L"txt";
            SendMessageW(GetParent(dialog), CDM_SETDEFEXT, 0, (LPARAM)extension);
        }
    }
    return 0;
}

static const char *capture_label(const serial_session *current)
{
    if (current->capture_root[0] != L'\0')
    {
        return "正在按日期记录文本日志（.txt）";
    }
    switch (current->capture_mode)
    {
    case CAPTURE_HEX:
        return "正在记录十六进制日志（.txt）";
    case CAPTURE_RAW:
        return "正在记录原始字节（.bin）";
    default:
        return "正在记录文本日志（.txt）";
    }
}

static bool write_capture(serial_session *current, const unsigned char *data, size_t length)
{
    if (current->capture_mode == CAPTURE_RAW)
    {
        return fwrite(data, 1, length, current->capture) == length;
    }
    char timestamp[CAPTURE_TIMESTAMP_LENGTH + 1];
    unsigned int milliseconds = (unsigned int)(current->last_rx_time * 1000.0 + 0.5);
    unsigned int seconds = milliseconds / 1000;
    snprintf(timestamp, sizeof timestamp, "[%02u:%02u:%02u.%03u] ",
             (seconds / 3600) % 24, (seconds / 60) % 60, seconds % 60, milliseconds % 1000);
    if (current->capture_mode == CAPTURE_HEX)
    {
        char row[CAPTURE_TIMESTAMP_LENGTH + HEX_COLUMNS * 3 + 1];
        memcpy(row, timestamp, CAPTURE_TIMESTAMP_LENGTH);
        for (size_t offset = 0; offset < length; offset += HEX_COLUMNS)
        {
            size_t used = CAPTURE_TIMESTAMP_LENGTH +
                          format_hex_row(row + CAPTURE_TIMESTAMP_LENGTH,
                                         data + offset, length - offset);
            row[used++] = '\r';
            row[used++] = '\n';
            if (fwrite(row, 1, used, current->capture) != used)
            {
                return false;
            }
        }
        return true;
    }
    return write_text_capture(current, data, length, timestamp);
}

static bool write_text_capture(serial_session *current, const unsigned char *data, size_t length,
                               const char *timestamp)
{
    unsigned char output[256];
    size_t used = 0;
    for (size_t i = 0; i < length; i++)
    {
        unsigned char byte = data[i];
        bool after_cr = current->capture_after_cr;
        current->capture_after_cr = byte == '\r';
        /* 同一条 CRLF 可能被驱动分成两次接收。 */
        if (byte == '\n' && after_cr)
        {
            continue;
        }
        if (used + CAPTURE_TIMESTAMP_LENGTH + 2 > sizeof output)
        {
            if (fwrite(output, 1, used, current->capture) != used)
            {
                return false;
            }
            used = 0;
        }
        if (byte == '\r' || byte == '\n')
        {
            output[used++] = '\r';
            output[used++] = '\n';
            current->capture_line_started = false;
        }
        else
        {
            /* 跨批次收到的同一行，只在第一个字节前写一次时间。 */
            if (!current->capture_line_started)
            {
                memcpy(output + used, timestamp, CAPTURE_TIMESTAMP_LENGTH);
                used += CAPTURE_TIMESTAMP_LENGTH;
                current->capture_line_started = true;
            }
            output[used++] = byte < ' ' && byte != '\t' ? '.' : byte;
        }
    }
    return fwrite(output, 1, used, current->capture) == used;
}
