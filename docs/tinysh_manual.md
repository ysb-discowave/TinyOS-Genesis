# tinysh 使用手册（TinyOS Genesis v0.1）

_本手册由修订版 docx 提取，与《TinyOS Genesis v0.1 发行说明》一致。_

tinysh — TinyOS Shell 命令行交互终端 完整手册
tinysh — TinyOS Genesis v0.1 Shell 完整使用手册
文档类型：开发手册（用于编码实现tinysh）
版本：v0.1
项目：TinyOS Genesis
作者：TinyOS开发团队
更新日期：2026

tinysh — TinyOS Shell 命令行交互终端 完整手册
目录
基础概述
语法与交互规则
启动、退出与快捷键
内置命令参考大全
系统错误码规范
tinysh 程序运行逻辑（REPL主循环）
内核API对接约定
限制与未来版本扩展规划
完整交互式会话示例
开发备注

1. 基础概述
1.1 简介
tinysh 是 TinyOS Genesis v0.1 内置交互式命令解释器（Shell终端），作为用户与操作系统内核交互的入口程序。
设计目标：轻量、精简、易于编码实现，适配嵌入式内核，只保留最核心基础功能。
特性清单
单行命令解析，空格分割参数；
内置命令执行，统一返回执行状态码；
命令历史记录（最多保存32条）；
Tab按键命令名自动补全；
大小写敏感；
v0.1版本不支持管道|、输出重定向>、后台任务&；
注释支持，行首#代表注释，整行忽略不执行。
1.2 提示符约定
tinysh> 为标准命令提示符。
tinysh>
2. 语法与交互规则
命令基础格式：
command [arg1] [arg2] [arg3] ...
<> 代表必填参数；
[] 代表可选参数；
| 代表多选。
参数分割：以空格切分参数。v0.1不支持引号包裹带空格字符串。
注释规则：行首#，该行所有文本忽略。
Bash# 这是一行注释，不会执行tinysh> # ls /dev
历史记录：上下方向键翻阅历史输入，上限32条。
Tab自动补全：输入部分命令名，按下Tab，自动匹配补全命令。
3. 启动、退出与快捷键
3.1 程序启动
TinyOS内核初始化完成后，自动加载并启动tinysh。
启动输出：
TinyOS Genesis v0.1tinysh>
3.2 退出命令
命令：exit
Bashtinysh> exit
功能：退出tinysh会话，释放tinysh占用资源，返回内核。
3.3 预留快捷键
Ctrl + C：终止当前前台正在运行的命令，立刻返回 tinysh> 提示符。
返回码通用约定：0 = 执行成功；非0 = 执行异常错误码。
4. 内置命令参考大全
4.1 系统信息类
help — 查看命令帮助
Bashhelp [command]
help：列出系统全部可用命令简短列表
help <command>：查看指定命令详细说明
示例：
Bashtinysh> helptinysh> help ls
version — 查看系统版本
Bashversion
输出：TinyOS版本号、编译时间、tinysh终端版本。
sysinfo — 系统状态概览
Bashsysinfo
输出内容：CPU型号、总内存、空闲内存、系统运行时长、当前进程总数。
date — 获取系统时间戳
Bashdate
读取内核系统时间戳并打印，v0.1仅支持读取，不能修改时间。

4.2 文件系统操作类
ls — 列出目录内容
Bashls [path]
不带参数：列出当前工作目录
带路径参数：ls /dev，列出指定目录
输出字段：文件名、文件类型（目录/普通文件）、文件字节大小。
cd — 切换工作目录
Bashcd <target_path>
示例
Bashcd /cd /bincd ..
限制：v0.1不支持cd ~家目录语法。
pwd — 打印当前工作目录
Bashpwd
输出当前目录绝对路径。
cat — 读取文本文件并输出内容
Bashcat <filepath>
读取指定文本文件，将文件内容打印到终端。
mkdir — 创建单层目录
Bashmkdir <dir_name>
在当前目录新建文件夹。v0.1仅支持单层创建，不支持递归创建（无mkdir -p）。
rm — 删除文件
Bashrm <filepath>
⚠️ v0.1安全限制：不能删除目录，仅可删除普通文件，防止误操作。
touch — 创建空文件
Bashtouch <filename>
在当前目录新建空白文件。

4.3 进程管理命令
ps — 查看进程列表
Bashps
输出全部运行进程信息：PID、进程名称、进程状态、内存占用大小。
kill — 终止指定进程
Bashkill <pid>
向对应PID发送终止信号。
安全规则：内核基础守护进程不允许kill，内核直接返回权限不足错误。

4.4 硬件调试专属命令（TinyOS特色）
devlist — 枚举硬件设备
Bashdevlist
遍历系统挂载硬件，输出设备路径，格式如 /dev/uart、/dev/gpio。
readdev — 读取硬件寄存器
Bashreaddev <devpath> <addr>
读取指定硬件设备对应地址寄存器数值，调用底层内核硬件接口。
writedev — 写入硬件寄存器
Bashwritedev <devpath> <addr> <value>
⚠️ 高危调试命令，错误参数可能造成硬件异常，普通用户谨慎使用。

4.5 基础工具命令
echo — 文本输出
Bashecho <text>
把输入文本直接打印输出至终端
示例：
Bashtinysh> echo hello tinyos
clear — 清屏
Bashclear
清空终端屏幕内容，保留提示符tinysh>。
5. 系统错误码规范
返回码
含义说明
0
OK，命令执行成功
1
未知命令，没有找到该内置命令
2
参数数量错误，过多或者缺少必填参数
3
文件/路径不存在
4
权限不足，禁止操作
5
无效参数，参数格式错误
6
IO读写失败
7
进程PID不存在
编码时，所有命令处理函数统一返回int类型的错误码。
6. tinysh 程序运行逻辑（REPL主循环）
tinysh 核心是REPL（读取-解析-执行-循环），编码主流程：
初始化阶段
分配命令历史记录缓冲区，最多32条；
设置初始工作目录为根目录 /；
初始化命令注册表，注册全部内置命令。
REPL循环主体（无限循环，直到收到exit）
打印提示符 tinysh>
读取一行用户输入字符串
去除首尾空白字符；如果是空行，直接回到循环开头；
判断行首#，是注释则跳过，回到循环；
分割字符串，切割成【命令 + 参数数组】
在命令注册表查找，匹配对应命令处理函数
调用命令函数，传入参数列表，接收返回错误码
根据错误码，输出对应的错误提示信息
回到步骤1，等待用户下一行输入
收到exit命令：退出REPL循环，释放tinysh申请的内存资源，返回内核。
7. 内核API对接约定
tinysh不实现底层硬件、文件、进程逻辑，tinysh只做命令解析，所有底层操作调用内核API。
| tinysh功能模块 | 调用内核API组 |
| ---- | ---- |
| ls cd pwd cat mkdir rm touch | fs_api 文件系统接口 |
| ps kill | proc_api 进程管理接口 |
| devlist readdev writedev | hw_api 硬件寄存器访问接口 |
8. 限制与未来版本扩展规划（v0.1暂不实现，预留接口）
不支持通配符 *；
不支持引号包裹带空格字符串；
无管道|、输出重定向>；
v0.1不支持执行外部脚本文件；
暂不实现环境变量 $PATH、命令别名alias、后台任务。
v0.2版本规划增加：脚本执行、环境变量、通配符。
9. 完整交互式会话示例（测试用例）
TinyOS Genesis v0.1tinysh> help可用命令: help version sysinfo date ls cd pwd cat mkdir rm touch ps kill devlist readdev writedev echo clear exittinysh> pwd/tinysh> cd /bintinysh> pwd/bintinysh> lstinysh tncr tcr ppgtinysh> sysinfoCPU: TinyRISCMem total: 256KB, Free: 142KBUptime: 12sProcess count:4tinysh> echo TinyOS GenesisTinyOS Genesistinysh> exittinysh terminated
10. 开发备注
命令表使用结构体数组注册，方便新增命令；
所有字符串缓冲区增加长度限制，防止缓冲区溢出；
Tab补全仅匹配命令名，暂不实现文件路径补全；
硬件调试命令 readdev / writedev 增加权限校验。
