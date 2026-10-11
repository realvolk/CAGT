#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <ncurses.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pwd.h>
#include <errno.h>
#include <limits.h>

#define MAX_GPUS          8
#define MAX_CURVE_POINTS  12
#define PWM_MAX           255
#define TICK_MS           100
#define SOCKET_BUF        2048
#define PATH_BUF          512
#define CURVE_COUNT       5
#define PP_OVERDRIVE_BIT  0x4000

#define PWM_MODE_NONE     0
#define PWM_MODE_MANUAL   1
#define PWM_MODE_AUTO     2

enum {
    GEN_GCN = 0,
    GEN_POLARIS,
    GEN_VEGA,
    GEN_RDNA1,
    GEN_RDNA2,
    GEN_RDNA3,
    GEN_RDNA4,
};

enum { UNIT_C = 0, UNIT_F, UNIT_K, UNIT_COUNT };
enum { SENSOR_EDGE = 0, SENSOR_JUNCTION, SENSOR_MEMORY, SENSOR_COUNT };
enum { CURVE_DEFAULT = 0, CURVE_QUIET, CURVE_AGGRESSIVE, CURVE_LINEAR, CURVE_CUSTOM };
enum { MODE_CURVE = 0, MODE_MANUAL, MODE_HARDWARE, MODE_COUNT };

typedef struct { int temp; int fan; } CurvePoint;

static const CurvePoint curve_default[] = {
    {20, 0}, {35, 20}, {50, 30}, {60, 45},
    {70, 60}, {80, 75}, {90, 90}, {100, 100},
};
static const CurvePoint curve_quiet[] = {
    {20, 0}, {40, 15}, {55, 25}, {65, 35},
    {75, 50}, {85, 65}, {95, 85}, {100, 100},
};
static const CurvePoint curve_aggressive[] = {
    {20, 10}, {35, 30}, {50, 50}, {60, 65},
    {70, 80}, {80, 90}, {90, 100}, {100, 100},
};
static const CurvePoint curve_linear[] = {
    {20, 0}, {100, 100},
};

struct CurveDef {
    const CurvePoint *pts;
    int n;
};

static const struct CurveDef builtin_curves[CURVE_COUNT - 1] = {
    { curve_default,    (int)(sizeof(curve_default)    / sizeof(curve_default[0])) },
    { curve_quiet,      (int)(sizeof(curve_quiet)      / sizeof(curve_quiet[0])) },
    { curve_aggressive, (int)(sizeof(curve_aggressive) / sizeof(curve_aggressive[0])) },
    { curve_linear,     (int)(sizeof(curve_linear)     / sizeof(curve_linear[0])) },
};

static const char *const curve_names[CURVE_COUNT] = {
    "Default", "Quiet", "Aggressive", "Linear", "Custom",
};

static const char *const unit_suffix[UNIT_COUNT] = { "°C", "°F", " K" };
static const char *const unit_names[UNIT_COUNT]  = { "Celsius", "Fahrenheit", "Kelvin" };
static const char *const sensor_names[SENSOR_COUNT] = { "Edge", "Junction", "Memory" };
static const char *const mode_names[MODE_COUNT] = { "curve", "manual", "hardware" };

typedef struct {
    char hwmon_path[PATH_BUF];
    char device_path[PATH_BUF];
    char fan_curve_path[PATH_BUF];
    char name[128];
    char short_name[32];
    int  generation;

    int  fan_count;
    int  fan_rpm_available;
    int  temp2_available;
    int  temp3_available;
    int  pwm_writable;
    int  fan_curve_available;
    int  overdrive_available;

    int  mode;
    int  manual_speed;
    int  curve_profile;
    int  temp_sensor;
    int  max_temp_c;
    int  zero_rpm_active;
    int  restore_failed;

    int  last_pwm_written;
    int  curve_dirty;
    int  last_temp_c;
    int  last_rpm;
} GPUState;

static GPUState gpus[MAX_GPUS];
static int      gpu_count  = 0;
static int      active_idx = 0;

static int    temp_unit      = UNIT_C;
static int    running        = 1;
static time_t start_time;

static int   daemon_mode     = 0;
static int   logging_enabled = 0;
static int   log_interval_ms = 1000;
static FILE *log_file        = NULL;
static int   socket_fd       = -1;

static int        custom_curve_n = 0;
static CurvePoint custom_curve[MAX_CURVE_POINTS];

static char driver_name[32] = "unknown";

static volatile sig_atomic_t g_signal_flag = 0;

struct gpu_entry {
    const char *device_id;
    const char *name;
    int generation;
};

static const struct gpu_entry known_gpus[] = {
    {"0x679a", "Radeon HD 7950/70 (Tahiti)",        GEN_GCN},
    {"0x6798", "Radeon HD 7970 (Tahiti)",           GEN_GCN},
    {"0x6810", "Radeon R9 290/390 (Hawaii)",        GEN_GCN},
    {"0x67b0", "Radeon R9 290X/390X (Hawaii)",      GEN_GCN},
    {"0x7300", "Radeon R9 Fury (Fiji)",             GEN_GCN},
    {"0x67df", "Radeon RX 580 2048SP (Polaris 20)", GEN_POLARIS},
    {"0x6fdf", "Radeon RX 580 (Polaris 20)",        GEN_POLARIS},
    {"0x67ef", "Radeon RX 570 (Polaris 20)",        GEN_POLARIS},
    {"0x67ff", "Radeon RX 570 (Polaris 20)",        GEN_POLARIS},
    {"0x67c0", "Radeon RX 560 (Polaris 11)",        GEN_POLARIS},
    {"0x67c7", "Radeon RX 560 (Polaris 11)",        GEN_POLARIS},
    {"0x67e0", "Radeon RX 560 (Polaris 11)",        GEN_POLARIS},
    {"0x67e3", "Radeon RX 560 (Polaris 11)",        GEN_POLARIS},
    {"0x67e8", "Radeon RX 560 (Polaris 11)",        GEN_POLARIS},
    {"0x687f", "Radeon RX Vega 64",                 GEN_VEGA},
    {"0x6863", "Radeon RX Vega 56",                 GEN_VEGA},
    {"0x66af", "Radeon VII",                        GEN_VEGA},
    {"0x731f", "Radeon RX 5700 XT (Navi 10)",       GEN_RDNA1},
    {"0x7310", "Radeon RX 5700 (Navi 10)",          GEN_RDNA1},
    {"0x7312", "Radeon RX 5600 XT (Navi 10)",       GEN_RDNA1},
    {"0x7340", "Radeon RX 5500 XT (Navi 14)",       GEN_RDNA1},
    {"0x734f", "Radeon RX 5500 (Navi 14)",          GEN_RDNA1},
    {"0x73bf", "Radeon RX 6900 XT (Navi 21)",       GEN_RDNA2},
    {"0x73a5", "Radeon RX 6800 XT (Navi 21)",       GEN_RDNA2},
    {"0x73a3", "Radeon RX 6800 (Navi 21)",          GEN_RDNA2},
    {"0x73af", "Radeon RX 6950 XT (Navi 21)",       GEN_RDNA2},
    {"0x73df", "Radeon RX 6700 XT (Navi 22)",       GEN_RDNA2},
    {"0x73e0", "Radeon RX 6700 (Navi 22)",          GEN_RDNA2},
    {"0x73ef", "Radeon RX 6650 XT (Navi 23)",       GEN_RDNA2},
    {"0x73ff", "Radeon RX 6600/6600 XT (Navi 23)",  GEN_RDNA2},
    {"0x7424", "Radeon RX 6300 (Navi 24)",          GEN_RDNA2},
    {"0x743f", "Radeon RX 6400/6500 XT (Navi 24)",  GEN_RDNA2},
    {"0x744c", "Radeon RX 7900 XTX (Navi 31)",      GEN_RDNA3},
    {"0x7448", "Radeon RX 7900 XT (Navi 31)",       GEN_RDNA3},
    {"0x7480", "Radeon RX 7800 XT (Navi 32)",       GEN_RDNA3},
    {"0x7484", "Radeon RX 7700 XT (Navi 32)",       GEN_RDNA3},
    {"0x74a0", "Radeon RX 7600 (Navi 33)",          GEN_RDNA3},
    {"0x7550", "Radeon RX 9070 XT (Navi 48)",       GEN_RDNA4},
    {"0x7551", "Radeon RX 9070 (Navi 48)",          GEN_RDNA4},
    {"0x7590", "Radeon RX 9060 XT (Navi 44)",       GEN_RDNA4},
    {NULL, NULL, 0}
};

static int read_file_str(const char *path, char *buf, size_t sz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)sz, f)) { fclose(f); return 0; }
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 1;
}

static int read_file_int(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) { fclose(f); return -1; }
    fclose(f);
    return v;
}

static int write_file_int(const char *path, int value) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fprintf(f, "%d", value) > 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static int parse_int(const char *str, int *value) {
    if (!str || !*str) return 0;
    char *end;
    errno = 0;
    long v = strtol(str, &end, 10);
    if (errno || end == str || *end != '\0') return 0;
    if (v < INT_MIN || v > INT_MAX) return 0;
    *value = (int)v;
    return 1;
}

static void sig_handler(int sig) {
    (void)sig;
    g_signal_flag = 1;
}

static uid_t invoking_uid(void) {
    const char *s = getenv("SUDO_UID");
    if (s && s[0]) {
        char *end;
        unsigned long u = strtoul(s, &end, 10);
        if (*end == '\0') return (uid_t)u;
    }
    return getuid();
}

static gid_t invoking_gid(void) {
    const char *s = getenv("SUDO_GID");
    if (s && s[0]) {
        char *end;
        unsigned long g = strtoul(s, &end, 10);
        if (*end == '\0') return (gid_t)g;
    }
    return getgid();
}

static const char *resolve_home(void) {
    const char *sudo_uid = getenv("SUDO_UID");
    if (sudo_uid && sudo_uid[0]) {
        char *end;
        unsigned long u = strtoul(sudo_uid, &end, 10);
        if (*end == '\0') {
            struct passwd *pw = getpwuid((uid_t)u);
            if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
        }
    }
    const char *home = getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return NULL;
}

static void chown_to_invoker(const char *path) {
    if (geteuid() != 0) return;
    const char *suid = getenv("SUDO_UID");
    const char *sgid = getenv("SUDO_GID");
    if (!suid || !sgid) return;
    if (chown(path, invoking_uid(), invoking_gid()) < 0) { }
}

static int make_socket_path(char *buf, size_t sz) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) {
        int n = snprintf(buf, sz, "%s/cagt.sock", xdg);
        if (n > 0 && (size_t)n < sz && strlen(buf) < 108) return 1;
    }
    char dir[64];
    int n = snprintf(dir, sizeof(dir), "/tmp/cagt-%u", (unsigned)invoking_uid());
    if (n <= 0 || (size_t)n >= sizeof(dir)) return 0;
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) return 0;
    struct stat st;
    if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode)) return 0;
    if (st.st_uid != invoking_uid() && geteuid() != 0) return 0;
    n = snprintf(buf, sz, "%s/sock", dir);
    if (n <= 0 || (size_t)n >= sz) return 0;
    if (strlen(buf) >= 108) return 0;
    return 1;
}

static int get_curve(int profile, const CurvePoint **pts_out, int *n_out) {
    if (profile == CURVE_CUSTOM && custom_curve_n >= 2) {
        *pts_out = custom_curve;
        *n_out = custom_curve_n;
        return 1;
    }
    if (profile >= 0 && profile < CURVE_COUNT - 1) {
        *pts_out = builtin_curves[profile].pts;
        *n_out = builtin_curves[profile].n;
        return 1;
    }
    if (custom_curve_n >= 2) {
        *pts_out = custom_curve;
        *n_out = custom_curve_n;
        return 0;
    }
    *pts_out = curve_default;
    *n_out = (int)(sizeof(curve_default) / sizeof(curve_default[0]));
    return 0;
}

static int eval_curve(const CurvePoint *pts, int n, int temp) {
    if (n <= 0) return 50;
    if (temp <= pts[0].temp) return pts[0].fan;
    if (temp >= pts[n-1].temp) return pts[n-1].fan;
    for (int i = 0; i < n - 1; i++) {
        if (temp >= pts[i].temp && temp <= pts[i+1].temp) {
            int t0 = pts[i].temp, t1 = pts[i+1].temp;
            int f0 = pts[i].fan,  f1 = pts[i+1].fan;
            if (t1 == t0) return f0;
            return f0 + (f1 - f0) * (temp - t0) / (t1 - t0);
        }
    }
    return 100;
}

static int curve_for_profile(int profile, int temp) {
    const CurvePoint *pts;
    int n;
    get_curve(profile, &pts, &n);
    return eval_curve(pts, n, temp);
}

static int read_temp(GPUState *gp) {
    if (gp->hwmon_path[0] == '\0') return -1;
    char path[PATH_BUF];
    int idx = gp->temp_sensor + 1;
    snprintf(path, sizeof(path), "%s/temp%d_input", gp->hwmon_path, idx);
    int t = read_file_int(path);
    if (t < 0 && idx != 1) {
        snprintf(path, sizeof(path), "%s/temp1_input", gp->hwmon_path);
        t = read_file_int(path);
    }
    if (t < 0) return -1;
    return t / 1000;
}

static int read_rpm(GPUState *gp) {
    if (!gp->fan_rpm_available || gp->hwmon_path[0] == '\0') return -1;
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/fan1_input", gp->hwmon_path);
    return read_file_int(path);
}

static void detect_capabilities(GPUState *gp) {
    char path[PATH_BUF];

    snprintf(path, sizeof(path), "%s/gpu_od/fan_ctrl/fan_curve", gp->device_path);
    if (access(path, W_OK) == 0) {
        gp->fan_curve_available = 1;
        snprintf(gp->fan_curve_path, sizeof(gp->fan_curve_path), "%s", path);
    }

    snprintf(path, sizeof(path), "%s/pp_od_clk_voltage", gp->device_path);
    if (access(path, W_OK) == 0) gp->overdrive_available = 1;

    snprintf(path, sizeof(path), "%s/temp2_input", gp->hwmon_path);
    if (read_file_int(path) >= 0) gp->temp2_available = 1;

    snprintf(path, sizeof(path), "%s/temp3_input", gp->hwmon_path);
    if (read_file_int(path) >= 0) gp->temp3_available = 1;

    snprintf(path, sizeof(path), "%s/fan1_input", gp->hwmon_path);
    if (read_file_int(path) >= 0) gp->fan_rpm_available = 1;

    gp->fan_count = 1;
    for (int i = 2; i <= 4; i++) {
        snprintf(path, sizeof(path), "%s/pwm%d", gp->hwmon_path, i);
        if (access(path, F_OK) != 0) break;
        gp->fan_count = i;
    }

    snprintf(path, sizeof(path), "%s/pwm1", gp->hwmon_path);
    if (access(path, W_OK) == 0) gp->pwm_writable = 1;
    if (gp->fan_curve_available) gp->pwm_writable = 1;
}

static void make_short_name(GPUState *gp, const char *full, int idx) {
    const char *paren = strchr(full, '(');
    size_t len = paren ? (size_t)(paren - full) : strlen(full);
    while (len > 0 && full[len - 1] == ' ') len--;
    if (len == 0 || len >= sizeof(gp->short_name)) {
        snprintf(gp->short_name, sizeof(gp->short_name), "GPU %d", idx + 1);
        return;
    }
    memcpy(gp->short_name, full, len);
    gp->short_name[len] = 0;
}

static int detect_amd_gpus(void) {
    const char *base = "/sys/bus/pci/devices";
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    gpu_count = 0;

    while ((e = readdir(d)) != NULL && gpu_count < MAX_GPUS) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_BUF];
        char vendor[16] = {0};
        char device[16] = {0};

        snprintf(path, sizeof(path), "%s/%s/vendor", base, e->d_name);
        read_file_str(path, vendor, sizeof(vendor));
        snprintf(path, sizeof(path), "%s/%s/device", base, e->d_name);
        read_file_str(path, device, sizeof(device));

        if (strcmp(vendor, "0x1002") != 0) continue;

        GPUState *gp = &gpus[gpu_count];
        memset(gp, 0, sizeof(*gp));
        gp->curve_profile = CURVE_DEFAULT;
        gp->manual_speed = 50;
        gp->mode = MODE_CURVE;
        gp->last_pwm_written = -1;
        gp->curve_dirty = 1;

        const char *name = NULL;
        for (int i = 0; known_gpus[i].device_id; i++) {
            if (strcmp(device, known_gpus[i].device_id) == 0) {
                name = known_gpus[i].name;
                gp->generation = known_gpus[i].generation;
                break;
            }
        }
        if (name) snprintf(gp->name, sizeof(gp->name), "%s", name);
        else snprintf(gp->name, sizeof(gp->name), "AMD GPU (%s)", device);

        snprintf(gp->device_path, sizeof(gp->device_path), "%s/%s", base, e->d_name);

        snprintf(path, sizeof(path), "%s/hwmon", gp->device_path);
        DIR *hd = opendir(path);
        if (!hd) continue;

        struct dirent *he;
        while ((he = readdir(hd)) != NULL) {
            if (he->d_name[0] == '.') continue;
            snprintf(gp->hwmon_path, sizeof(gp->hwmon_path), "%s/%s", path, he->d_name);
            break;
        }
        closedir(hd);
        if (gp->hwmon_path[0] == '\0') continue;

        make_short_name(gp, gp->name, gpu_count);
        detect_capabilities(gp);
        gpu_count++;
    }
    closedir(d);
    return gpu_count;
}

static void detect_driver(void) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) return;
    char line[256];
    int amdgpu = 0, radeon = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "amdgpu ", 7) == 0) amdgpu = 1;
        else if (strncmp(line, "radeon ", 7) == 0) radeon = 1;
    }
    fclose(f);
    if (amdgpu) snprintf(driver_name, sizeof(driver_name), "amdgpu");
    else if (radeon) snprintf(driver_name, sizeof(driver_name), "radeon");
}

static void set_manual_enable(GPUState *gp) {
    if (gp->fan_curve_available) return;
    for (int i = 1; i <= gp->fan_count; i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/pwm%d_enable", gp->hwmon_path, i);
        write_file_int(path, PWM_MODE_MANUAL);
    }
}

static void write_pwm(GPUState *gp, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == gp->last_pwm_written) return;
    int pwm = (percent * PWM_MAX) / 100;
    for (int i = 1; i <= gp->fan_count; i++) {
        char path[PATH_BUF];
        snprintf(path, sizeof(path), "%s/pwm%d", gp->hwmon_path, i);
        write_file_int(path, pwm);
    }
    gp->last_pwm_written = percent;
}

static void write_flat_fan_curve(GPUState *gp, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == gp->last_pwm_written) return;
    int pwm = (percent * PWM_MAX) / 100;
    FILE *f = fopen(gp->fan_curve_path, "w");
    if (!f) return;
    for (int i = 0; i < 5; i++) {
        fprintf(f, "%d %d %d\n", i, 20 + i * 20, pwm);
    }
    fprintf(f, "c\n");
    fclose(f);
    gp->last_pwm_written = percent;
}

static void write_profile_fan_curve(GPUState *gp) {
    const CurvePoint *pts;
    int n;
    get_curve(gp->curve_profile, &pts, &n);
    FILE *f = fopen(gp->fan_curve_path, "w");
    if (!f) return;
    int max_pts = n < 5 ? n : 5;
    for (int i = 0; i < max_pts; i++) {
        int pwm = (pts[i].fan * PWM_MAX) / 100;
        fprintf(f, "%d %d %d\n", i, pts[i].temp, pwm);
    }
    fprintf(f, "c\n");
    fclose(f);
    gp->curve_dirty = 0;
    gp->last_pwm_written = -1;
}

static void apply_fan(GPUState *gp, int temp_c) {
    if (gp->mode == MODE_HARDWARE) return;

    if (gp->fan_curve_available) {
        if (gp->mode == MODE_MANUAL) {
            write_flat_fan_curve(gp, gp->manual_speed);
        } else {
            if (gp->curve_dirty) write_profile_fan_curve(gp);
        }
        return;
    }

    set_manual_enable(gp);
    int speed;
    if (gp->mode == MODE_MANUAL) speed = gp->manual_speed;
    else speed = curve_for_profile(gp->curve_profile, temp_c);
    write_pwm(gp, speed);
}

static void restore_gpu_auto(GPUState *gp) {
    if (gp->hwmon_path[0] == '\0') return;

    if (gp->fan_curve_available && gp->fan_curve_path[0]) {
        FILE *f = fopen(gp->fan_curve_path, "w");
        if (f) { fprintf(f, "r\n"); fclose(f); }
    }

    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/pwm1_enable", gp->hwmon_path);
    write_file_int(path, PWM_MODE_AUTO);
    if (read_file_int(path) == PWM_MODE_AUTO) { gp->restore_failed = 0; return; }

    write_file_int(path, PWM_MODE_MANUAL);
    snprintf(path, sizeof(path), "%s/pwm1", gp->hwmon_path);
    write_file_int(path, (50 * PWM_MAX) / 100);
    snprintf(path, sizeof(path), "%s/pwm1_enable", gp->hwmon_path);
    write_file_int(path, PWM_MODE_AUTO);

    if (read_file_int(path) != PWM_MODE_AUTO) gp->restore_failed = 1;
    else gp->restore_failed = 0;
}

static void restore_all(void) {
    for (int i = 0; i < gpu_count; i++) restore_gpu_auto(&gpus[i]);
}

static void detect_zero_rpm(GPUState *gp, int temp_c, int rpm) {
    gp->zero_rpm_active = 0;
    if (gp->fan_curve_available) return;
    if (temp_c <= 0 || temp_c >= 55) return;
    if (rpm != 0) return;
    char path[PATH_BUF];
    snprintf(path, sizeof(path), "%s/pwm1_enable", gp->hwmon_path);
    if (read_file_int(path) == PWM_MODE_AUTO) gp->zero_rpm_active = 1;
}

static void load_custom_curve(void) {
    const char *home = resolve_home();
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.config/cagt/custom_curve.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    int n = 0;
    char line[128];
    while (fgets(line, sizeof(line), f) && n < MAX_CURVE_POINTS) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        int t, fan;
        if (sscanf(line, "%d %d", &t, &fan) == 2) {
            if (t < 0 || t > 120) continue;
            if (fan < 0 || fan > 100) continue;
            custom_curve[n].temp = t;
            custom_curve[n].fan = fan;
            n++;
        }
    }
    fclose(f);

    if (n < 2) { custom_curve_n = 0; return; }
    for (int i = 1; i < n; i++) {
        if (custom_curve[i].temp <= custom_curve[i-1].temp) { custom_curve_n = 0; return; }
    }
    custom_curve_n = n;
}

static void load_settings(void) {
    const char *home = resolve_home();
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.config/cagt/config.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    int cur = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            if (sscanf(line, "[gpu%d]", &cur) != 1 || cur < 0 || cur >= MAX_GPUS) cur = -1;
        } else if (cur >= 0) {
            char key[64];
            int value;
            if (sscanf(line, "%63[^=]=%d", key, &value) == 2) {
                if (strcmp(key, "mode") == 0 && value >= 0 && value < MODE_COUNT)
                    gpus[cur].mode = value;
                else if (strcmp(key, "manual_mode") == 0)
                    gpus[cur].mode = value ? MODE_MANUAL : MODE_CURVE;
                else if (strcmp(key, "manual_speed") == 0 && value >= 0 && value <= 100)
                    gpus[cur].manual_speed = value;
                else if (strcmp(key, "curve_profile") == 0 && value >= 0 && value < CURVE_COUNT)
                    gpus[cur].curve_profile = value;
                else if (strcmp(key, "temp_sensor") == 0 && value >= 0 && value < SENSOR_COUNT)
                    gpus[cur].temp_sensor = value;
            } else if (sscanf(line, "%63[^=]=%d", key, &value) == 1) {
                /* fallthrough - not used */
            }
        } else {
            char key[64];
            int value;
            if (sscanf(line, "%63[^=]=%d", key, &value) == 2) {
                if (strcmp(key, "temp_unit") == 0 && value >= 0 && value < UNIT_COUNT)
                    temp_unit = value;
            }
        }
    }
    fclose(f);
}

static void save_settings(void) {
    const char *home = resolve_home();
    if (!home) return;

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.config/cagt", home);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/config.conf", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# CAGT configuration\n");
    fprintf(f, "temp_unit=%d\n\n", temp_unit);
    for (int i = 0; i < gpu_count; i++) {
        fprintf(f, "[gpu%d]\n", i);
        fprintf(f, "mode=%d\n", gpus[i].mode);
        fprintf(f, "manual_speed=%d\n", gpus[i].manual_speed);
        fprintf(f, "curve_profile=%d\n", gpus[i].curve_profile);
        fprintf(f, "temp_sensor=%d\n\n", gpus[i].temp_sensor);
    }
    fclose(f);
    chown_to_invoker(path);
    chown_to_invoker(dir);
}

static int init_logging(void) {
    const char *home = resolve_home();
    if (!home) return 0;

    char path[1024];
    snprintf(path, sizeof(path), "%s/.local", home);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return 0;
    snprintf(path, sizeof(path), "%s/.local/share", home);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return 0;
    snprintf(path, sizeof(path), "%s/.local/share/cagt", home);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) return 0;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);

    snprintf(path, sizeof(path), "%s/.local/share/cagt/fanlog_%s.csv", home, ts);
    log_file = fopen(path, "w");
    if (!log_file) return 0;
    fprintf(log_file, "timestamp,temp_c,fan_percent,fan_rpm,profile,mode,gpu\n");
    chown_to_invoker(path);
    return 1;
}

static void log_sample(int temp_c, int speed, int gpu_idx) {
    if (!logging_enabled || !log_file) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    int rpm = read_rpm(&gpus[gpu_idx]);
    fprintf(log_file, "%s,%d,%d,%d,%s,%s,%d\n",
            ts, temp_c, speed, rpm,
            curve_names[gpus[gpu_idx].curve_profile],
            mode_names[gpus[gpu_idx].mode],
            gpu_idx);
    fflush(log_file);
}

static void close_logging(void) {
    if (log_file) { fclose(log_file); log_file = NULL; }
    logging_enabled = 0;
}

static void check_ppfeaturemask(void) {
    int needs_od = 0;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].generation >= GEN_RDNA3 && !gpus[i].overdrive_available) {
            needs_od = 1;
            break;
        }
    }
    if (!needs_od) return;
    int mask = read_file_int("/sys/module/amdgpu/parameters/ppfeaturemask");
    if (mask < 0) return;
    if ((mask & PP_OVERDRIVE_BIT) == 0) {
        printf("[CAGT] WARNING: amdgpu.ppfeaturemask=%#x\n", mask);
        printf("[CAGT] Overdrive bit (0x%x) is disabled.\n", PP_OVERDRIVE_BIT);
        printf("[CAGT] Manual fan control may not work on RDNA3+.\n");
        printf("[CAGT] Add 'amdgpu.ppfeaturemask=0xffffffff' to kernel cmdline.\n");
    }
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        off += (size_t)n;
    }
    return 1;
}

static void send_response(int fd, const char *msg) {
    write_all(fd, msg, strlen(msg));
}

static int create_control_socket(void) {
    char sock_path[256];
    if (!make_socket_path(sock_path, sizeof(sock_path))) return -1;

    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
        if (connect(probe, (struct sockaddr *)&a, sizeof(a)) == 0) {
            close(probe);
            return -1;
        }
        close(probe);
    }

    unlink(sock_path);

    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        close(socket_fd); socket_fd = -1; return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(socket_fd); socket_fd = -1; return -1;
    }
    if (listen(socket_fd, 8) < 0) {
        close(socket_fd); unlink(sock_path); socket_fd = -1; return -1;
    }

    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

    chmod(sock_path, 0666);
    return 0;
}

static void cleanup_socket(void) {
    if (socket_fd >= 0) { close(socket_fd); socket_fd = -1; }
    char sock_path[256];
    if (make_socket_path(sock_path, sizeof(sock_path))) unlink(sock_path);
}

static void cmd_status(int fd) {
    if (gpu_count > 1) {
        char buf[SOCKET_BUF];
        int off = 0;
        for (int i = 0; i < gpu_count; i++) {
            GPUState *gp = &gpus[i];
            int t = read_temp(gp);
            int r = read_rpm(gp);
            int spd;
            if (gp->mode == MODE_MANUAL) spd = gp->manual_speed;
            else if (gp->mode == MODE_HARDWARE) spd = -1;
            else spd = curve_for_profile(gp->curve_profile, t);
            int n;
            if (spd < 0) {
                n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "GPU %d [%s]: temp=%d fan=n/a (%d RPM) mode=%s profile=%s%s\n",
                    i + 1, gp->short_name, t, r,
                    mode_names[gp->mode], curve_names[gp->curve_profile],
                    (i == active_idx) ? " [active]" : "");
            } else {
                n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "GPU %d [%s]: temp=%d fan=%d%% (%d RPM) mode=%s profile=%s%s\n",
                    i + 1, gp->short_name, t, spd, r,
                    mode_names[gp->mode], curve_names[gp->curve_profile],
                    (i == active_idx) ? " [active]" : "");
            }
            if (n < 0 || off + n >= (int)sizeof(buf)) break;
            off += n;
        }
        send_response(fd, buf);
    } else {
        GPUState *gp = &gpus[0];
        int t = read_temp(gp);
        int r = read_rpm(gp);
        int spd;
        if (gp->mode == MODE_MANUAL) spd = gp->manual_speed;
        else if (gp->mode == MODE_HARDWARE) spd = -1;
        else spd = curve_for_profile(gp->curve_profile, t);
        char buf[256];
        if (spd < 0) {
            snprintf(buf, sizeof(buf),
                "STATUS temp=%d fan=n/a (%d RPM) mode=%s profile=%s gpu=%d\n",
                t, r, mode_names[gp->mode],
                curve_names[gp->curve_profile], active_idx + 1);
        } else {
            snprintf(buf, sizeof(buf),
                "STATUS temp=%d fan=%d%% (%d RPM) mode=%s profile=%s gpu=%d\n",
                t, spd, r, mode_names[gp->mode],
                curve_names[gp->curve_profile], active_idx + 1);
        }
        send_response(fd, buf);
    }
}

static void handle_command(int fd, char *cmd) {
    cmd[strcspn(cmd, "\n")] = 0;

    if (strcmp(cmd, "status") == 0) {
        cmd_status(fd);
    }
    else if (strncmp(cmd, "gpu ", 4) == 0) {
        int n;
        if (!parse_int(cmd + 4, &n) || n < 1 || n > gpu_count) {
            send_response(fd, "ERROR invalid GPU index\n");
        } else {
            active_idx = n - 1;
            char r[64];
            snprintf(r, sizeof(r), "OK active gpu=%d (%s)\n", n, gpus[active_idx].short_name);
            send_response(fd, r);
        }
    }
    else if (strncmp(cmd, "fan ", 4) == 0) {
        char *arg = cmd + 4;
        GPUState *gp = &gpus[active_idx];

        if (strcmp(arg, "auto") == 0 || strcmp(arg, "curve") == 0) {
            gp->mode = MODE_CURVE;
            gp->curve_dirty = 1;
            gp->last_pwm_written = -1;
            send_response(fd, "OK mode=curve\n");
        }
        else if (strcmp(arg, "gpu") == 0 || strcmp(arg, "hardware") == 0) {
            restore_gpu_auto(gp);
            gp->mode = MODE_HARDWARE;
            gp->last_pwm_written = -1;
            send_response(fd, "OK mode=hardware\n");
        }
        else {
            int n;
            if (!parse_int(arg, &n) || n < 0 || n > 100) {
                send_response(fd, "ERROR speed must be 0-100\n");
            } else {
                gp->mode = MODE_MANUAL;
                gp->manual_speed = n;
                gp->last_pwm_written = -1;
                char r[64];
                snprintf(r, sizeof(r), "OK speed=%d\n", n);
                send_response(fd, r);
            }
        }
    }
    else if (strncmp(cmd, "curve ", 6) == 0) {
        char *arg = cmd + 6;
        if (strcmp(arg, "list") == 0) {
            send_response(fd, "CURVES: default, quiet, aggressive, linear, custom\n");
        } else {
            int c = -1;
            if (strcmp(arg, "default") == 0) c = CURVE_DEFAULT;
            else if (strcmp(arg, "quiet") == 0) c = CURVE_QUIET;
            else if (strcmp(arg, "aggressive") == 0) c = CURVE_AGGRESSIVE;
            else if (strcmp(arg, "linear") == 0) c = CURVE_LINEAR;
            else if (strcmp(arg, "custom") == 0) c = CURVE_CUSTOM;
            else if (!parse_int(arg, &c)) c = -1;

            if (c < 0 || c >= CURVE_COUNT) {
                send_response(fd, "ERROR invalid curve\n");
            } else if (c == CURVE_CUSTOM && custom_curve_n < 2) {
                send_response(fd, "ERROR custom curve not loaded\n");
            } else {
                GPUState *gp = &gpus[active_idx];
                gp->curve_profile = c;
                gp->curve_dirty = 1;
                gp->last_pwm_written = -1;
                char r[64];
                snprintf(r, sizeof(r), "OK curve=%s\n", curve_names[c]);
                send_response(fd, r);
            }
        }
    }
    else if (strncmp(cmd, "log ", 4) == 0) {
        char *arg = cmd + 4;
        if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0) {
            logging_enabled = 0;
            send_response(fd, "OK logging disabled\n");
            return;
        }
        int ms = 0;
        int len = (int)strlen(arg);
        if (len > 2 && arg[len-2] == 'm' && arg[len-1] == 's') {
            char num[32];
            if (len - 2 >= (int)sizeof(num)) { send_response(fd, "ERROR invalid interval\n"); return; }
            memcpy(num, arg, (size_t)(len - 2)); num[len-2] = 0;
            if (!parse_int(num, &ms)) { send_response(fd, "ERROR invalid interval\n"); return; }
        } else if (len > 1 && arg[len-1] == 's') {
            char num[32];
            if (len - 1 >= (int)sizeof(num)) { send_response(fd, "ERROR invalid interval\n"); return; }
            memcpy(num, arg, (size_t)(len - 1)); num[len-1] = 0;
            int s;
            if (!parse_int(num, &s)) { send_response(fd, "ERROR invalid interval\n"); return; }
            if (s > 3600) { send_response(fd, "ERROR interval too large\n"); return; }
            ms = s * 1000;
        } else {
            if (!parse_int(arg, &ms)) { send_response(fd, "ERROR invalid interval\n"); return; }
        }
        if (ms <= 0) { send_response(fd, "ERROR invalid interval\n"); return; }
        if (ms > 3600000) ms = 3600000;
        if (!log_file && !init_logging()) {
            send_response(fd, "ERROR could not open log file\n");
            return;
        }
        logging_enabled = 1;
        log_interval_ms = ms;
        char r[64];
        if (ms % 1000 == 0) snprintf(r, sizeof(r), "OK log interval = %ds\n", ms / 1000);
        else snprintf(r, sizeof(r), "OK log interval = %dms\n", ms);
        send_response(fd, r);
    }
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        char r[64];
        snprintf(r, sizeof(r), "CAGT daemon (PID: %d) has quit.\n", (int)getpid());
        send_response(fd, r);
        running = 0;
    }
    else if (strcmp(cmd, "help") == 0) {
        char r[512];
        snprintf(r, sizeof(r),
            "COMMANDS:\n"
            "  status\n"
            "  gpu <1-%d>\n"
            "  fan <0-100>\n"
            "  fan auto\n"
            "  fan gpu\n"
            "  curve <name|0-4>\n"
            "  curve list\n"
            "  log <1s|5s|10s|500ms|off>\n"
            "  quit\n",
            gpu_count);
        send_response(fd, r);
    }
    else {
        send_response(fd, "ERROR unknown command (try help)\n");
    }
}

static void poll_socket(void) {
    if (socket_fd < 0) return;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(socket_fd, &rfds);
    struct timeval tv = {0, 0};
    if (select(socket_fd + 1, &rfds, NULL, NULL, &tv) <= 0) return;

    int cfd = accept(socket_fd, NULL, NULL);
    if (cfd < 0) return;

    struct ucred cred;
    socklen_t clen = sizeof(cred);
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0) {
        close(cfd); return;
    }
    if (cred.uid != invoking_uid()) {
        send_response(cfd, "ERROR: Access denied\n");
        close(cfd); return;
    }

    fd_set crfds;
    FD_ZERO(&crfds);
    FD_SET(cfd, &crfds);
    struct timeval ctv = {1, 0};
    if (select(cfd + 1, &crfds, NULL, NULL, &ctv) > 0) {
        char buf[512];
        int total = 0;
        while (total < (int)sizeof(buf) - 1) {
            ssize_t n = read(cfd, buf + total, sizeof(buf) - 1 - (size_t)total);
            if (n <= 0) break;
            total += (int)n;
            if (memchr(buf, '\n', (size_t)total)) break;
        }
        if (total > 0) {
            buf[total] = 0;
            handle_command(cfd, buf);
        }
    }
    close(cfd);
}

static int send_command_to_daemon(int argc, char *argv[]) {
    char sock_path[256];
    if (!make_socket_path(sock_path, sizeof(sock_path))) return 1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) { close(fd); return 1; }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[CAGT] Daemon not running\n");
        close(fd);
        return 1;
    }

    char cmd[512] = {0};
    int off = 0;
    for (int i = 0; i < argc; i++) {
        int n = snprintf(cmd + off, sizeof(cmd) - (size_t)off, "%s%s",
                         (i > 0) ? " " : "", argv[i]);
        if (n < 0 || off + n >= (int)sizeof(cmd)) { close(fd); return 1; }
        off += n;
    }
    if (off + 1 >= (int)sizeof(cmd)) { close(fd); return 1; }
    cmd[off] = '\n';
    cmd[off + 1] = 0;

    write_all(fd, cmd, strlen(cmd));

    char resp[SOCKET_BUF];
    int total = 0;
    while (total < (int)sizeof(resp) - 1) {
        ssize_t n = read(fd, resp + total, sizeof(resp) - 1 - (size_t)total);
        if (n <= 0) break;
        total += (int)n;
    }
    resp[total] = 0;
    printf("%s", resp);

    close(fd);
    return 0;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) {
        printf("[CAGT] Daemon started with PID %d\n", (int)pid);
        fflush(stdout);
        _exit(0);
    }
    if (setsid() < 0) exit(1);

    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) _exit(0);

    umask(022);
    if (chdir("/") < 0) { }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        if (devnull > 2) close(devnull);
    }
}

static int run_daemon(void) {
    if (create_control_socket() < 0) {
        fprintf(stderr, "[CAGT] Warning: could not create control socket\n");
    }

    long long last_log_ms = 0;

    while (running && !g_signal_flag) {
        for (int i = 0; i < gpu_count; i++) {
            GPUState *gp = &gpus[i];
            int t = read_temp(gp);
            if (t < 0) t = 0;
            gp->last_temp_c = t;
            gp->last_rpm = read_rpm(gp);
            if (t > gp->max_temp_c) gp->max_temp_c = t;
            detect_zero_rpm(gp, t, gp->last_rpm);
            apply_fan(gp, t);
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        if (logging_enabled && (now_ms - last_log_ms >= log_interval_ms)) {
            GPUState *gp = &gpus[active_idx];
            int spd;
            if (gp->mode == MODE_MANUAL) spd = gp->manual_speed;
            else if (gp->mode == MODE_HARDWARE) spd = 0;
            else spd = curve_for_profile(gp->curve_profile, gp->last_temp_c);
            log_sample(gp->last_temp_c, spd, active_idx);
            last_log_ms = now_ms;
        }

        poll_socket();

        struct timespec st = {0, TICK_MS * 1000000L};
        nanosleep(&st, NULL);
    }

    restore_all();
    return 0;
}

static double convert_temp(int c) {
    switch (temp_unit) {
        case UNIT_F: return (c * 9.0 / 5.0) + 32.0;
        case UNIT_K: return c + 273.15;
        default: return (double)c;
    }
}

static void draw_bar(WINDOW *win, int y, int x, int width, double value,
                     double max, const char *label, const char *unit) {
    int filled = 0;
    if (max > 0 && value > 0) filled = (int)((value * width) / max);
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;
    mvwprintw(win, y, x, "%s%3.0f%s [", label, value, unit);
    for (int i = 0; i < width; i++) {
        if (i < filled) { wattron(win, A_REVERSE); waddch(win, ' '); wattroff(win, A_REVERSE); }
        else waddch(win, ' ');
    }
    waddch(win, ']');
}

static void draw_curve_info(WINDOW *win, int y, int x, int temp_c, int profile) {
    const CurvePoint *pts;
    int n;
    get_curve(profile, &pts, &n);

    wattron(win, A_BOLD);
    mvwprintw(win, y, x, "Fan Curve [%s]", curve_names[profile]);
    wattroff(win, A_BOLD);

    for (int i = 0; i < n; i++) {
        int active = (temp_c >= pts[i].temp &&
                     (i == n - 1 || temp_c < pts[i+1].temp));
        double dt = convert_temp(pts[i].temp);
        if (active) wattron(win, A_BOLD);
        mvwprintw(win, y + 1 + i, x, "  %s %3.0f%s -> %3d%%",
                 active ? ">" : " ",
                 dt, unit_suffix[temp_unit],
                 pts[i].fan);
        if (active) wattroff(win, A_BOLD);
    }
}

static void draw_controls(WINDOW *win, int start_y, int x) {
    int y = start_y;
    wattron(win, A_BOLD);
    mvwprintw(win, y++, x, "--- Controls ---");
    wattroff(win, A_BOLD);
    y++;
    if (gpu_count > 1) mvwprintw(win, y++, x, " 1-%d       Select GPU", gpu_count);
    mvwprintw(win, y++, x, " M         Cycle Mode (curve/manual/hardware)");
    mvwprintw(win, y++, x, " T         Cycle °C / °F / K");
    mvwprintw(win, y++, x, " S         Cycle temp sensor");
    mvwprintw(win, y++, x, " C         Cycle fan curve");
    mvwprintw(win, y++, x, " R         Reset peak temperature");
    mvwprintw(win, y++, x, " +/-       Adjust fan by 5%%");
    mvwprintw(win, y++, x, " Up/Down   Adjust fan by 1%%");
    mvwprintw(win, y++, x, " 0         Set fan 0%%");
    mvwprintw(win, y++, x, " Q         Quit (restore auto)");
}

static void draw_ui(int temp_c, int speed) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    GPUState *gp = &gpus[active_idx];
    double display_temp = convert_temp(temp_c);
    int mode_color = (gp->mode == MODE_MANUAL) ? 3 :
                     (gp->mode == MODE_HARDWARE) ? 2 : 1;

    move(0, 0);
    clrtoeol();
    attron(A_REVERSE);
    printw(" CAGT - AMD GPU Fan Control  ");
    if (gpu_count > 1) {
        for (int i = 0; i < gpu_count; i++) {
            if (i == active_idx) {
                attron(A_BOLD);
                printw(" [%d:%s] ", i + 1, gpus[i].short_name);
                attroff(A_BOLD);
            } else {
                printw("  %d:%s  ", i + 1, gpus[i].short_name);
            }
        }
    }
    attroff(A_REVERSE);

    int row = 2;

    attron(A_BOLD);
    mvprintw(row, 2, "GPU Information");
    attroff(A_BOLD);
    mvprintw(row + 1, 4, "Model:  %s", gp->name);
    mvprintw(row + 2, 4, "Driver: %s", driver_name);
    mvprintw(row + 3, 4, "Fans:   %d", gp->fan_count);
    if (gp->fan_rpm_available && gp->last_rpm >= 0) {
        mvprintw(row + 3, 20, "(%d RPM)", gp->last_rpm);
    }
    clrtoeol();
    mvprintw(row + 4, 4, "Path:   %s", gp->hwmon_path);
    clrtoeol();

    if (gp->restore_failed) {
        attron(COLOR_PAIR(3) | A_BOLD);
        mvprintw(row + 5, 4, "WARNING: Auto fan restore may have failed! Reboot to recover.");
        attroff(COLOR_PAIR(3) | A_BOLD);
        row++;
    }

    int srow = row + 6;

    move(srow, 0);
    clrtoeol();
    attron(A_BOLD);
    mvprintw(srow, 2, "Status  [%s]", unit_names[temp_unit]);
    attroff(A_BOLD);

    mvprintw(srow + 1, 4, "Mode: ");
    if (has_colors()) attron(COLOR_PAIR(mode_color) | A_BOLD);
    printw("%s", (gp->mode == MODE_MANUAL) ? "MANUAL" :
                 (gp->mode == MODE_HARDWARE) ? "HARDWARE" :
                 "AUTO (curve)");
    if (has_colors()) attroff(COLOR_PAIR(mode_color) | A_BOLD);

    if (!gp->overdrive_available && gp->generation >= GEN_RDNA3) {
        attron(COLOR_PAIR(3));
        printw("  [Overdrive not enabled!]");
        attroff(COLOR_PAIR(3));
    }
    if (gp->zero_rpm_active) printw("  [Zero RPM active]");
    clrtoeol();

    mvprintw(srow + 2, 4, "Sensor: ");
    printw("%s", sensor_names[gp->temp_sensor]);
    if (gp->temp_sensor == SENSOR_JUNCTION && !gp->temp2_available)
        printw(" (unavailable, using edge)");
    else if (gp->temp_sensor == SENSOR_MEMORY && !gp->temp3_available)
        printw(" (unavailable, using edge)");
    clrtoeol();

    mvprintw(srow + 3, 4, "Temperature: ");
    if (has_colors()) {
        if (temp_c < 60) attron(COLOR_PAIR(1) | A_BOLD);
        else if (temp_c < 80) attron(COLOR_PAIR(2) | A_BOLD);
        else attron(COLOR_PAIR(3) | A_BOLD);
    }
    if (temp_unit == UNIT_K) printw("%.1f K ", display_temp);
    else if (temp_unit == UNIT_F) printw("%.1f°F ", display_temp);
    else printw("%.1f°C ", display_temp);
    if (has_colors()) attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3) | A_BOLD);

    if (gp->max_temp_c > 0) {
        double dm = convert_temp(gp->max_temp_c);
        if (temp_unit == UNIT_K) printw("(Max: %.1f K)", dm);
        else if (temp_unit == UNIT_F) printw("(Max: %.1f°F)", dm);
        else printw("(Max: %.1f°C)", dm);
    }
    clrtoeol();

    mvprintw(srow + 4, 4, "Fan Speed: ");
    if (has_colors()) {
        if (speed < 40) attron(COLOR_PAIR(1) | A_BOLD);
        else if (speed < 70) attron(COLOR_PAIR(2) | A_BOLD);
        else attron(COLOR_PAIR(3) | A_BOLD);
    }
    printw("%d%%", speed);
    if (has_colors()) attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3) | A_BOLD);
    clrtoeol();

    time_t now = time(NULL);
    int elapsed = (int)(now - start_time);
    mvprintw(srow + 4, 30, "Runtime: %02d:%02d:%02d",
             elapsed / 3600, (elapsed % 3600) / 60, elapsed % 60);
    clrtoeol();

    int bar_w = max_x - 22;
    if (bar_w > 60) bar_w = 60;
    if (bar_w < 10) bar_w = 10;

    int br = srow + 6;

    move(br, 0);
    clrtoeol();
    if (has_colors()) {
        if (speed < 40) attron(COLOR_PAIR(1));
        else if (speed < 70) attron(COLOR_PAIR(2));
        else attron(COLOR_PAIR(3));
    }
    draw_bar(stdscr, br, 4, bar_w, (double)speed, 100.0, "Fan   ", "%");
    if (has_colors()) attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3));

    double tmax;
    switch (temp_unit) {
        case UNIT_F: tmax = 212.0; break;
        case UNIT_K: tmax = 373.15; break;
        default: tmax = 100.0;
    }
    double td = display_temp;
    if (td > tmax) td = tmax;
    if (td < 0) td = 0;

    char tlabel[16];
    if (temp_unit == UNIT_K) snprintf(tlabel, sizeof(tlabel), "Temp K ");
    else if (temp_unit == UNIT_F) snprintf(tlabel, sizeof(tlabel), "Temp °F ");
    else snprintf(tlabel, sizeof(tlabel), "Temp °C ");

    move(br + 2, 0);
    clrtoeol();
    if (has_colors()) {
        if (temp_c < 60) attron(COLOR_PAIR(1));
        else if (temp_c < 80) attron(COLOR_PAIR(2));
        else attron(COLOR_PAIR(3));
    }
    draw_bar(stdscr, br + 2, 4, bar_w, td, tmax, tlabel, "");
    if (has_colors()) attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3));

    for (int i = 0; i < 14; i++) {
        move(br + 4 + i, 4);
        clrtoeol();
    }
    draw_curve_info(stdscr, br + 4, 4, temp_c, gp->curve_profile);

    int cc = (max_x > 80) ? 46 : 4;
    int cr = (max_x > 80) ? br + 4 : br + 18;
    draw_controls(stdscr, cr, cc);

    if (!gp->overdrive_available && gp->generation >= GEN_RDNA3) {
        attron(COLOR_PAIR(3) | A_BOLD);
        int wr = max_y - 2;
        int mask = read_file_int("/sys/module/amdgpu/parameters/ppfeaturemask");
        if (mask >= 0 && (mask & PP_OVERDRIVE_BIT) == 0) {
            mvprintw(wr, 2, "ppfeaturemask=%#x - overdrive disabled!", mask);
            mvprintw(wr + 1, 2, "Set amdgpu.ppfeaturemask=0xffffffff in kernel cmdline.");
        } else {
            mvprintw(wr, 2, "Overdrive unavailable despite ppfeaturemask - check kernel.");
        }
        attroff(COLOR_PAIR(3) | A_BOLD);
    }

    refresh();
}

static void handle_key(int ch, int temp_c, int speed) {
    GPUState *gp = &gpus[active_idx];

    if (ch >= '1' && ch <= '9' && (ch - '1') < gpu_count) {
        active_idx = ch - '1';
        GPUState *np = &gpus[active_idx];
        if (np->temp_sensor == SENSOR_JUNCTION && !np->temp2_available)
            np->temp_sensor = SENSOR_EDGE;
        if (np->temp_sensor == SENSOR_MEMORY && !np->temp3_available)
            np->temp_sensor = SENSOR_EDGE;
        return;
    }

    switch (ch) {
        case 'q': case 'Q':
            running = 0;
            break;
        case 'm': case 'M':
            gp->mode = (gp->mode + 1) % MODE_COUNT;
            if (gp->mode == MODE_MANUAL) gp->manual_speed = speed;
            if (gp->mode == MODE_CURVE) gp->curve_dirty = 1;
            if (gp->mode == MODE_HARDWARE) restore_gpu_auto(gp);
            gp->last_pwm_written = -1;
            break;
        case 't': case 'T':
            temp_unit = (temp_unit + 1) % UNIT_COUNT;
            break;
        case 's': case 'S':
            gp->temp_sensor = (gp->temp_sensor + 1) % SENSOR_COUNT;
            if (gp->temp_sensor == SENSOR_JUNCTION && !gp->temp2_available)
                gp->temp_sensor = SENSOR_MEMORY;
            if (gp->temp_sensor == SENSOR_MEMORY && !gp->temp3_available)
                gp->temp_sensor = SENSOR_EDGE;
            break;
        case 'c': case 'C':
            gp->curve_profile = (gp->curve_profile + 1) % CURVE_COUNT;
            if (gp->curve_profile == CURVE_CUSTOM && custom_curve_n < 2)
                gp->curve_profile = CURVE_DEFAULT;
            gp->curve_dirty = 1;
            gp->last_pwm_written = -1;
            break;
        case 'r': case 'R':
            gp->max_temp_c = temp_c;
            break;
        case '+': case '=':
            if (gp->mode != MODE_MANUAL) { gp->mode = MODE_MANUAL; gp->manual_speed = speed; }
            gp->manual_speed += 5;
            if (gp->manual_speed > 100) gp->manual_speed = 100;
            gp->last_pwm_written = -1;
            break;
        case '-': case '_':
            if (gp->mode != MODE_MANUAL) { gp->mode = MODE_MANUAL; gp->manual_speed = speed; }
            gp->manual_speed -= 5;
            if (gp->manual_speed < 0) gp->manual_speed = 0;
            gp->last_pwm_written = -1;
            break;
        case KEY_UP: case KEY_RIGHT:
            if (gp->mode != MODE_MANUAL) { gp->mode = MODE_MANUAL; gp->manual_speed = speed; }
            gp->manual_speed += 1;
            if (gp->manual_speed > 100) gp->manual_speed = 100;
            gp->last_pwm_written = -1;
            break;
        case KEY_DOWN: case KEY_LEFT:
            if (gp->mode != MODE_MANUAL) { gp->mode = MODE_MANUAL; gp->manual_speed = speed; }
            gp->manual_speed -= 1;
            if (gp->manual_speed < 0) gp->manual_speed = 0;
            gp->last_pwm_written = -1;
            break;
        case '0':
            if (gp->mode != MODE_MANUAL) gp->mode = MODE_MANUAL;
            gp->manual_speed = 0;
            gp->last_pwm_written = -1;
            break;
        case KEY_RESIZE:
            endwin();
            clear();
            refresh();
            clearok(stdscr, TRUE);
            break;
    }
}

static void print_help(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --daemon      Run in background\n");
    printf("  --log         Enable CSV logging\n");
    printf("  --help, -h    Show this help\n");
    printf("\n");
    printf("Daemon commands (via %s <command>):\n", prog);
    printf("  status\n");
    printf("  gpu <1-N>\n");
    printf("  fan <0-100>\n");
    printf("  fan auto\n");
    printf("  fan gpu\n");
    printf("  curve <name|0-4>\n");
    printf("  curve list\n");
    printf("  log <1s|5s|10s|500ms|off>\n");
    printf("  quit\n");
}

int main(int argc, char *argv[]) {
    char sock_path[256];
    if (!make_socket_path(sock_path, sizeof(sock_path))) {
        fprintf(stderr, "[CAGT] Could not determine socket path\n");
        return 1;
    }

    int is_command = 0;
    int has_daemon = 0;
    int has_log = 0;
    int has_help = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) has_daemon = 1;
        else if (strcmp(argv[i], "--log") == 0) has_log = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) has_help = 1;
        else is_command = 1;
    }

    if (has_help && !is_command) {
        print_help(argv[0]);
        return 0;
    }

    if (is_command) {
        struct stat st;
        if (stat(sock_path, &st) != 0) {
            printf("[CAGT] Daemon is not running.\n");
            printf("[CAGT] Start it with: sudo %s --daemon\n", argv[0]);
            return 1;
        }
        return send_command_to_daemon(argc - 1, &argv[1]);
    }

    if (argc == 1 && access(sock_path, F_OK) == 0) {
        printf("[CAGT] Daemon already running.\n");
        printf("[CAGT] Commands: status | gpu <N> | fan <0-100> | fan auto | fan gpu | curve <name> | log <interval> | quit\n");
        return 0;
    }

    daemon_mode = has_daemon;
    logging_enabled = has_log;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (!detect_amd_gpus()) {
        printf("[CAGT] No AMD GPU found.\n");
        return 1;
    }

    detect_driver();
    load_settings();
    load_custom_curve();

    for (int i = 0; i < gpu_count; i++) {
        gpus[i].last_pwm_written = -1;
        gpus[i].curve_dirty = 1;
    }

    start_time = time(NULL);

    printf("[CAGT] Found %d AMD GPU(s)\n", gpu_count);
    for (int i = 0; i < gpu_count; i++) {
        printf("[CAGT] GPU %d: %s\n", i + 1, gpus[i].name);
        if (gpus[i].fan_curve_available)
            printf("[CAGT]   Using fan_curve interface (RDNA3+)\n");
        printf("[CAGT]   Fans detected: %d\n", gpus[i].fan_count);
    }
    printf("[CAGT] Driver: %s\n", driver_name);
    check_ppfeaturemask();

    int any_writable = 0;
    for (int i = 0; i < gpu_count; i++) {
        if (gpus[i].pwm_writable) { any_writable = 1; break; }
    }
    if (!any_writable) {
        printf("[CAGT] Cannot write to any GPU hwmon — permission denied.\n");
        printf("[CAGT] Run with: sudo %s\n", argv[0]);
        return 1;
    }

    if (logging_enabled) init_logging();

    if (daemon_mode) {
        printf("[CAGT] Starting daemon...\n");
        fflush(stdout);
        daemonize();
        atexit(cleanup_socket);
        atexit(close_logging);
        atexit(save_settings);
        return run_daemon();
    }

    atexit(close_logging);
    atexit(save_settings);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    timeout(TICK_MS);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
    }

    long long last_log_ms = 0;

    while (running && !g_signal_flag) {
        for (int i = 0; i < gpu_count; i++) {
            GPUState *gp = &gpus[i];
            int t = read_temp(gp);
            if (t < 0) t = 0;
            gp->last_temp_c = t;
            gp->last_rpm = read_rpm(gp);
            if (t > gp->max_temp_c) gp->max_temp_c = t;
            detect_zero_rpm(gp, t, gp->last_rpm);
            apply_fan(gp, t);
        }

        GPUState *gp = &gpus[active_idx];
        int t = gp->last_temp_c;
        int speed;
        if (gp->mode == MODE_MANUAL) speed = gp->manual_speed;
        else if (gp->mode == MODE_HARDWARE) speed = 0;
        else speed = curve_for_profile(gp->curve_profile, t);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (logging_enabled && (now_ms - last_log_ms >= log_interval_ms)) {
            log_sample(t, speed, active_idx);
            last_log_ms = now_ms;
        }

        draw_ui(t, speed);

        int ch = getch();
        if (ch != ERR) handle_key(ch, t, speed);
    }

    endwin();
    restore_all();

    if (gpus[active_idx].restore_failed) {
        printf("[CAGT] WARNING: Could not restore automatic fan control.\n");
        printf("[CAGT] Reboot to fully restore.\n");
    }
    printf("[CAGT] Exited cleanly.\n");
    return 0;
}