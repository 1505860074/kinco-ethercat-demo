#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
send_command.py —— Python 上位机（学习精简版）

对应原项目里的 python 上位机（box_aligner_tool / pyRocky 那一层）。
原项目把命令打包成二进制 struct 通过 socket 发给下位机；
本 demo 精简成发送一行“可读文本”命令，方便你直接看懂、也方便用 nc/telnet 手调。

用法：
    # 让电机相对转 90 度
    python3 send_command.py --host 127.0.0.1 --port 5051 move 90

    # 查询当前电机位置和状态字
    python3 send_command.py --host 127.0.0.1 --port 5051 status

原理：
    socket 就是“网络上的一根管子”。上位机(客户端)用 socket 连上下位机(服务端)的
    IP+端口，把一行文本(末尾加 '\\n')塞进管子发过去；下位机收到后执行，并把应答
    从管子里发回来。整条链路的“入口”就是这里发出去的这一行文本。
"""

import argparse
import socket
import sys


def send_line(host: str, port: int, line: str) -> str:
    """连上下位机，发一行命令，返回下位机的应答文本。"""
    # AF_INET = IPv4，SOCK_STREAM = TCP（可靠、按顺序的字节流）
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(5.0)  # 5 秒超时，连不上/没应答就报错，别死等
        sock.connect((host, port))          # 建立连接
        sock.sendall((line + "\n").encode())  # 发送命令（务必加换行，下位机按行切分）
        data = sock.recv(1024)              # 等待并接收应答
        return data.decode(errors="replace").strip()


def main():
    """命令行入口：解析用户敲的命令行参数，拼出要发的文本命令，交给 send_line 发出去并打印应答。"""
    # 建一个命令行解析器；description 会显示在 --help 顶部，说明这个程序是干嘛的
    parser = argparse.ArgumentParser(description="Kinco EtherCAT demo 上位机")
    # 下面两个是“全局可选参数”（带 -- 前缀，不填就用默认值），指定要连的下位机地址
    parser.add_argument("--host", default="127.0.0.1", help="下位机 IP（默认本机）")
    parser.add_argument("--port", type=int, default=5051, help="下位机端口（默认 5051）")  # type=int：自动把字符串转成整数

    # 注册“子命令”机制：让本程序支持 move / status 两种动作，像 git commit / git push 那样
    # dest="cmd"：用户选的子命令名会存进 args.cmd，下面靠它分流；required=True：必须选一个，不选就报错
    sub = parser.add_subparsers(dest="cmd", required=True)

    # 子命令一：move —— 需要再跟一个位置参数 angle（转多少度）
    p_move = sub.add_parser("move", help="相对转动一个角度")
    p_move.add_argument("angle", type=float, help="角度（度），如 90")  # type=float：允许 90、90.5 这类小数

    # 子命令二：status —— 纯查询，不需要额外参数，所以拿到 parser 后不加任何参数
    sub.add_parser("status", help="查询当前位置/状态字")

    # 真正开始解析用户在命令行敲的内容，结果放进 args（含 host/port/cmd/angle 等字段）
    args = parser.parse_args()

    # 分支：根据用户选的子命令，拼出要发给下位机的那一行文本命令
    if args.cmd == "move":
        # move 分支：拼成 "MOVE 90" 这样，下位机按空格切分后取角度
        line = f"MOVE {args.angle}"
    elif args.cmd == "status":
        # status 分支：固定就是一个 "STATUS"，无参数
        line = "STATUS"
    else:
        # 兜底分支：理论上进不来（required=True 已挡住空命令），留作防御，打印帮助后退出
        parser.print_help()
        sys.exit(1)

    # 发送前先回显一下要发什么、发到哪，方便调试；{line!r} 用 repr 显示，能看清引号和换行等
    print(f"[上位机] 发送: {line!r}  →  {args.host}:{args.port}")
    # 把命令通过 socket 发出去；网络类操作可能失败，用 try 包住统一处理
    try:
        reply = send_line(args.host, args.port, line)  # 阻塞等待下位机应答，拿到应答文本
    # 兜底：连接被拒 / 超时 / 其它系统级网络错误，都在这里统一捕获，给出可读的排错提示
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        print(f"[上位机] 连接/通信失败: {e}", file=sys.stderr)  # 错误信息走 stderr，和正常输出区分开
        print("  提示: 确认下位机 (controller) 已启动、IP/端口正确、网络可达。",
              file=sys.stderr)
        sys.exit(2)  # 以非 0 码退出，方便脚本/shell 判断这次调用失败了
    # 一切正常：打印下位机返回的应答
    print(f"[上位机] 收到应答: {reply}")


if __name__ == "__main__":
    main()
