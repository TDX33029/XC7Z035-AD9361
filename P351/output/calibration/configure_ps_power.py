"""P351 功率配置向导：校验后同步两份 PS 标定头文件。

默认仅预览；--apply 才写文件并备份。无网络、串口、JTAG、烧录或 RF 操作。
核心计算复用 calibrate_up70.py，与生产 p351_up70_calibration.h 对应。
"""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

import calibrate_up70 as core

BASE = Path(__file__).resolve().parent
TARGETS = (
    'ps/src/p351_up70_calibration_data.h',
    'vitis_offline/P351_APP/src/p351_up70_calibration_data.h',
)
SHANGHAI = timezone(timedelta(hours=8), 'Asia/Shanghai')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def config_from_values(zero, soq_zero=None, *, net_gain=None, amplifier_gain=None, path_loss=None):
    """快速输入一律标为模型；不凭几个数字冒充整模式实测标定表。"""
    if zero is None:
        raise ValueError('请填写 TRX2 在70 MHz、AD9361零衰减时的实测平均功率(dBm)')
    if net_gain is not None:
        if amplifier_gain is not None or path_loss is not None:
            raise ValueError('整链净增益与功放增益/路径损耗不能同时填写')
        gain = {'net_gain_db': net_gain}
    else:
        if amplifier_gain is None or path_loss is None:
            raise ValueError('只有功放参数时，必须同时填增益(dB)和路径总损耗(dB)；未知损耗不能自动当0')
        gain = {'amplifier_gain_db': amplifier_gain, 'path_loss_db': path_loss}
    # 不允许理想零损耗的隐式回退。没有单独SOQPSK数据时显式使用共同模型。
    if soq_zero is None:
        return {'schema': 1, 'quick': {'p351_zero_dbm': zero, **gain}}
    return {
        'schema': 1,
        'if_profiles': [dict(mode=mode, rate_min_hz=100000, rate_max_hz=20000000,
                             zero_dbm=value, measured=False)
                        for mode, value in [('PCM/FM', zero), ('SOQPSK', soq_zero)]],
        'rf_profiles': [dict(mode=mode, freq_min_khz=low, freq_max_khz=high,
                             rate_min_hz=100000, rate_max_hz=20000000,
                             measured=False, **gain)
                        for mode in ('PCM/FM', 'SOQPSK') for low, high in core.BANDS],
    }


def interactive_values():
    print('P351 功率标定配置（仅改PS文件，不连接硬件）')
    print('测量条件：TRX2、70 MHz、幅度26214、模拟带宽56 MHz；平均功率，补偿测量线缆/外部衰减。')
    zero = input('PCM/FM：AD9361衰减0 dB时，TRX2实测平均功率[dBm]：').strip()
    soq = input('SOQPSK：对应实测平均功率[dBm]（回车暂用相同模型）：').strip()
    choice = input('后级填写方式：1=整链净增益；2=功放增益+其他路径总损耗 [默认1]：').strip() or '1'
    if choice == '1':
        gain = input('70 MHz输入到最终RF输出的整链净增益[dB]（已含功放与全部插损）：').strip()
        return config_from_values(zero, soq or None, net_gain=gain)
    if choice == '2':
        amp = input('功放增益[dB]（不是额定输出瓦数/饱和输出dBm）：').strip()
        loss = input('其余路径总损耗[dB]（混频/滤波/分路/线缆；确知无损耗才填0）：').strip()
        return config_from_values(zero, soq or None, amplifier_gain=amp, path_loss=loss)
    raise ValueError('填写方式只能选择1或2')


def target_paths(root):
    root = root.resolve(strict=True)
    paths = []
    for relative in TARGETS:
        path = (root/relative).resolve(strict=True)
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError('目标文件越出所选P351工程或不是普通文件：'+relative)
        if b'P351_UP70_CALIBRATION_DATA_H' not in path.read_bytes():
            raise ValueError('目标不是预期的标定头文件：'+relative)
        paths.append(path)
    if paths[0] == paths[1]:
        raise ValueError('两份标定头文件不能指向同一个文件')
    return root, paths


def stage(path, data):
    """同目录临时文件，保持替换在同一文件系统；只清理本工具新建的临时文件。"""
    f = tempfile.NamedTemporaryFile(mode='wb', prefix=path.name+'.power_', suffix='.tmp',
                                    dir=path.parent, delete=False)
    pending = Path(f.name)
    try:
        with f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
    except BaseException:
        pending.unlink(missing_ok=True)
        raise
    return pending


def apply_both(root, paths, content, raw, source_hash):
    """先备份和暂存两份文件，再替换。第二份失败时回滚第一份。

    无法跨两个文件提供操作系统级原子事务；并发改动时拒绝覆盖他人数据，保留备份。
    因此请先在Vitis保存/关闭此头文件，不要在脚本运行时同时Build或编辑。
    """
    before = [p.read_bytes() for p in paths]
    if before[0] != before[1]:
        raise ValueError('规范PS与Vitis标定头文件当前不一致，已拒绝覆盖；请先核实/同步两份原标定')
    new = content.encode('utf-8')
    if all(b == new for b in before):
        return dict(changed=False, backup_directory=None, files=[str(p) for p in paths])
    stamp = datetime.now(SHANGHAI).strftime('%Y%m%d_%H%M%S_%f')
    backup = root/'logs/power_calibration_updates'/stamp
    backup.mkdir(parents=True, exist_ok=False)
    state = dict(status='prepared', source_sha256=source_hash,
                 timezone='Asia/Shanghai', files=[])
    for i, (p, data) in enumerate(zip(paths, before)):
        name = 'canonical_before.h' if i == 0 else 'vitis_before.h'
        (backup/name).write_bytes(data)
        state['files'].append(dict(path=str(p), backup=name, before_sha256=digest(data),
                                   after_sha256=digest(new)))
    (backup/'input_config.json').write_text(json.dumps(raw, ensure_ascii=False, indent=2, default=str), encoding='utf-8')
    journal = backup/'transaction.json'
    journal.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
    pending, restore, changed = [], [], []
    try:
        # 暂存新/旧内容，失败时不需要删除或移动任一用户源目录。
        for p, old in zip(paths, before):
            pending.append(stage(p, new))
            restore.append(stage(p, old))
        if [p.read_bytes() for p in paths] != before:
            raise RuntimeError('暂存期间源文件被其他进程修改，拒绝写入')
        for i, p in enumerate(paths):
            if p.read_bytes() != before[i]:
                raise RuntimeError('替换前检测到并发编辑，拒绝覆盖：'+str(p))
            # 先记录尝试，再替换；Ctrl+C恰好发生在replace返回前也能检查并回滚。
            changed.append(i)
            os.replace(pending[i], p)
        if any(p.read_bytes() != new for p in paths):
            raise RuntimeError('替换后的双工程读回校验失败')
        state['status'] = 'applied'
        journal.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
    except BaseException as error:
        failures = []
        for i in reversed(changed):
            try:
                current = paths[i].read_bytes()
                if current == before[i]:
                    continue  # 此次replace尚未生效，不触碰原文件。
                if current != new:
                    raise RuntimeError('已被其他进程更改，保留其内容，不自动回滚')
                os.replace(restore[i], paths[i])
                if paths[i].read_bytes() != before[i]:
                    raise RuntimeError('回滚读回校验失败')
            except Exception as rollback:
                failures.append(str(paths[i])+': '+str(rollback))
        state.update(status='rollback_incomplete' if failures else 'rolled_back',
                     error=str(error), rollback_errors=failures)
        journal.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
        raise RuntimeError(f'同步失败；{state["status"]}。原文件备份：{backup}；原因：{error}') from error
    finally:
        for p in pending+restore:
            p.unlink(missing_ok=True)
    return dict(changed=True, backup_directory=str(backup), files=[str(p) for p in paths])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--interactive', action='store_true')
    parser.add_argument('--config', type=Path, help='沿用原calibrate_up70.py的JSON格式，支持实测分表')
    parser.add_argument('--p351-zero-dbm', help='TRX2 70MHz、0dB衰减实测功率；共同模型或PCM/FM值')
    parser.add_argument('--soqpsk-zero-dbm', help='可选：SOQPSK单独的零衰减实测功率')
    parser.add_argument('--net-gain-db', help='后级整链净增益，dB')
    parser.add_argument('--amplifier-gain-db', help='功放增益，dB，必须配合--path-loss-db')
    parser.add_argument('--path-loss-db', help='其余路径总损耗，dB，非负')
    parser.add_argument('--frequency-mhz', default='5137', help='仅用于预览最终输出功率预算')
    parser.add_argument('--target-dbm', default='-30', help='仅用于预览最终输出目标，不改屏幕草稿')
    parser.add_argument('--rate-hz', type=int, default=4000000)
    parser.add_argument('--apply', action='store_true', help='备份后同步两份PS标定头文件')
    parser.add_argument('--project-dir', type=Path, default=BASE, help='P351工程根目录，默认脚本所在目录')
    args = parser.parse_args(argv)
    try:
        direct = any(v is not None for v in (args.p351_zero_dbm, args.soqpsk_zero_dbm,
                     args.net_gain_db, args.amplifier_gain_db, args.path_loss_db))
        if sum((bool(args.config), args.interactive, direct)) != 1:
            raise ValueError('必须且只能选择一种输入：--interactive、--config，或命令行数值')
        if args.config:
            raw = json.loads(args.config.read_text(encoding='utf-8-sig'), parse_float=Decimal)
        elif args.interactive:
            raw = interactive_values()
        else:
            raw = config_from_values(args.p351_zero_dbm, args.soqpsk_zero_dbm,
                                     net_gain=args.net_gain_db, amplifier_gain=args.amplifier_gain_db,
                                     path_loss=args.path_loss_db)
        cfg = core.validate(raw)  # 不提供--ideal-model，未知损耗不得默认为零。
        freq = core.number(args.frequency_mhz, 'frequency_mhz', lo=5091000, hi=6125000)
        target = core.number(args.target_dbm, 'target_dbm', lo=-40000, hi=-10000)
        if freq % 100 or not any(a <= freq <= b for a,b in core.BANDS):
            raise ValueError('预览频率不在允许的两频段/100kHz步进内')
        if target % 500 or not 100000 <= args.rate_hz <= 20000000:
            raise ValueError('预览功率须0.5dB步进，码率须100k..20M')
        root, paths = target_paths(args.project_dir)
        canonical = json.dumps(raw, ensure_ascii=False, sort_keys=True, default=str).encode('utf-8')
        source_hash = digest(canonical)
        content = core.render(cfg, source_hash)
        preview = {mode:core.plan(cfg, value, freq, args.rate_hz, target) for mode,value in [('PCM/FM',0),('SOQPSK',1)]}
        report = dict(applied=False, changed=False, files=[str(p) for p in paths],
                      source_sha256=source_hash, if_records=len(cfg.if_rows), rf_records=len(cfg.rf_rows),
                      fixed_if_mhz=70, fixed_if_target_dbm=-30,
                      preview_frequency_mhz=args.frequency_mhz, preview_target_dbm=args.target_dbm,
                      rate_hz=args.rate_hz, preview=preview, notices=list(cfg.notices),
                      hardware_accessed=False, rf_qualification_changed=False,
                      auto_tx_changed=False, pc_ui_changed=False, built_or_flashed=False)
        print('\n计算预览（不是在线功率测量）：')
        for mode, p in preview.items():
            if p is None:
                print(f'  {mode}: 此预览目标不可达或未被表覆盖；PS会拒绝此目标，不会静默限幅。')
            else:
                print(f'  {mode}: AD9361衰减 {p["if_atten_mdb"]/1000:g} dB，'
                      f'预测TRX2 {p["predicted_if_mdbm"]/1000:g} dBm；'
                      f'后级衰减 {p["ext_half_db"]/2:g} dB，'
                      f'预测最终输出 {p["predicted_final_mdbm"]/1000:g} dBm。')
        print('快速输入只生成统一模型，不是全码率/全频段实测标定；硬件资格、锁定保护不变。')
        print('将同步：\n  '+'\n  '.join(str(p) for p in paths))
        if args.apply:
            if args.interactive and input('确认写入这两份PS工程？输入 YES 继续，其余取消：').strip() != 'YES':
                print('已取消，未修改PS文件。')
                return 0
            report.update(apply_both(root, paths, content, raw, source_hash), applied=True)
            print('已同步。请在Vitis刷新工程并Build；旧ELF/BOOT不会自动更新。')
        else:
            print('仅预览，未写文件。确认数值后在同一命令末尾加 --apply。')
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, OSError, RuntimeError, EOFError) as error:
        print('配置拒绝：'+str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
