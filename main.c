#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <locale.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/time.h>
#include <sys/select.h>
#include <xkbcommon/xkbcommon.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

/* ========== CONSTANTS AND DEFINITIONS ========== */
#define MAX_WORD_LEN 256
#define MAX_DICT_SIZE 100000
#define INPUT_DEVICE "/dev/input/event3"  // Укажи своё устройство
#define UINPUT_DEVICE "/dev/uinput"
#define DICT_FILE_ENG "english_dict.txt"
#define DICT_FILE_RUS "russian_dict.txt"

#define ESC_KEY_CODE 1
#define SPACE_KEY_CODE 57
#define BACKSPACE_KEY_CODE 14
#define LEFTSHIFT_KEY_CODE 42
#define LEFTARROW_KEY_CODE 105
#define LEFTALT_KEY_CODE 56
#define LEFTMETA_KEY_CODE 125 // Super

// Минимальные задержки для Wayland
#define LAYOUT_SWITCH_DELAY 100000  // 100 мс
#define KEY_PRESS_DELAY 10000       // 10 мс
#define DELETE_WORD_DELAY 30000     // 30 мс

// Структура словаря
typedef struct {
    wchar_t **words;
    size_t count;
} Dictionary;

// Таблицы символов для раскладок
static const wchar_t eng_chars[] = L"qwertyuiop[]asdfghjkl;'zxcvbnm,./`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?~";
static const wchar_t rus_chars[] = L"йцукенгшщзхъфывапролджэячсмитьбю.ёЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,Ё";

// Таблица соответствия кодов клавиш и символов QWERTY
static const struct {
    int key_code;
    wchar_t eng_char;
} key_map[] = {
        {KEY_Q, L'q'}, {KEY_W, L'w'}, {KEY_E, L'e'}, {KEY_R, L'r'}, {KEY_T, L't'},
        {KEY_Y, L'y'}, {KEY_U, L'u'}, {KEY_I, L'i'}, {KEY_O, L'o'}, {KEY_P, L'p'},
        {KEY_A, L'a'}, {KEY_S, L's'}, {KEY_D, L'd'}, {KEY_F, L'f'}, {KEY_G, L'g'},
        {KEY_H, L'h'}, {KEY_J, L'j'}, {KEY_K, L'k'}, {KEY_L, L'l'}, {KEY_Z, L'z'},
        {KEY_X, L'x'}, {KEY_C, L'c'}, {KEY_V, L'v'}, {KEY_B, L'b'}, {KEY_N, L'n'},
        {KEY_M, L'm'}, {KEY_1, L'1'}, {KEY_2, L'2'}, {KEY_3, L'3'}, {KEY_4, L'4'},
        {KEY_5, L'5'}, {KEY_6, L'6'}, {KEY_7, L'7'}, {KEY_8, L'8'}, {KEY_9, L'9'},
        {KEY_0, L'0'}, {KEY_MINUS, L'-'}, {KEY_EQUAL, L'='}, {KEY_LEFTBRACE, L'['},
        {KEY_RIGHTBRACE, L']'}, {KEY_SEMICOLON, L';'}, {KEY_APOSTROPHE, L'\''},
        {KEY_GRAVE, L'`'}, {KEY_BACKSLASH, L'\\'}, {KEY_COMMA, L','}, {KEY_DOT, L'.'},
        {KEY_SLASH, L'/'}
};

/* ========== FUNCTION PROTOTYPES ========== */
void send_key(int fd, int keycode, int value);
void switch_layout(Display *display, int uinput_fd, bool use_super_space, int *system_layout);
void switch_layout_fallback(int uinput_fd, bool use_super_space);
void convert_layout(const wchar_t *input, wchar_t *output, bool to_russian);
int detect_word_layout(const wchar_t *text, int system_layout);
int setup_uinput_device(int *uinput_fd);
int get_x11_layout_group(Display *display);
int get_gsettings_layout_group();
void sync_xkb_state(struct xkb_state *xkb_state, int group);
int update_system_layout(Display *display, int *system_layout);

/* ========== UTILITY FUNCTIONS ========== */

// Конвертация слова между раскладками
void convert_layout(const wchar_t *input, wchar_t *output, bool to_russian) {
    size_t len = wcslen(input);
    for (size_t i = 0; i < len; i++) {
        const wchar_t *pos;
        if (to_russian) {
            pos = wcschr(eng_chars, input[i]);
            output[i] = pos ? rus_chars[pos - eng_chars] : input[i];
        } else {
            pos = wcschr(rus_chars, input[i]);
            output[i] = pos ? eng_chars[pos - rus_chars] : input[i];
        }
    }
    output[len] = L'\0';
}

// Получение текущей раскладки через gsettings (Wayland)
int get_gsettings_layout_group() {
    FILE *pipe = popen("gsettings get org.gnome.desktop.input-sources current", "r");
    if (!pipe) {
        wprintf(L"Failed to run gsettings\n");
        return -1;
    }
    char buffer[256];
    if (fgets(buffer, sizeof(buffer), pipe)) {
        int group = atoi(buffer);
        wprintf(L"gsettings layout group: %d (%ls)\n", group, group == 0 ? L"us" : L"ru");
        pclose(pipe);
        return group;
    }
    pclose(pipe);
    return -1;
}

// Обновление system_layout
int update_system_layout(Display *display, int *system_layout) {
    int new_layout = -1;
    if (display) {
        XkbStateRec xkb_state;
        if (XkbGetState(display, XkbUseCoreKbd, &xkb_state) == Success) {
            new_layout = xkb_state.group;
            wprintf(L"X11 layout group: %d (%ls)\n", new_layout, new_layout == 0 ? L"us" : L"ru");
        } else {
            wprintf(L"Failed to get X11 keyboard state\n");
        }
    }
    if (new_layout < 0) {
        new_layout = get_gsettings_layout_group();
        if (new_layout < 0) {
            new_layout = (*system_layout + 1) % 2;
            wprintf(L"Fallback: Updated system_layout: %d (%ls)\n",
                    new_layout, new_layout == 0 ? L"us" : L"ru");
        }
    }
    *system_layout = new_layout;
    return new_layout;
}

// Функция для определения раскладки слова
int detect_word_layout(const wchar_t *text, int system_layout) {
    if (text == NULL || *text == L'\0') {
        wprintf(L"Empty text in detect_word_layout\n");
        return 0;
    }

    int en_count = 0;
    int ru_count = 0;
    int total_chars = 0;

    for (size_t i = 0; text[i]; i++) {
        wprintf(L"Processing char: %lc (U+%04X)\n", text[i], (unsigned int)text[i]);
        if ((text[i] >= L'A' && text[i] <= L'Z') || (text[i] >= L'a' && text[i] <= L'z')) {
            total_chars++;
            en_count++;
        } else if (text[i] >= 0x0410 && text[i] <= 0x044F) {
            total_chars++;
            ru_count++;
        }
    }

    if (total_chars == 0) {
        wprintf(L"No valid characters in text\n");
        return 0;
    }

    float en_ratio = (float)en_count / total_chars;
    float ru_ratio = (float)ru_count / total_chars;

    wprintf(L"en_ratio: %.2f, ru_ratio: %.2f, system_layout: %d (%ls)\n",
            en_ratio, ru_ratio, system_layout, system_layout == 0 ? L"us" : L"ru");

    if (system_layout == 1 && ru_ratio >= 0.5) {
        return 2; // Русский
    }
    if (system_layout == 0 && en_ratio >= 0.5) {
        return 1; // Английский
    }
    if (ru_ratio >= 0.5 && en_ratio < 0.5) {
        return 2; // Русский
    }
    if (en_ratio >= 0.5 && ru_ratio < 0.5) {
        return 1; // Английский
    }

    wprintf(L"Ambiguous layout detected\n");
    return 0;
}

// Переключение раскладки через X11 или fallback
void switch_layout(Display *display, int uinput_fd, bool use_super_space, int *system_layout) {
    if (display) {
        int new_group = (*system_layout + 1) % 2;
        wprintf(L"Switching layout via X11 to group: %d (%ls)\n", new_group, new_group == 0 ? L"us" : L"ru");
        XkbLockGroup(display, XkbUseCoreKbd, new_group);
        XFlush(display);
        *system_layout = new_group;
    } else {
        wprintf(L"X11 unavailable, using fallback layout switch\n");
        switch_layout_fallback(uinput_fd, use_super_space);
        update_system_layout(display, system_layout);
    }
}

// Fallback для переключения раскладки через uinput (Wayland)
void switch_layout_fallback(int uinput_fd, bool use_super_space) {
    wprintf(L"Switching layout via uinput: Emulating %ls\n", use_super_space ? L"Super + Space" : L"Shift + Alt");
    if (use_super_space) {
        send_key(uinput_fd, LEFTMETA_KEY_CODE, 1);
        send_key(uinput_fd, SPACE_KEY_CODE, 1);
        usleep(KEY_PRESS_DELAY);
        send_key(uinput_fd, SPACE_KEY_CODE, 0);
        send_key(uinput_fd, LEFTMETA_KEY_CODE, 0);
    } else {
        send_key(uinput_fd, LEFTSHIFT_KEY_CODE, 1);
        send_key(uinput_fd, LEFTALT_KEY_CODE, 1);
        usleep(KEY_PRESS_DELAY);
        send_key(uinput_fd, LEFTALT_KEY_CODE, 0);
        send_key(uinput_fd, LEFTSHIFT_KEY_CODE, 0);
    }
    usleep(LAYOUT_SWITCH_DELAY);
}

// Отправка нажатия клавиши
void send_key(int fd, int keycode, int value) {
    struct input_event ev = {0};
    gettimeofday(&ev.time, NULL);
    ev.type = EV_KEY;
    ev.code = keycode;
    ev.value = value;
    write(fd, &ev, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
}

// Эмуляция ввода символа через uinput
void send_char(int uinput_fd, wchar_t target_char, bool is_russian) {
    int key_code = 0;
    if (is_russian) {
        const wchar_t *pos = wcschr(rus_chars, target_char);
        if (!pos) {
            wprintf(L"Cannot map char: %lc\n", target_char);
            return;
        }
        size_t index = pos - rus_chars;
        if (index >= wcslen(eng_chars)) {
            wprintf(L"Index out of range for char: %lc\n", target_char);
            return;
        }
        wchar_t source_char = eng_chars[index];
        for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
            if (key_map[i].eng_char == source_char) {
                key_code = key_map[i].key_code;
                break;
            }
        }
    } else {
        for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
            if (key_map[i].eng_char == target_char) {
                key_code = key_map[i].key_code;
                break;
            }
        }
    }
    if (key_code == 0) {
        wprintf(L"No key code for char: %lc\n", target_char);
        return;
    }
    send_key(uinput_fd, key_code, 1);
    send_key(uinput_fd, key_code, 0);
    usleep(KEY_PRESS_DELAY);
}

// Выделение и удаление слова
void select_and_delete_word(int uinput_fd, int len) {
    send_key(uinput_fd, LEFTSHIFT_KEY_CODE, 1);
    for (int i = 0; i < len + 1; i++) {
        send_key(uinput_fd, LEFTARROW_KEY_CODE, 1);
        send_key(uinput_fd, LEFTARROW_KEY_CODE, 0);
        usleep(KEY_PRESS_DELAY);
    }
    send_key(uinput_fd, LEFTSHIFT_KEY_CODE, 0);
    send_key(uinput_fd, BACKSPACE_KEY_CODE, 1);
    send_key(uinput_fd, BACKSPACE_KEY_CODE, 0);
    usleep(DELETE_WORD_DELAY);
}

/* ========== X11 FUNCTIONS ========== */

int get_x11_layout_group(Display *display) {
    if (!display) {
        wprintf(L"X11 display not available\n");
        return -1;
    }
    XkbStateRec xkb_state;
    if (XkbGetState(display, XkbUseCoreKbd, &xkb_state) != Success) {
        wprintf(L"Failed to get X11 keyboard state\n");
        return -1;
    }
    int group = xkb_state.group;
    wprintf(L"X11 layout group: %d (%ls)\n", group, group == 0 ? L"us" : L"ru");
    return group;
}

void sync_xkb_state(struct xkb_state *xkb_state, int group) {
    if (group >= 0) {
        wprintf(L"Syncing xkb_state to group: %d\n", group);
        xkb_state_update_mask(xkb_state, 0, 0, 0, 0, 0, group);
    }
}

/* ========== DICTIONARY FUNCTIONS ========== */

bool load_dictionary(const char *filename, Dictionary *dict) {
    FILE *file = fopen(filename, "r, ccs=UTF-8");
    if (!file) {
        wprintf(L"Ошибка: Не удалось открыть файл словаря %hs\n", filename);
        return false;
    }
    dict->words = malloc(MAX_DICT_SIZE * sizeof(wchar_t*));
    if (!dict->words) {
        fclose(file);
        return false;
    }
    dict->count = 0;
    wchar_t buffer[MAX_WORD_LEN];
    while (fgetws(buffer, MAX_WORD_LEN, file) && dict->count < MAX_DICT_SIZE) {
        size_t len = wcslen(buffer);
        if (len > 0 && buffer[len-1] == L'\n') {
            buffer[len-1] = L'\0';
            len--;
        }
        if (len == 0) continue;
        dict->words[dict->count] = malloc((len + 1) * sizeof(wchar_t));
        if (!dict->words[dict->count]) {
            for (size_t i = 0; i < dict->count; i++) free(dict->words[i]);
            free(dict->words);
            fclose(file);
            return false;
        }
        wcscpy(dict->words[dict->count], buffer);
        dict->count++;
    }
    fclose(file);
    return true;
}

void free_dictionary(Dictionary *dict) {
    for (size_t i = 0; i < dict->count; i++) free(dict->words[i]);
    free(dict->words);
    dict->words = NULL;
    dict->count = 0;
}

bool is_in_dict(const wchar_t *word, Dictionary *dict) {
    for (size_t i = 0; i < dict->count; i++) {
        if (wcscmp(word, dict->words[i]) == 0) return true;
    }
    return false;
}

/* ========== LAYOUT SWITCHING FUNCTIONS ========== */

void process_word(wchar_t *word, Dictionary *eng_dict, Dictionary *rus_dict, int uinput_fd, bool use_super_space, int *system_layout, Display *display, struct xkb_state *xkb_state) {
    if (!word || wcslen(word) == 0) {
        wprintf(L"Empty word, skipping\n");
        return;
    }

    wprintf(L"Processing word: %ls\n", word);

    // Обновляем раскладку перед обработкой слова
    update_system_layout(display, system_layout);
    wprintf(L"System layout before processing: %d (%ls)\n", *system_layout, *system_layout == 0 ? L"us" : L"ru");

    int layout = detect_word_layout(word, *system_layout);
    const wchar_t *layout_name;
    switch (layout) {
        case 1: layout_name = L"English"; break;
        case 2: layout_name = L"Russian"; break;
        case 0:
            wprintf(L"Ambiguous or unsupported word layout, skipping\n");
            return;
        default:
            wprintf(L"Unexpected layout value, skipping\n");
            return;
    }
    wprintf(L"Detected layout: %ls\n", layout_name);

    wchar_t converted_word[MAX_WORD_LEN];
    convert_layout(word, converted_word, layout == 1);

    bool word_found = false;
    const wchar_t *target_word = NULL;
    bool target_is_russian = false;

    if (layout == 1) {
        if (is_in_dict(converted_word, rus_dict)) {
            word_found = true;
            target_word = converted_word;
            target_is_russian = true;
        }
    } else if (layout == 2) {
        if (is_in_dict(converted_word, eng_dict)) {
            word_found = true;
            target_word = converted_word;
            target_is_russian = false;
        }
    }

    if (word_found) {
        wprintf(L"Found in %ls dictionary: %ls, selecting and deleting %d chars\n",
                layout == 1 ? L"Russian" : L"English", target_word, (int)wcslen(word) + 1);
        select_and_delete_word(uinput_fd, wcslen(word));
        switch_layout(display, uinput_fd, use_super_space, system_layout);
        sync_xkb_state(xkb_state, *system_layout);
        wprintf(L"Inputting word: %ls\n", target_word);
        for (size_t i = 0; i < wcslen(target_word); i++) {
            send_char(uinput_fd, target_word[i], target_is_russian);
        }
    } else {
        wprintf(L"No match in %ls dictionary\n", layout == 1 ? L"Russian" : L"English");
    }
}

/* ========== INPUT DEVICE FUNCTIONS ========== */

int setup_uinput_device(int *uinput_fd) {
    *uinput_fd = open(UINPUT_DEVICE, O_WRONLY | O_NONBLOCK);
    if (*uinput_fd < 0) {
        perror("Не удалось открыть /dev/uinput");
        return -1;
    }

    ioctl(*uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(*uinput_fd, UI_SET_EVBIT, EV_SYN);
    for (int i = 0; i < 256; ++i) ioctl(*uinput_fd, UI_SET_KEYBIT, i);

    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "virtual-keyboard");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0xfedc;
    uidev.id.version = 1;

    write(*uinput_fd, &uidev, sizeof(uidev));
    ioctl(*uinput_fd, UI_DEV_CREATE);
    sleep(1);
    return 0;
}

/* ========== MAIN FUNCTION ========== */

int main() {
    setlocale(LC_ALL, "");

    bool use_super_space = false;
    FILE *gsettings_pipe = popen("gsettings get org.gnome.desktop.input-sources xkb-options", "r");
    if (gsettings_pipe) {
        char buffer[256];
        if (fgets(buffer, sizeof(buffer), gsettings_pipe)) {
            if (strstr(buffer, "win_space_toggle")) {
                use_super_space = true;
                wprintf(L"Detected Super + Space for layout switching\n");
            } else {
                wprintf(L"Using Shift + Alt for layout switching\n");
            }
        }
        pclose(gsettings_pipe);
    }

    Display *display = XOpenDisplay(NULL);
    if (display) {
        wprintf(L"X11 display initialized\n");
    } else {
        wprintf(L"Running in Wayland, X11 unavailable\n");
    }

    struct xkb_context *xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_context) {
        wprintf(L"Ошибка: Не удалось создать xkb_context\n");
        if (display) XCloseDisplay(display);
        return 1;
    }

    const char *rules = "evdev";
    const char *model = "pc105";
    const char *layout = "us,ru";
    const char *variant = "";
    const char *options = use_super_space ? "grp:win_space_toggle" : "grp:alt_shift_toggle";
    struct xkb_rule_names names = { rules, model, layout, variant, options };
    struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_names(xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!xkb_keymap) {
        wprintf(L"Ошибка: Не удалось создать xkb_keymap\n");
        xkb_context_unref(xkb_context);
        if (display) XCloseDisplay(display);
        return 1;
    }

    struct xkb_state *xkb_state = xkb_state_new(xkb_keymap);
    if (!xkb_state) {
        wprintf(L"Ошибка: Не удалось создать xkb_state\n");
        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_context);
        if (display) XCloseDisplay(display);
        return 1;
    }

    int system_layout = get_x11_layout_group(display);
    if (system_layout < 0) {
        system_layout = get_gsettings_layout_group();
    }
    if (system_layout >= 0) {
        sync_xkb_state(xkb_state, system_layout);
    } else {
        wprintf(L"Could not determine initial layout, defaulting to us\n");
        system_layout = 0;
    }

    Dictionary eng_dict = {0}, rus_dict = {0};
    wprintf(L"Загрузка словарей...\n");
    if (!load_dictionary(DICT_FILE_ENG, &eng_dict) || !load_dictionary(DICT_FILE_RUS, &rus_dict)) {
        xkb_state_unref(xkb_state);
        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_context);
        if (display) XCloseDisplay(display);
        free_dictionary(&eng_dict);
        free_dictionary(&rus_dict);
        return 1;
    }

    int input_fd = open(INPUT_DEVICE, O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) {
        perror("Не удалось открыть устройство ввода");
        xkb_state_unref(xkb_state);
        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_context);
        if (display) XCloseDisplay(display);
        free_dictionary(&eng_dict);
        free_dictionary(&rus_dict);
        return 1;
    }

    int uinput_fd;
    if (setup_uinput_device(&uinput_fd) < 0) {
        close(input_fd);
        xkb_state_unref(xkb_state);
        xkb_keymap_unref(xkb_keymap);
        xkb_context_unref(xkb_context);
        if (display) XCloseDisplay(display);
        free_dictionary(&eng_dict);
        free_dictionary(&rus_dict);
        return 1;
    }

    wprintf(L"Слушаю ввод... Нажмите ESC для выхода.\n");

    struct input_event ev;
    wchar_t word[MAX_WORD_LEN] = {0};
    int word_len = 0;
    bool shift_pressed = false;
    bool alt_pressed = false;
    bool super_pressed = false;
    fd_set read_fds;
    struct timeval tv;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(input_fd, &read_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;

        int ret = select(input_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            perror("select failed");
            break;
        }
        if (ret == 0 || !FD_ISSET(input_fd, &read_fds)) continue;

        if (read(input_fd, &ev, sizeof(ev)) != sizeof(ev)) continue;

        if (ev.type == EV_KEY && ev.value == 1) {
            if (ev.code == LEFTSHIFT_KEY_CODE) {
                shift_pressed = true;
            } else if (ev.code == LEFTALT_KEY_CODE) {
                alt_pressed = true;
            } else if (ev.code == LEFTMETA_KEY_CODE) {
                super_pressed = true;
            } else if (ev.code == ESC_KEY_CODE) {
                wprintf(L"ESC нажат. Выход.\n");
                break;
            } else if (ev.code == SPACE_KEY_CODE) {
                if (word_len > 0) {
                    word[word_len] = L'\0';
                    process_word(word, &eng_dict, &rus_dict, uinput_fd, use_super_space, &system_layout, display, xkb_state);
                    word_len = 0;
                    memset(word, 0, sizeof(word));
                }
                send_key(uinput_fd, SPACE_KEY_CODE, 1);
                send_key(uinput_fd, SPACE_KEY_CODE, 0);
                wprintf(L"Space pressed, processed word\n");
            } else if (ev.code == BACKSPACE_KEY_CODE) {
                if (word_len > 0) {
                    word[--word_len] = L'\0';
                    wprintf(L"Backspace pressed, removed last char, word_len: %d\n", word_len);
                }
            } else {
                // Обновляем раскладку перед добавлением символа
                update_system_layout(display, &system_layout);
                wprintf(L"System layout before adding char: %d (%ls)\n", system_layout, system_layout == 0 ? L"us" : L"ru");

                wchar_t c = L'\0';
                bool found = false;
                for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
                    if (key_map[i].key_code == ev.code) {
                        c = key_map[i].eng_char;
                        found = true;
                        break;
                    }
                }
                if (found && word_len < MAX_WORD_LEN - 1) {
                    if (system_layout == 1) {
                        const wchar_t *pos = wcschr(eng_chars, c);
                        if (pos) {
                            c = rus_chars[pos - eng_chars];
                        }
                    }
                    if (iswalpha(c)) {
                        word[word_len++] = c;
                        wprintf(L"Added char: %lc (U+%04X), word_len: %d, system_layout: %d (%ls)\n",
                                c, (unsigned int)c, word_len, system_layout, system_layout == 0 ? L"us" : L"ru");
                    }
                }
            }

            // Обработка ручного переключения раскладки
            if ((shift_pressed && alt_pressed) || (super_pressed && ev.code == SPACE_KEY_CODE && ev.value == 1)) {
                wprintf(L"Detected manual %ls, updating layout\n", use_super_space ? L"Super + Space" : L"Shift + Alt");
                update_system_layout(display, &system_layout);
                sync_xkb_state(xkb_state, system_layout);
            }
        } else if (ev.type == EV_KEY && ev.value == 0) {
            if (ev.code == LEFTSHIFT_KEY_CODE) {
                shift_pressed = false;
            } else if (ev.code == LEFTALT_KEY_CODE) {
                alt_pressed = false;
            } else if (ev.code == LEFTMETA_KEY_CODE) {
                super_pressed = false;
            }
        }
    }

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(input_fd);
    xkb_state_unref(xkb_state);
    xkb_keymap_unref(xkb_keymap);
    xkb_context_unref(xkb_context);
    if (display) XCloseDisplay(display);
    free_dictionary(&eng_dict);
    free_dictionary(&rus_dict);
    wprintf(L"Программа завершена.\n");
    return 0;
}