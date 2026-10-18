/* ============================================================
 * TinyOS sample program: fw  (file worker)
 * Language: C -- user-space IPC worker for the kernel file manager.
 * Each instance is spawned as an independent task and loops forever
 * receiving FW_MAGIC_JOB / FW_MAGIC_BYE commands, performing chunked
 * file operations, and reporting progress back to pid 0 (master).
 * All user-visible strings are pure ASCII (CP437 VGA font).
 * ============================================================ */
#include "api_user.h"

#define FW_MAGIC_JOB  0x464A4F42u  /* 'FJOB' master -> worker */
#define FW_MAGIC_BYE  0x46425945u  /* 'FBYE' master -> worker: exit */
#define FW_MAGIC_PROG 0x46505247u  /* 'FPRG' worker -> master */

#define FW_OP_COPY   1   /* src -> dst (overwrite) */
#define FW_OP_MOVE   2   /* copy src->dst then delete src */
#define FW_OP_DELETE 3   /* delete src (file or empty dir) */
#define FW_OP_MKDIR  4   /* create directory src */
#define FW_OP_TOUCH  5   /* create empty file src */
#define FW_OP_FAULT  7   /* TEST ONLY: deliberately crash (read *0) */

#define FW_CHUNK 2048    /* copy/move read size */

/* master -> worker command (fixed 88-byte wire layout) */
typedef struct {
    u32  magic;       /* FW_MAGIC_JOB / FW_MAGIC_BYE */
    u32  job_id;      /* echoed back in progress reports */
    u8   op;          /* FW_OP_* */
    u8   pad[3];
    char src[36];     /* absolute path (35 chars + NUL) */
    char dst[36];     /* absolute path (unused for delete/mkdir/touch/fault) */
} fw_cmd;

/* worker -> master progress report (fixed 64-byte wire layout) */
typedef struct {
    u32  magic;       /* FW_MAGIC_PROG */
    u32  job_id;
    u32  done;        /* bytes completed so far */
    u32  total;       /* total bytes (0 if unknown) */
    u32  fin;         /* 1 = job finished (success OR failure) */
    int  err;         /* 0 = ok/progress; nonzero = failure */
    char msg[40];     /* short ASCII status/error */
} fw_prog;

/* Bounded copy of a short ASCII status string into a 40-byte field.
 * Static buffers only; no libc string helpers. */
static void set_msg(char *dst, const char *src) {
    for (int i = 0; i < 39; i++) {
        dst[i] = src[i];
        if (!src[i]) return;
    }
    dst[39] = 0;
}

/* Send a progress report. When progress=1 and the mailbox is full
 * (ipc_send != 0), skip this message and keep working. */
static void report(tinyos_api_t *api, u32 job_id, u32 done, u32 total,
                   u32 fin, int err, const char *msg, int progress) {
    fw_prog p;
    p.magic  = FW_MAGIC_PROG;
    p.job_id = job_id;
    p.done   = done;
    p.total  = total;
    p.fin    = fin;
    p.err    = err;
    set_msg(p.msg, msg);
    int r = api->ipc_send(0, &p, sizeof(p));
    if (progress && r != 0) { /* mailbox full: skip this progress msg */ }
}

/* Chunked copy src -> dst. msg is the progress hint ("copying"/"moving").
 * Returns 0 on success; on read failure it already reported fin=1 and
 * returns -1. */
static int copy_loop(tinyos_api_t *api, const fw_cmd *cmd, const char *msg) {
    static u8 chunk[FW_CHUNK];   /* static buffer, no malloc */
    long off = 0, done = 0;
    int nchunks = 0;

    for (;;) {
        int n = api->fs_read_at(cmd->src, chunk, FW_CHUNK, off);
        if (n < 0) {
            report(api, cmd->job_id, (u32)done, 0, 1, -1, "read failed", 0);
            return -1;
        }
        if (n == 0) break;  /* EOF */
        api->fs_write_at(cmd->dst, chunk, n, off);
        off  += n;
        done += n;
        api->task_yield();  /* yield after every chunk */
        nchunks++;
        if ((nchunks % 2) == 0) {
            report(api, cmd->job_id, (u32)done, 0, 0, 0, msg, 1);
        }
    }
    return 0;
}

void user_main(tinyos_api_t *api) {
    fw_cmd cmd;
    int from;

    for (;;) {
        int r = api->ipc_recv(&cmd, sizeof(cmd), &from, -1);  /* blocking */
        if (r < (int)sizeof(fw_cmd)) {
            if (r <= 0) api->task_yield();  /* defensive; should not happen */
            continue;
        }

        if (cmd.magic == FW_MAGIC_BYE) {
            api->task_exit(0);  /* never returns */
            return;
        }
        if (cmd.magic != FW_MAGIC_JOB) continue;  /* unknown magic: ignore */

        switch (cmd.op) {
        case FW_OP_COPY: {
            int is_dir; long total;
            if (api->stat(cmd.src, &is_dir, &total) != 0) {
                report(api, cmd.job_id, 0, 0, 1, -1, "stat failed", 0);
                break;
            }
            if (is_dir) {
                report(api, cmd.job_id, 0, 0, 1, -1, "src is a directory", 0);
                break;
            }
            int rc = copy_loop(api, &cmd, "copying");
            if (rc == 0)
                report(api, cmd.job_id, (u32)total, (u32)total, 1, 0, "ok", 0);
            break;
        }
        case FW_OP_MOVE: {
            int is_dir; long total;
            if (api->stat(cmd.src, &is_dir, &total) != 0) {
                report(api, cmd.job_id, 0, 0, 1, -1, "stat failed", 0);
                break;
            }
            if (is_dir) {
                report(api, cmd.job_id, 0, 0, 1, -1, "src is a directory", 0);
                break;
            }
            int rc = copy_loop(api, &cmd, "moving");
            if (rc != 0) break;  /* copy failed, already reported */
            int rm = api->rm_file(cmd.src);
            if (rm != 0)
                report(api, cmd.job_id, (u32)total, (u32)total, 1, -1,
                       "moved but src delete failed", 0);
            else
                report(api, cmd.job_id, (u32)total, (u32)total, 1, 0, "moved", 0);
            break;
        }
        case FW_OP_DELETE: {
            int rc = api->rm_file(cmd.src);
            report(api, cmd.job_id, 1, 1, 1, rc ? -1 : 0,
                   rc ? "delete failed" : "deleted", 0);
            break;
        }
        case FW_OP_MKDIR: {
            int rc = api->mkdir(cmd.src);
            report(api, cmd.job_id, 0, 0, 1, rc ? -1 : 0,
                   rc ? "mkdir failed" : "created", 0);
            break;
        }
        case FW_OP_TOUCH: {
            int rc = api->file_write(cmd.src, "", 0);
            report(api, cmd.job_id, 0, 0, 1, rc ? -1 : 0,
                   rc ? "touch failed" : "created", 0);
            break;
        }
        case FW_OP_FAULT: {
            /* 内核没开分页，物理 0 页是可写的——读 *0 根本不会 #PF，
             * 只会静默写坏 IVT。要用一条真的会陷进内核的指令：ud2 -> #UD。
             * 内核 isr_handler 收到任务里的 CPU 异常后只杀该任务并重启，
             * 主进程（文件管理器）与服务池继续存活。 */
            __asm__ volatile("ud2");
            return;                  /* never reached */
        }
        default:
            break;  /* unknown op: ignore */
        }
    }
}
