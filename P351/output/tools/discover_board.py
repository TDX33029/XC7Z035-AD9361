# -*- coding: utf-8 -*-
"""只读发现 P351：打印板卡地址、本机接口、身份与诊断（不改板卡任何配置）。

用法（仓库根目录）：
    python P351/output/tools/discover_board.py
板卡上电后等待约 15 秒再运行；发现失败会列出每个网卡/候选地址的诊断。
"""
import sys
from pathlib import Path

OUTPUT = Path(__file__).resolve().parents[1]
UI = OUTPUT / "ui_source" / "migration" / "p351" / "ui"
sys.path.insert(0, str(UI))

import p351_discovery  # noqa: E402


def main():
    print("本机可探测网卡：")
    try:
        for item in p351_discovery.candidate_interfaces():
            print(f"  {item.name:28s} {item.local_ip}/{item.prefix_length}"
                  f"{'  (Ethernet)' if item.is_ethernet else '  (Wi-Fi)'}")
    except Exception as exc:
        print("  ", exc)
    print("广播 DSCI + CFGQ 身份校验中（6 秒）…")
    try:
        result = p351_discovery.choose_single("", "", timeout=6.0)
    except Exception as exc:
        print(f"未发现：{type(exc).__name__}: {exc}")
        for detail in getattr(exc, "diagnostics", ()):
            print("  -", detail)
        raise SystemExit(1)
    config = result.config
    print(f"已发现：板卡 {result.board_ip} · 本机 {result.local_ip}"
          f" · 接口 {result.interface_name} · RTT {result.rtt_ms:.1f} ms")
    for name in ("board_version", "ver", "tx_mode", "rf_mode"):
        if hasattr(config, name):
            print(f"  {name} = {getattr(config, name)}")
    print("身份校验通过；未发送任何 RF/采集/改址命令。")


if __name__ == "__main__":
    main()
