# TinyOS Genesis v0.1 — tinysh 构建
#
# 默认目标：宿主机后端（kernel_api_host.c），可在普通 PC 上编译运行，
#          用于验证手册里的行为（REPL / 命令 / 错误码）。
#     make            -> 生成 ./tinysh
#     make run        -> 运行（交互）
#     make test       -> 管道跑示例会话（非交互自测）
#
# 产品目标：make tinyos 用 kernel_api_tinyos.c，严格走 tinyos_api_t 函数
#          指针表（.tncr 用户态只能经此表访问内核），并追加 Genesis v0.1
#          扩展字段（fs_*/proc_*/hw_*/系统信息）。需 zig + TinyOS 2.0 用户态
#          stdio shim，并把 TINYOS_KERNEL_ROOT 指向内核 include 目录
#          （同 LUA.TNCR 的编法）。产出 /bin/tinysh.tncr。

ZIG ?= zig
CC  ?= $(ZIG) cc
CFLAGS = -std=c11 -O2 -Wall -Wextra

SRCS_HOST = tinysh.c kernel_api_host.c
SRCS_TINY = tinysh.c kernel_api_tinyos.c

tinysh: $(SRCS_HOST) tinysh.h kernel_api.h
	$(CC) $(CFLAGS) -o $@ $(SRCS_HOST)

test: tinysh
	./tinysh < test_session.txt

run: tinysh
	./tinysh

tinyos: $(SRCS_TINY) tinysh.h kernel_api.h
	$(CC) $(CFLAGS) -DTINYOS_USER -DTINYOS_KERNEL_ROOT=../../../kernel/include \
	      -I../../../kernel/include -o tinysh.tncr $(SRCS_TINY)

clean:
	rm -f tinysh tinysh.tncr

.PHONY: tinysh tinyos run test clean
