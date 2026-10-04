/* kernel_api_host.c — 宿主机（开发/测试）后端
 *
 * 该后端把 fs_api / proc_api / hw_api 映射到宿主机 POSIX/Win32 调用，
 * 让 tinysh 可以在普通 PC 上编译运行、验证手册里的行为（REPL、命令、
 * 错误码）。它不依赖 TinyOS 内核，仅用于开发自测。
 *
 * 真正的产品后端见 kernel_api_tinyos.c。
 */
#include "kernel_api.h"
#include "tinysh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

/* ---------------- fs_api ---------------- */

int fs_getcwd(char *buf, int n) {
#ifdef _WIN32
    return GetCurrentDirectoryA((DWORD)n, buf) ? 0 : E_IO;
#else
    return getcwd(buf, (size_t)n) ? 0 : E_IO;
#endif
}

int fs_chdir(const char *path) {
#ifdef _WIN32
    return SetCurrentDirectoryA(path) ? 0 : E_NOENT;
#else
    return chdir(path) == 0 ? 0 : E_NOENT;
#endif
}

int fs_list(const char *path, fs_entry **out, int *count) {
    static fs_entry arr[1024];
    int n = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pat[640];
    snprintf(pat, sizeof pat, "%s/*", path);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return E_NOENT;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (n < 1024) {
            strncpy(arr[n].name, fd.cFileName, 63); arr[n].name[63] = 0;
            arr[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            arr[n].size   = (long)fd.nFileSizeLow;
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    struct dirent *e;
    struct stat st;
    if (!d) return E_NOENT;
    while ((e = readdir(d)) != NULL && n < 1024) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        strncpy(arr[n].name, e->d_name, 63); arr[n].name[63] = 0;
        stat(e->d_name, &st);
        arr[n].is_dir = S_ISDIR(st.st_mode);
        arr[n].size   = (long)st.st_size;
        n++;
    }
    closedir(d);
#endif
    *out = arr; *count = n;
    return 0;
}

long fs_read(const char *path, char *buf, long n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -(long)E_NOENT;
    long r = (long)fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (r < 0) return -(long)E_IO;
    return r;
}

int fs_mkdir(const char *name) {
#ifdef _WIN32
    return CreateDirectoryA(name, NULL) ? 0 : E_IO;
#else
    return mkdir(name, 0755) == 0 ? 0 : E_IO;
#endif
}

int fs_remove(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return E_NOENT;
    if (a & FILE_ATTRIBUTE_DIRECTORY) return E_PERM;   /* v0.1 不允许删目录 */
    return DeleteFileA(path) ? 0 : E_IO;
#else
    struct stat st;
    if (stat(path, &st) != 0) return E_NOENT;
    if (S_ISDIR(st.st_mode)) return E_PERM;            /* v0.1 不允许删目录 */
    return remove(path) == 0 ? 0 : E_IO;
#endif
}

int fs_touch(const char *name) {
    FILE *f = fopen(name, "a");
    if (!f) return E_IO;
    fclose(f);
    return 0;
}

/* ---------------- proc_api ---------------- */

void proc_list_print(void) {
    int pid =
#ifdef _WIN32
        (int)GetCurrentProcessId();
#else
        (int)getpid();
#endif
    printf("PID\tNAME\tSTATE\tMEM\n");
    printf("%d\ttinysh\trunning\t-\n", pid);
    printf("(宿主机后端仅显示当前进程；TinyOS 后端将列出真实进程表)\n");
}

int proc_kill(int pid) {
    (void)pid;
    /* 宿主机自测后端不真正发信号，按手册返回 PID 不存在 */
    return E_NOPID;
}

/* ---------------- hw_api ---------------- */

int hw_devlist(hw_dev **out, int *count) {
    static hw_dev arr[8];
    *out = arr; *count = 0;   /* 宿主机无硬件设备 */
    return 0;
}

int hw_readdev(const char *dev, unsigned addr, unsigned *val) {
    (void)dev; (void)addr; (void)val;
    return E_IO;   /* 宿主机不支持硬件寄存器访问 */
}

int hw_writedev(const char *dev, unsigned addr, unsigned val) {
    (void)dev; (void)addr; (void)val;
    return E_IO;
}

/* ---------------- 系统信息 ---------------- */

void sys_version(char *buf, int n) {
    snprintf(buf, n, "TinyOS Genesis v0.1  |  tinysh v0.1  |  host build");
}

void sys_sysinfo(char *buf, int n) {
    snprintf(buf, n,
        "CPU: host  Mem total: -  Free: -  Uptime: -  Process count: 1");
}

void sys_date(char *buf, int n) {
    time_t t = time(NULL);
    char *s = ctime(&t);
    if (s) { s[strcspn(s, "\n")] = 0; snprintf(buf, n, "%s", s); }
    else   { buf[0] = 0; }
}
