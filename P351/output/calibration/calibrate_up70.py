"""Offline P351 + two-conversion upconverter power configuration.

Default is dry-run. --apply changes only generated calibration_data.h, after
validation and a recoverable backup. No board/network/serial access, no RF
qualification override, no BOOT enable and no automatic build/flash.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
from decimal import Decimal, DecimalException
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
from datetime import datetime, timezone

BASE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = BASE / "ps/src/p351_up70_calibration_data.h"
BANDS = ((5091000, 5150000), (5925000, 6125000))
MODE = {"PCM/FM": 0, "SOQPSK": 1, "SOQPSK-TG": 1}


def number(value, name, lo=-100000, hi=100000):
    if isinstance(value, bool) or value is None:
        raise ValueError(f"{name}: 必须填写数值，不接受空值/布尔值")
    try:
        value = Decimal(str(value))
        if not value.is_finite():
            raise ValueError(f"{name}: 必须是有限数")
        scaled = value * 1000
        if scaled != scaled.to_integral_value() or not lo <= scaled <= hi:
            raise ValueError(f"{name}: 超范围或多于3位小数")
    except DecimalException as exc:
        raise ValueError(f"{name}: 不是有效数值或数值过大") from exc
    return int(scaled)


def integer(value, name, lo, hi):
    if isinstance(value, bool) or not isinstance(value, int) or not lo <= value <= hi:
        raise ValueError(f"{name}: 必须是{lo}..{hi}的整数")
    return value


def measured(record):
    flag = record.get("measured", False)
    if type(flag) is not bool:
        raise ValueError("measured必须为true/false")
    if flag and not str(record.get("evidence", "")).strip():
        raise ValueError("实测表必须填写evidence：端口、负载、幅度、带宽、仪器及测试范围")
    return int(flag)


def mode_of(record):
    value = record.get("mode")
    if value not in MODE:
        raise ValueError("mode仅支持PCM/FM、SOQPSK或SOQPSK-TG")
    return MODE[value]


def rates(record):
    low = integer(record.get("rate_min_hz", 100000), "rate_min_hz", 100000, 20000000)
    high = integer(record.get("rate_max_hz", 20000000), "rate_max_hz", 100000, 20000000)
    if low > high:
        raise ValueError("码率范围倒置")
    return low, high


def net_gain(record, ideal=False):
    if "net_gain_db" in record:
        if "amplifier_gain_db" in record or "path_loss_db" in record:
            raise ValueError("净增益和功放/损耗写法不可混用")
        return number(record["net_gain_db"], "net_gain_db"), "净增益模型"
    gain = number(record.get("amplifier_gain_db"), "amplifier_gain_db")
    if "path_loss_db" not in record:
        if not ideal:
            raise ValueError("单独功放增益不等于整机净增益：必须填写path_loss_db，或显式--ideal-model仅作理想估算")
        loss = 0
        label = "理想模型：未计混频器/滤波器/分路器/线缆损耗"
    else:
        loss = number(record["path_loss_db"], "path_loss_db", lo=0, hi=100000)
        label = "功放增益减已填链路总损耗模型"
    result = gain - loss
    if not -100000 <= result <= 100000:
        raise ValueError("整机净增益超出-100..100dB")
    return result, label


def no_overlap(rows, if_rows=False):
    for i, a in enumerate(rows):
        for b in rows[:i]:
            rate_a, rate_b = ((a[1], a[2]), (b[1], b[2])) if if_rows else ((a[3], a[4]), (b[3], b[4]))
            if a[0] != b[0] or max(rate_a[0], rate_b[0]) > min(rate_a[1], rate_b[1]):
                continue
            if if_rows or max(a[1], b[1]) <= min(a[2], b[2]):
                raise ValueError("存在同模式、频率/码率范围重叠记录，拒绝任意优先匹配")


@dataclass(frozen=True)
class Config:
    if_rows: tuple
    rf_rows: tuple
    notices: tuple


def validate(config, ideal=False):
    if not isinstance(config, dict):
        raise ValueError("JSON顶层必须是对象")
    if config.get("schema") != 1:
        raise ValueError("schema必须为1")
    if "quick" in config:
        if "if_profiles" in config or "rf_profiles" in config:
            raise ValueError("quick不能与分模式/分频点表混用")
        quick = config["quick"]
        if not isinstance(quick, dict):
            raise ValueError("quick必须是对象")
        zero = number(quick.get("p351_zero_dbm"), "p351_zero_dbm")
        gain, label = net_gain(quick, ideal)
        if_rows = [(m, 100000, 20000000, zero, 0) for m in (0, 1)]
        rf_rows = [(m, low, high, 100000, 20000000, gain, 0) for m in (0, 1) for low, high in BANDS]
        notices = ["两数快速配置是模型，不是全模式/全频段整机实测标定；所有预测值均非在线测量。", label,
                   "同一P351功率值同时用于PCM/FM和SOQPSK；真实模式、码率差异需用实测表补充。"]
    else:
        ir, rr = config.get("if_profiles", []), config.get("rf_profiles", [])
        if not isinstance(ir, list) or not isinstance(rr, list) or not 1 <= len(ir) <= 32 or not 1 <= len(rr) <= 64:
            raise ValueError("if_profiles需1..32条，rf_profiles需1..64条")
        if any(not isinstance(r, dict) for r in ir+rr):
            raise ValueError("每条实测表记录必须是对象")
        if_rows = [(mode_of(r), *rates(r), number(r.get("zero_dbm"), "zero_dbm"), measured(r)) for r in ir]
        rf_rows = []
        for r in rr:
            low = integer(r.get("freq_min_khz"), "freq_min_khz", 5091000, 6125000)
            high = integer(r.get("freq_max_khz"), "freq_max_khz", 5091000, 6125000)
            if low % 100 or high % 100 or not any(a <= low <= high <= b for a, b in BANDS):
                raise ValueError("频段必须完整落在5091..5150或5925..6125MHz且为100kHz步进")
            gain, _ = net_gain(r, ideal)
            flag = measured(r)
            if flag and "net_gain_db" not in r:
                raise ValueError("整机实测表必须填实测net_gain_db；单独功放增益加损耗仍是模型")
            rf_rows.append((mode_of(r), low, high, *rates(r), gain, flag))
        notices = ["实测标志表示用户提交的离线测量记录，不是功率计在线回读；仅在所填模式/频率/码率范围有效。"]
    no_overlap(if_rows, True)
    no_overlap(rf_rows)
    # Any IF profile unable to generate <=-30dBm is a configuration error.
    for _, _, _, zero, _ in if_rows:
        if not 0 <= zero + 30000 <= 89750:
            raise ValueError("P351的-30dBm目标不可达：所需AD9361衰减不在0..89.75dB")
    return Config(tuple(if_rows), tuple(rf_rows), tuple(notices))


def plan(config, mode, freq, rate, target):
    if mode not in (0, 1) or not 100000 <= rate <= 20000000 or not any(a <= freq <= b for a, b in BANDS) or freq % 100:
        return None
    if not -40000 <= target <= -10000 or target % 500:
        return None
    candidates = [r for r in config.if_rows if r[0] == mode and r[1] <= rate <= r[2]]
    rf = [r for r in config.rf_rows if r[0] == mode and r[1] <= freq <= r[2] and r[3] <= rate <= r[4]]
    if len(candidates) != 1 or len(rf) != 1:
        return None
    ifr, rfr = candidates[0], rf[0]
    need = ifr[3] + 30000
    if not 0 <= need <= 89750:
        return None
    ia = ((need + 249) // 250) * 250
    ip = ifr[3] - ia
    need_ext = ip + rfr[5] - target
    if not 0 <= need_ext <= 63000:
        return None
    half = (need_ext + 499) // 500
    return dict(if_atten_mdb=ia, predicted_if_mdbm=ip, ext_half_db=half,
                predicted_final_mdbm=ip + rfr[5] - half * 500,
                confidence=2 if ifr[4] and rfr[6] else 1,
                measured=int(bool(ifr[4] and rfr[6])))


def render(config, source_hash):
    def rows(values):
        return ",\n".join("    {" + ",".join(str(v) + ("U" if v >= 0 else "") for v in row) + "}" for row in values)
    return f'''#ifndef P351_UP70_CALIBRATION_DATA_H
#define P351_UP70_CALIBRATION_DATA_H
/* Generated by calibrate_up70.py. Source SHA256: {source_hash}
 * confidence=1: MODEL, not full-system measured power calibration.
 * Does not qualify hardware, enable automatic TX, flash or access a board.
 */
#define P351_UP70_IF_RECORD_COUNT {len(config.if_rows)}U
#define P351_UP70_RF_RECORD_COUNT {len(config.rf_rows)}U
static const p351_up70_if_record_t p351_up70_if_records[{len(config.if_rows)}]={{
{rows(config.if_rows)}
}};
static const p351_up70_rf_record_t p351_up70_rf_records[{len(config.rf_rows)}]={{
{rows(config.rf_rows)}
}};
#endif
'''


def atomic_apply(path, content):
    """Write only after validation; backup prior bytes, atomic same-directory swap.

    Refuse concurrent edits between backup and swap; leave backup recoverable.
    Existing filesystem ACLs are inherited by the temp file in the same folder.
    """
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    before = path.read_bytes() if path.exists() else None
    backup = None
    if before is not None:
        suffix = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        backup = path.with_name(path.name + "." + suffix + ".bak")
        with backup.open("xb") as f:
            f.write(before)
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", prefix=path.name + ".", suffix=".tmp", dir=path.parent, delete=False) as f:
            tmp = Path(f.name)
            f.write(content.encode("utf-8"))
            f.flush()
            os.fsync(f.fileno())
        current = path.read_bytes() if path.exists() else None
        if current != before:
            raise RuntimeError("目标文件已被其他进程更改，拒绝覆盖")
        os.replace(tmp, path)
        tmp = None
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)
    return str(backup) if backup else None


def main(argv=None):
    parser = argparse.ArgumentParser(description="P351+上变频器离线功率配置；默认只预览")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--p351-zero-dbm")
    parser.add_argument("--net-gain-db")
    parser.add_argument("--amplifier-gain-db")
    parser.add_argument("--path-loss-db")
    parser.add_argument("--ideal-model", action="store_true", help="显式允许未知链路损耗的理想模型，绝非实测标定")
    parser.add_argument("--interactive", action="store_true", help="只输入P351零衰减功率与上变频器整机净增益")
    parser.add_argument("--frequency-mhz", default="5137")
    parser.add_argument("--target-dbm", default="-30")
    parser.add_argument("--rate-hz", type=int, default=4000000)
    parser.add_argument("--mode", choices=("PCM/FM", "SOQPSK"), default="PCM/FM")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        direct = any(v is not None for v in (args.p351_zero_dbm, args.net_gain_db, args.amplifier_gain_db, args.path_loss_db))
        if args.config and (direct or args.interactive) or args.interactive and direct:
            raise ValueError("--config、命令行数值、--interactive三种输入互斥")
        if args.config:
            raw = json.loads(args.config.read_text(encoding="utf-8-sig"), parse_float=Decimal)
        else:
            quick = {"p351_zero_dbm": args.p351_zero_dbm}
            if args.interactive:
                quick["p351_zero_dbm"] = input("P351 TRX2在70MHz、0dB衰减的实测平均功率(dBm)：")
                quick["net_gain_db"] = input("上变频器整条链路净增益(dB，含混频/滤波/分路/线缆损耗；不是单级功放增益)：")
            else:
                for key in ("net_gain_db", "amplifier_gain_db", "path_loss_db"):
                    value = getattr(args, key)
                    if value is not None:
                        quick[key] = value
            raw = {"schema": 1, "quick": quick}
        if not isinstance(raw, dict):
            raise ValueError("JSON顶层必须是对象")
        config = validate(raw, args.ideal_model)
        freq = number(args.frequency_mhz, "frequency_mhz", lo=5091000, hi=6125000)
        target = number(args.target_dbm, "target_dbm", lo=-40000, hi=-10000)
        result = plan(config, MODE[args.mode], freq, args.rate_hz, target)
        if result is None:
            raise ValueError("预览目标频率/功率/码率未覆盖，或目标功率不可达；未修改文件")
        canonical = json.dumps(raw, ensure_ascii=False, sort_keys=True, default=str).encode("utf-8")
        source_hash = hashlib.sha256(canonical).hexdigest()
        report = dict(applied=False, model_or_offline_measured=True, hardware_qualified_unchanged=True,
            boot_enabled_unchanged=True, notices=list(config.notices), source_sha256=source_hash,
            preview=dict(mode=args.mode, frequency_khz=freq, rate_hz=args.rate_hz,
                         target_mdbm=target, **result),
            output=str(args.output.resolve()), if_records=len(config.if_rows), rf_records=len(config.rf_rows))
        if args.apply:
            report["backup"] = atomic_apply(args.output, render(config, source_hash))
            report["applied"] = True
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, OSError, RuntimeError, EOFError) as error:
        print(f"配置拒绝：{error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
