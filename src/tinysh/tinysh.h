/* tinysh.h — TinyOS Genesis v0.1 系统默认 Shell 公共头
 * 对应《tinysh 使用手册》§4 命令表 / §5 错误码 / §6 REPL 约定
 */
#ifndef TINYSH_H
#define TINYSH_H

#include <stddef.h>

#define TINYSH_PROMPT  "tinysh> "
#define TINYSH_BANNER  "TinyOS Genesis v0.1"
#define HISTORY_MAX    32
#define LINE_MAX       256
#define ARG_MAX        32
#define NAME_MAX       64

/* §5 系统错误码规范 */
enum {
    E_OK     = 0,   /* OK，命令执行成功 */
    E_UNKNOWN = 1,  /* 未知命令，没有找到该内置命令 */
    E_ARGC   = 2,   /* 参数数量错误，过多或者缺少必填参数 */
    E_NOENT  = 3,   /* 文件/路径不存在 */
    E_PERM   = 4,   /* 权限不足，禁止操作 */
    E_INVAL  = 5,   /* 无效参数，参数格式错误 */
    E_IO     = 6,   /* IO 读写失败 */
    E_NOPID  = 7    /* 进程 PID 不存在 */
};

typedef int (*cmd_fn)(int argc, char **argv);

typedef struct {
    const char *name;     /* 命令名（命令名自动补全基于此字段）*/
    cmd_fn      func;     /* 处理函数，统一返回 int 错误码 */
    const char *usage;    /* 用法 */
    const char *desc;     /* 简短描述 */
} cmd_t;

/* 历史记录（最多 32 条）；hist_prev/hist_next 仅在 tinysh.c 内部使用 */
void hist_init(void);
void hist_add(const char *line);

/* 行读取：TTY 下支持历史(↑/↓)与 Tab 命令名补全；非 TTY 退化为逐行读取 */
int  read_line(char *buf, int n);

/* 命令注册表 */
const cmd_t *cmd_find(const char *name);
int          cmd_count(void);
const cmd_t *cmd_get(int i);

/* 错误码 -> 人类可读信息 */
const char *errmsg(int code);

#endif /* TINYSH_H */
