#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <ncurses.h>
#include <time.h>

#define MAX_GPUS 8

static char hwmon_path[256];
static char gpu_name[128] = "Unknown AMD GPU";
static char driver_name[32] = "unknown";
static char device_path[256];
static int manual_mode = 0;
static int manual_speed = 50;
static int running = 1;

static int temp_unit = 0;
static const char *temp_units[] = {"°C", "°F", " K"};
static const char *temp_unit_names[] = {"Celsius", "Fahrenheit", "Kelvin"};

static int temp_sensor = 0;
static int temp2_available = 0;
static int temp3_available = 0;
static const char *temp_sensor_names[] = {"Edge", "Junction", "Memory"};

static int fan_count = 1;
static int fan_rpm_available = 0;

static int curve_profile = 0;
static const char *curve_profile_names[] = {"Default", "Quiet", "Aggressive", "Linear"};

static int max_temp_c = 0;
static time_t start_time;

static int pwm_writable = 0;
static int fan_curve_available = 0;
static int overdrive_available = 0;
static int zero_rpm_active = 0;
static int restore_failed = 0;

static int gpu_count = 0;
static int active_gpu = 0;

typedef struct {
    char hwmon_path[256];
    char gpu_name[128];
    char device_path[256];
    char short_name[32];
    int gpu_generation;
    int fan_count;
    int fan_rpm_available;
    int pwm_writable;
    int fan_curve_available;
    int overdrive_available;
    int temp2_available;
    int temp3_available;
    int manual_mode;
    int manual_speed;
    int curve_profile;
    int max_temp_c;
    int zero_rpm_active;
    int restore_failed;
} GPUState;

static GPUState gpus[MAX_GPUS];

struct gpu_entry {
    const char *device_id;
    const char *name;
    int generation;
};

#define GEN_GCN     0
#define GEN_POLARIS 1
#define GEN_VEGA    2
#define GEN_RDNA1   3
#define GEN_RDNA2   4
#define GEN_RDNA3   5
#define GEN_RDNA4   6

static const struct gpu_entry known_gpus[] = {
    {"0x679a", "Radeon HD 7950/70 (Tahiti)",           GEN_GCN},
    {"0x6798", "Radeon HD 7970 (Tahiti)",              GEN_GCN},
    {"0x6810", "Radeon R9 290/390 (Hawaii)",           GEN_GCN},
    {"0x67b0", "Radeon R9 290X/390X (Hawaii)",         GEN_GCN},
    {"0x7300", "Radeon R9 Fury (Fiji)",                GEN_GCN},
    {"0x67df", "Radeon RX 580 2048SP (Polaris 20)",    GEN_POLARIS},
    {"0x6fdf", "Radeon RX 580 (Polaris 20)",           GEN_POLARIS},
    {"0x67ef", "Radeon RX 570 (Polaris 20)",           GEN_POLARIS},
    {"0x67ff", "Radeon RX 570 (Polaris 20)",           GEN_POLARIS},
    {"0x67c0", "Radeon RX 560 (Polaris 11)",           GEN_POLARIS},
    {"0x67c7", "Radeon RX 560 (Polaris 11)",           GEN_POLARIS},
    {"0x67e0", "Radeon RX 560 (Polaris 11)",           GEN_POLARIS},
    {"0x67e3", "Radeon RX 560 (Polaris 11)",           GEN_POLARIS},
    {"0x67e8", "Radeon RX 560 (Polaris 11)",           GEN_POLARIS},
    {"0x687f", "Radeon RX Vega 64",                    GEN_VEGA},
    {"0x6863", "Radeon RX Vega 56",                    GEN_VEGA},
    {"0x66af", "Radeon VII",                           GEN_VEGA},
    {"0x731f", "Radeon RX 5700 XT (Navi 10)",          GEN_RDNA1},
    {"0x7310", "Radeon RX 5700 (Navi 10)",             GEN_RDNA1},
    {"0x7312", "Radeon RX 5600 XT (Navi 10)",          GEN_RDNA1},
    {"0x73bf", "Radeon RX 6900 XT (Navi 21)",          GEN_RDNA2},
    {"0x73a5", "Radeon RX 6800 XT (Navi 21)",          GEN_RDNA2},
    {"0x73a3", "Radeon RX 6800 (Navi 21)",             GEN_RDNA2},
    {"0x73df", "Radeon RX 6700 XT (Navi 22)",          GEN_RDNA2},
    {"0x744c", "Radeon RX 7900 XTX (Navi 31)",         GEN_RDNA3},
    {"0x7448", "Radeon RX 7900 XT (Navi 31)",          GEN_RDNA3},
    {"0x7480", "Radeon RX 7800 XT (Navi 32)",          GEN_RDNA3},
    {"0x7484", "Radeon RX 7700 XT (Navi 32)",          GEN_RDNA3},
    {"0x74a0", "Radeon RX 7600 (Navi 33)",             GEN_RDNA3},
    {NULL, NULL, 0}
};

int read_file_int(const char *path);

void sync_globals_from_gpu(int idx) {
    GPUState *g = &gpus[idx];
    strcpy(hwmon_path, g->hwmon_path);
    strcpy(gpu_name, g->gpu_name);
    strcpy(device_path, g->device_path);
    fan_count = g->fan_count;
    fan_rpm_available = g->fan_rpm_available;
    pwm_writable = g->pwm_writable;
    fan_curve_available = g->fan_curve_available;
    overdrive_available = g->overdrive_available;
    temp2_available = g->temp2_available;
    temp3_available = g->temp3_available;
    manual_mode = g->manual_mode;
    manual_speed = g->manual_speed;
    curve_profile = g->curve_profile;
    max_temp_c = g->max_temp_c;
    zero_rpm_active = g->zero_rpm_active;
    restore_failed = g->restore_failed;
}

void sync_gpu_from_globals(int idx) {
    GPUState *g = &gpus[idx];
    g->manual_mode = manual_mode;
    g->manual_speed = manual_speed;
    g->curve_profile = curve_profile;
    g->max_temp_c = max_temp_c;
    g->zero_rpm_active = zero_rpm_active;
    g->restore_failed = restore_failed;
}

void restore_fan_auto_gpu(GPUState *g) {
    if (g->hwmon_path[0] == '\0') return;
    
    if (g->fan_curve_available) {
        char path[300];
        snprintf(path, sizeof(path), "%s/gpu_od/fan_curve", g->hwmon_path);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "c"); fclose(f); return; }
    }
    
    char path[300];
    snprintf(path, sizeof(path), "%s/pwm1_enable", g->hwmon_path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "2");
    fclose(f);
    
    int actual = read_file_int(path);
    if (actual == 2) return;
    
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "1");
    fclose(f);
    
    char pwm_path[300];
    snprintf(pwm_path, sizeof(pwm_path), "%s/pwm1", g->hwmon_path);
    f = fopen(pwm_path, "w");
    if (f) { fprintf(f, "%d", (50 * 255) / 100); fclose(f); }
    
    snprintf(path, sizeof(path), "%s/pwm1_enable", g->hwmon_path);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "2");
    fclose(f);
    
    actual = read_file_int(path);
    if (actual != 2) g->restore_failed = 1;
}

void restore_fan_auto() {
    for (int i = 0; i < gpu_count; i++) restore_fan_auto_gpu(&gpus[i]);
    if (gpu_count > 0) restore_failed = gpus[active_gpu].restore_failed;
}

void handle_exit(int sig) {
    (void)sig;
    restore_fan_auto();
    running = 0;
}

int read_file_str(const char *path, char *buf, int size) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, size, f)) { fclose(f); return 0; }
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 1;
}

int read_file_int(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) { fclose(f); return -1; }
    fclose(f);
    return v;
}

void write_file_int(const char *path, int value) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", value);
    fclose(f);
}

void detect_capabilities_gpu(GPUState *g) {
    char path[300];
    
    snprintf(path, sizeof(path), "%s/pwm1", g->hwmon_path);
    FILE *f = fopen(path, "w");
    if (f) { g->pwm_writable = 1; fclose(f); }
    
    snprintf(path, sizeof(path), "%s/gpu_od/fan_curve", g->hwmon_path);
    f = fopen(path, "r");
    if (f) { g->fan_curve_available = 1; fclose(f); }
    
    snprintf(path, sizeof(path), "%s/../pp_od_clk_voltage", g->hwmon_path);
    f = fopen(path, "w");
    if (f) { g->overdrive_available = 1; fclose(f); }
    
    snprintf(path, sizeof(path), "%s/temp2_input", g->hwmon_path);
    if (read_file_int(path) >= 0) g->temp2_available = 1;
    
    snprintf(path, sizeof(path), "%s/temp3_input", g->hwmon_path);
    if (read_file_int(path) >= 0) g->temp3_available = 1;
    
    snprintf(path, sizeof(path), "%s/fan1_input", g->hwmon_path);
    if (read_file_int(path) >= 0) g->fan_rpm_available = 1;
    
    g->fan_count = 1;
    for (int i = 2; i <= 4; i++) {
        snprintf(path, sizeof(path), "%s/pwm%d", g->hwmon_path, i);
        f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        g->fan_count = i;
    }
}

int detect_amd_gpus() {
    const char *base = "/sys/bus/pci/devices";
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    gpu_count = 0;

    while ((e = readdir(d)) != NULL && gpu_count < MAX_GPUS) {
        if (e->d_name[0] == '.') continue;
        char path[300];
        char vendor[16] = {0};
        char device[16] = {0};

        snprintf(path, sizeof(path), "%s/%s/vendor", base, e->d_name);
        read_file_str(path, vendor, sizeof(vendor));
        snprintf(path, sizeof(path), "%s/%s/device", base, e->d_name);
        read_file_str(path, device, sizeof(device));

        if (!strstr(vendor, "0x1002")) continue;

        GPUState *g = &gpus[gpu_count];
        memset(g, 0, sizeof(GPUState));
        g->curve_profile = 0;
        g->manual_speed = 50;

        const char *name = NULL;
        for (int i = 0; known_gpus[i].device_id != NULL; i++) {
            if (strstr(device, known_gpus[i].device_id)) {
                name = known_gpus[i].name;
                g->gpu_generation = known_gpus[i].generation;
                break;
            }
        }
        if (name) strcpy(g->gpu_name, name);
        else snprintf(g->gpu_name, sizeof(g->gpu_name), "AMD GPU (%s)", device);

        snprintf(g->device_path, sizeof(g->device_path), "%s/%s", base, e->d_name);

        snprintf(path, sizeof(path), "%s/%s/hwmon", base, e->d_name);
        DIR *hd = opendir(path);
        if (!hd) continue;

        struct dirent *he;
        while ((he = readdir(hd)) != NULL) {
            if (he->d_name[0] == '.') continue;
            snprintf(g->hwmon_path, sizeof(g->hwmon_path), "%s/%s", path, he->d_name);
            break;
        }
        closedir(hd);
        if (g->hwmon_path[0] == '\0') continue;

        char *dash = strrchr(name ? name : g->gpu_name, ' ');
        if (dash && strstr(dash + 1, "(")) {
            char *paren = strchr(dash + 1, '(');
            if (paren) {
                size_t len = paren - (dash + 1);
                if (len < sizeof(g->short_name)) {
                    strncpy(g->short_name, dash + 1, len);
                    g->short_name[len] = 0;
                }
            }
        }
        if (g->short_name[0] == '\0') {
            char *p = strstr(g->gpu_name, "Radeon");
            if (p) snprintf(g->short_name, sizeof(g->short_name), "%s", p);
            else snprintf(g->short_name, sizeof(g->short_name), "GPU %d", gpu_count);
        }

        detect_capabilities_gpu(g);
        gpu_count++;
    }
    closedir(d);
    return gpu_count;
}

void detect_driver() {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) { strcpy(driver_name, "unknown"); return; }
    char line[256];
    int amdgpu = 0, radeon = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "amdgpu")) amdgpu = 1;
        if (strstr(line, "radeon")) radeon = 1;
    }
    fclose(f);
    if (amdgpu) strcpy(driver_name, "amdgpu");
    else if (radeon) strcpy(driver_name, "radeon");
    else strcpy(driver_name, "unknown");
}

void set_fan_manual_all() {
    GPUState *g = &gpus[active_gpu];
    if (g->fan_curve_available) return;
    for (int i = 1; i <= g->fan_count; i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/pwm%d_enable", g->hwmon_path, i);
        write_file_int(path, 1);
    }
}

void set_fan_manual_all_gpu(GPUState *g) {
    if (g->fan_curve_available) return;
    for (int i = 1; i <= g->fan_count; i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/pwm%d_enable", g->hwmon_path, i);
        write_file_int(path, 1);
    }
}

void set_fan_speed(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    GPUState *g = &gpus[active_gpu];
    
    if (g->fan_curve_available) {
        char path[300];
        snprintf(path, sizeof(path), "%s/gpu_od/fan_curve", g->hwmon_path);
        FILE *f = fopen(path, "w");
        if (!f) return;
        for (int i = 0; i < 5; i++) {
            int temp_c = 20 + i * 20;
            int pwm = (percent * 255) / 100;
            fprintf(f, "%d %d %d\n", i, temp_c * 1000, pwm);
        }
        fprintf(f, "c\n");
        fclose(f);
        return;
    }
    
    for (int i = 1; i <= g->fan_count; i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/pwm%d", g->hwmon_path, i);
        int pwm = (percent * 255) / 100;
        write_file_int(path, pwm);
    }
}

double convert_temp(int celsius) {
    switch (temp_unit) {
        case 0: return (double)celsius;
        case 1: return (celsius * 9.0 / 5.0) + 32.0;
        case 2: return celsius + 273.15;
        default: return (double)celsius;
    }
}

int fan_curve(int temp) {
    int points_default[][2] = {
        {20, 0}, {35, 20}, {50, 30}, {60, 45},
        {70, 60}, {80, 75}, {90, 90}, {100, 100}
    };
    int points_quiet[][2] = {
        {20, 0}, {40, 15}, {55, 25}, {65, 35},
        {75, 50}, {85, 65}, {95, 85}, {100, 100}
    };
    int points_aggressive[][2] = {
        {20, 10}, {35, 30}, {50, 50}, {60, 65},
        {70, 80}, {80, 90}, {90, 100}, {100, 100}
    };
    int points_linear[][2] = {
        {20, 0}, {100, 100}
    };
    
    int (*points)[2];
    int n;
    
    switch (curve_profile) {
        case 1: points = points_quiet; n = 8; break;
        case 2: points = points_aggressive; n = 8; break;
        case 3: points = points_linear; n = 2; break;
        default: points = points_default; n = 8; break;
    }

    if (temp <= points[0][0]) return points[0][1];
    if (temp >= points[n-1][0]) return points[n-1][1];

    for (int i = 0; i < n - 1; i++) {
        if (temp >= points[i][0] && temp <= points[i+1][0]) {
            int t0 = points[i][0];
            int t1 = points[i+1][0];
            int f0 = points[i][1];
            int f1 = points[i+1][1];
            return f0 + (f1 - f0) * (temp - t0) / (t1 - t0);
        }
    }

    return 100;
}

int get_temp() {
    GPUState *g = &gpus[active_gpu];
    char path[300];
    int sensor_idx = temp_sensor + 1;
    snprintf(path, sizeof(path), "%s/temp%d_input", g->hwmon_path, sensor_idx);
    int t = read_file_int(path);
    if (t < 0) {
        snprintf(path, sizeof(path), "%s/temp1_input", g->hwmon_path);
        t = read_file_int(path);
    }
    if (t < 0) return -1;
    return t / 1000;
}

int get_fan_rpm() {
    GPUState *g = &gpus[active_gpu];
    if (!g->fan_rpm_available) return -1;
    char path[300];
    snprintf(path, sizeof(path), "%s/fan1_input", g->hwmon_path);
    return read_file_int(path);
}

void detect_zero_rpm() {
    GPUState *g = &gpus[active_gpu];
    g->zero_rpm_active = 0;
    if (g->fan_curve_available) return;
    
    int temp = get_temp();
    int rpm = get_fan_rpm();
    if (temp > 0 && temp < 55 && rpm == 0) {
        char path[300];
        snprintf(path, sizeof(path), "%s/pwm1_enable", g->hwmon_path);
        int enable = read_file_int(path);
        if (enable == 2) g->zero_rpm_active = 1;
    }
    zero_rpm_active = g->zero_rpm_active;
}

void draw_bar(WINDOW *win, int y, int x, int width, double value, double max,
              const char *label, const char *unit) {
    int filled = (int)((value * width) / (max > 0 ? max : 1));
    if (filled > width) filled = width;
    mvwprintw(win, y, x, "%s%3.0f%s [", label, value, unit);
    for (int i = 0; i < width; i++) {
        if (i < filled) { wattron(win, A_REVERSE); waddch(win, ' '); wattroff(win, A_REVERSE); }
        else waddch(win, ' ');
    }
    waddch(win, ']');
}

void draw_curve_info(WINDOW *win, int y, int x, int current_temp_c) {
    int (*points)[2];
    int n;
    int points_default[][2] = {
        {20, 0}, {35, 20}, {50, 30}, {60, 45},
        {70, 60}, {80, 75}, {90, 90}, {100, 100}
    };
    int points_quiet[][2] = {
        {20, 0}, {40, 15}, {55, 25}, {65, 35},
        {75, 50}, {85, 65}, {95, 85}, {100, 100}
    };
    int points_aggressive[][2] = {
        {20, 10}, {35, 30}, {50, 50}, {60, 65},
        {70, 80}, {80, 90}, {90, 100}, {100, 100}
    };
    int points_linear[][2] = {
        {20, 0}, {100, 100}
    };
    
    switch (curve_profile) {
        case 1: points = points_quiet; n = 8; break;
        case 2: points = points_aggressive; n = 8; break;
        case 3: points = points_linear; n = 2; break;
        default: points = points_default; n = 8; break;
    }

    wattron(win, A_BOLD);
    mvwprintw(win, y, x, "Fan Curve [%s] (temp -> fan)", curve_profile_names[curve_profile]);
    wattroff(win, A_BOLD);

    for (int i = 0; i < n; i++) {
        int active = (current_temp_c >= points[i][0] &&
                     (i == n - 1 || current_temp_c < points[i+1][0]));
        double display_temp = convert_temp(points[i][0]);
        if (active) wattron(win, A_BOLD);
        mvwprintw(win, y + 1 + i, x, "  %s %3.0f%s -> %3d",
                 active ? ">" : " ",
                 display_temp, temp_units[temp_unit],
                 points[i][1]);
        wprintw(win, "%%");
        if (active) wattroff(win, A_BOLD);
    }
}

void draw_controls(WINDOW *win, int start_y, int x) {
    int y = start_y;
    wattron(win, A_BOLD);
    mvwprintw(win, y++, x, "--- Controls ---");
    wattroff(win, A_BOLD);
    y++;
    mvwprintw(win, y++, x, " 1-%d       Select GPU", gpu_count);
    mvwprintw(win, y++, x, " M         Toggle Manual / Auto");
    mvwprintw(win, y++, x, " T         Cycle °C / °F / K");
    mvwprintw(win, y++, x, " S         Cycle temp sensor");
    mvwprintw(win, y++, x, " C         Cycle fan curve profile");
    mvwprintw(win, y++, x, " R         Reset peak temperature");
    mvwprintw(win, y++, x, " +/-       Adjust fan by 5%%");
    mvwprintw(win, y++, x, " Up/Down   Adjust fan by 1%%");
    mvwprintw(win, y++, x, " 0         Set fan 0%%");
    mvwprintw(win, y++, x, " Q         Quit (restore auto)");
}

void draw_ui(int temp_c, int speed) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    double display_temp = convert_temp(temp_c);
    int mode_color = manual_mode ? 3 : 1;

    move(0, 0);
    clrtoeol();
    attron(A_REVERSE);
    if (gpu_count > 1) {
        printw(" CAGT - AMD GPU Fan Control  ");
        for (int i = 0; i < gpu_count; i++) {
            if (i == active_gpu) {
                wattron(stdscr, A_BOLD);
                printw(" [%d:%s] ", i + 1, gpus[i].short_name);
                wattroff(stdscr, A_BOLD);
            } else {
                printw("  %d:%s  ", i + 1, gpus[i].short_name);
            }
        }
    } else {
        printw(" CAGT - AMD GPU Fan Control ");
    }
    attroff(A_REVERSE);

    int info_row = 2;

    wattron(stdscr, A_BOLD);
    mvprintw(info_row, 2, "GPU Information");
    wattroff(stdscr, A_BOLD);
    mvprintw(info_row + 1, 4, "Model:  %s", gpu_name);
    mvprintw(info_row + 2, 4, "Driver: %s", driver_name);
    mvprintw(info_row + 3, 4, "Fans:   %d", fan_count);
    if (fan_rpm_available) {
        int rpm = get_fan_rpm();
        if (rpm >= 0) {
            mvprintw(info_row + 3, 20, "(%d RPM)", rpm);
            clrtoeol();
        }
    }
    mvprintw(info_row + 4, 4, "Path:   %s", hwmon_path);

    if (restore_failed) {
        wattron(stdscr, COLOR_PAIR(3) | A_BOLD);
        mvprintw(info_row + 5, 4, "WARNING: Auto fan restore may have failed! Reboot to recover.");
        wattroff(stdscr, COLOR_PAIR(3) | A_BOLD);
        info_row++;
    }

    int status_row = info_row + 6;
    
    move(status_row, 0);
    clrtoeol();
    wattron(stdscr, A_BOLD);
    mvprintw(status_row, 2, "Status  [%s]", temp_unit_names[temp_unit]);
    wattroff(stdscr, A_BOLD);

    mvprintw(status_row + 1, 4, "Mode: ");
    if (has_colors()) wattron(stdscr, COLOR_PAIR(mode_color) | A_BOLD);
    wprintw(stdscr, "%s", manual_mode ? "MANUAL" : "AUTO (curve)");
    if (has_colors()) wattroff(stdscr, COLOR_PAIR(mode_color) | A_BOLD);
    
    GPUState *g = &gpus[active_gpu];
    if (!g->overdrive_available && g->gpu_generation >= GEN_RDNA3) {
        wattron(stdscr, COLOR_PAIR(3));
        wprintw(stdscr, "  [Overdrive not enabled!]");
        wattroff(stdscr, COLOR_PAIR(3));
    }
    
    if (zero_rpm_active) {
        wprintw(stdscr, "  [Zero RPM active]");
    }
    clrtoeol();
    mvprintw(status_row + 2, 4, "Sensor: ");
    wprintw(stdscr, "%s", temp_sensor_names[temp_sensor]);
    if (temp_sensor > 0 && !(temp_sensor == 1 ? temp2_available : temp3_available)) {
        wprintw(stdscr, " (unavailable, using edge)");
    }
    clrtoeol();
    mvprintw(status_row + 3, 4, "Temperature: ");
    if (has_colors()) {
        if (temp_c < 60) wattron(stdscr, COLOR_PAIR(1) | A_BOLD);
        else if (temp_c < 80) wattron(stdscr, COLOR_PAIR(2) | A_BOLD);
        else wattron(stdscr, COLOR_PAIR(3) | A_BOLD);
    }
    if (temp_unit == 2)
        wprintw(stdscr, "%.1f K ", display_temp);
    else if (temp_unit == 1)
        wprintw(stdscr, "%.1f°F ", display_temp);
    else
        wprintw(stdscr, "%.1f°C ", display_temp);
    if (has_colors())
        wattroff(stdscr, COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3) | A_BOLD);

    if (max_temp_c > 0) {
        double display_max = convert_temp(max_temp_c);
        if (temp_unit == 2)
            wprintw(stdscr, "(Max: %.1f K)", display_max);
        else if (temp_unit == 1)
            wprintw(stdscr, "(Max: %.1f°F)", display_max);
        else
            wprintw(stdscr, "(Max: %.1f°C)", display_max);
        clrtoeol();
    }

    mvprintw(status_row + 4, 4, "Fan Speed: ");
    if (has_colors()) {
        if (speed < 40) wattron(stdscr, COLOR_PAIR(1) | A_BOLD);
        else if (speed < 70) wattron(stdscr, COLOR_PAIR(2) | A_BOLD);
        else wattron(stdscr, COLOR_PAIR(3) | A_BOLD);
    }
    wprintw(stdscr, "%d%%", speed);
    clrtoeol();
    if (has_colors())
        wattroff(stdscr, COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3) | A_BOLD);

    time_t now = time(NULL);
    int elapsed = (int)(now - start_time);
    mvprintw(status_row + 4, 30, "Runtime: %02d:%02d:%02d", 
             elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    clrtoeol();
    int bar_width = max_x - 22;
    if (bar_width > 60) bar_width = 60;

    int bar_row = status_row + 6;
    
    move(bar_row, 0);
    clrtoeol();
    if (has_colors()) {
        if (speed < 40) wattron(stdscr, COLOR_PAIR(1));
        else if (speed < 70) wattron(stdscr, COLOR_PAIR(2));
        else wattron(stdscr, COLOR_PAIR(3));
    }
    draw_bar(stdscr, bar_row, 4, bar_width, (double)speed, 100.0, "Fan   ", "%");
    if (has_colors())
        wattroff(stdscr, COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3));

    double temp_max;
    switch (temp_unit) {
        case 0: temp_max = 100.0; break;
        case 1: temp_max = 212.0; break;
        case 2: temp_max = 373.15; break;
        default: temp_max = 100.0;
    }
    double temp_display = display_temp;
    if (temp_display > temp_max) temp_display = temp_max;
    if (temp_display < 0) temp_display = 0;

    char temp_bar_label[16];
    if (temp_unit == 2)
        snprintf(temp_bar_label, sizeof(temp_bar_label), "Temp K ");
    else if (temp_unit == 1)
        snprintf(temp_bar_label, sizeof(temp_bar_label), "Temp °F ");
    else
        snprintf(temp_bar_label, sizeof(temp_bar_label), "Temp °C ");

    move(bar_row + 2, 0);
    clrtoeol();
    if (has_colors()) {
        if (temp_c < 60) wattron(stdscr, COLOR_PAIR(1));
        else if (temp_c < 80) wattron(stdscr, COLOR_PAIR(2));
        else wattron(stdscr, COLOR_PAIR(3));
    }
    draw_bar(stdscr, bar_row + 2, 4, bar_width, temp_display, temp_max, temp_bar_label, "");
    if (has_colors())
        wattroff(stdscr, COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3));

    for (int i = 0; i < 10; i++) {
        move(bar_row + 4 + i, 4);
        clrtoeol();
    }
    draw_curve_info(stdscr, bar_row + 4, 4, temp_c);

    int control_col = max_x > 80 ? 46 : 4;
    int control_row = max_x > 80 ? bar_row + 4 : bar_row + 14;
    draw_controls(stdscr, control_row, control_col);

    if (!g->overdrive_available && g->gpu_generation >= GEN_RDNA3) {
        wattron(stdscr, COLOR_PAIR(3) | A_BOLD);
        int warn_row = max_y - 2;
        mvprintw(warn_row, 2, "RDNA3 requires amdgpu.ppfeaturemask=0xffffffff for manual control.");
        mvprintw(warn_row + 1, 2, "Add to kernel cmdline or /etc/modprobe.d/amdgpu.conf and reboot.");
        wattroff(stdscr, COLOR_PAIR(3) | A_BOLD);
    }

    refresh();
}

int main() {
    signal(SIGINT, handle_exit);
    signal(SIGTERM, handle_exit);
    atexit(restore_fan_auto);

    if (!detect_amd_gpus()) {
        printf("[CAGT] No AMD GPU found.\n");
        return 1;
    }

    detect_driver();
    sync_globals_from_gpu(0);
    start_time = time(NULL);

    printf("[CAGT] Found %d AMD GPU(s)\n", gpu_count);
    for (int i = 0; i < gpu_count; i++) {
        printf("[CAGT] GPU %d: %s\n", i + 1, gpus[i].gpu_name);
        if (gpus[i].fan_curve_available) printf("[CAGT]   Using fan_curve interface (RDNA3+)\n");
        printf("[CAGT]   Fans detected: %d\n", gpus[i].fan_count);
    }
    printf("[CAGT] Driver: %s\n", driver_name);
    if (!overdrive_available && gpus[0].gpu_generation >= GEN_RDNA3)
        printf("[CAGT] WARNING: Overdrive not enabled on some GPUs.\n");
    printf("[CAGT] Starting TUI...\n");
    sleep(1);

    for (int i = 0; i < gpu_count; i++) {
        set_fan_manual_all_gpu(&gpus[i]);
    }

    int any_writable = 0;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].fan_curve_available) { any_writable = 1; continue; }
        char perm_test[300];
        snprintf(perm_test, sizeof(perm_test), "%s/pwm1", gpus[i].hwmon_path);
        FILE *pt = fopen(perm_test, "w");
        if (pt) { any_writable = 1; fclose(pt); }
    }
    if (!any_writable) {
        printf("[CAGT] Cannot write to any GPU hwmon — permission denied.\n");
        printf("[CAGT] Run with: sudo ./cagt\n");
        return 1;
    }

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    timeout(100);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
    }

    while (running) {
        int temp_c = get_temp();
        if (temp_c < 0) temp_c = 0;
        
        if (temp_c > max_temp_c) max_temp_c = temp_c;

        int speed;
        if (manual_mode) speed = manual_speed;
        else speed = fan_curve(temp_c);

        set_fan_speed(speed);
        detect_zero_rpm();
        draw_ui(temp_c, speed);

        int ch = getch();
        if (ch >= '1' && ch <= '9' && (ch - '1') < gpu_count) {
            sync_gpu_from_globals(active_gpu);
            active_gpu = ch - '1';
            sync_globals_from_gpu(active_gpu);
            if (temp_sensor == 1 && !temp2_available) temp_sensor = 0;
            if (temp_sensor == 2 && !temp3_available) temp_sensor = 0;
            continue;
        }
        
        switch (ch) {
            case 'q': case 'Q': running = 0; break;
            case 'm': case 'M':
                manual_mode = !manual_mode;
                if (manual_mode) manual_speed = speed;
                break;
            case 't': case 'T': temp_unit = (temp_unit + 1) % 3; break;
            case 's': case 'S':
                temp_sensor = (temp_sensor + 1) % 3;
                if (temp_sensor == 1 && !temp2_available) temp_sensor = 2;
                if (temp_sensor == 2 && !temp3_available) temp_sensor = 0;
                break;
            case 'c': case 'C': curve_profile = (curve_profile + 1) % 4; break;
            case 'r': case 'R': max_temp_c = temp_c; break;
            case '+': case '=':
                if (!manual_mode) { manual_mode = 1; manual_speed = speed; }
                manual_speed += 5; break;
            case '-': case '_':
                if (!manual_mode) { manual_mode = 1; manual_speed = speed; }
                manual_speed -= 5; break;
            case KEY_UP: case KEY_RIGHT:
                if (!manual_mode) { manual_mode = 1; manual_speed = speed; }
                manual_speed += 1; break;
            case KEY_DOWN: case KEY_LEFT:
                if (!manual_mode) { manual_mode = 1; manual_speed = speed; }
                manual_speed -= 1; break;
            case '0':
                if (gpu_count > 1 && active_gpu > 0) break;
                if (!manual_mode) manual_mode = 1;
                manual_speed = 0;
                break;
        }

        if (manual_speed > 100) manual_speed = 100;
        if (manual_speed < 0) manual_speed = 0;
    }

    endwin();
    restore_fan_auto();
    
    if (restore_failed) {
        printf("[CAGT] WARNING: Could not restore automatic fan control.\n");
        printf("[CAGT] Fan left at safe speed. Reboot to fully restore.\n");
    }
    printf("[CAGT] Exited cleanly.\n");
    return 0;
}