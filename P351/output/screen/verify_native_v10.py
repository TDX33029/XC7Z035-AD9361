"""核对原生编辑器保存的 HMI/TFT；不修改二进制、不替代厂家编译。"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

BASE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canon(lines):
    return ''.join(re.sub(r'\s+', '', line) for line in lines
                   if line.strip() and not line.lstrip().startswith('//'))


def saved_events(data):
    result = []
    for match in re.finditer(rb'codes[a-z]+-([0-9]+)', data):
        count, pos, lines = int(match[1]), match.end(), []
        if count > 3000:
            continue
        try:
            for _ in range(count):
                size = struct.unpack_from('<I', data, pos)[0]
                pos += 4
                if size > 65536 or pos+size > len(data):
                    raise ValueError('invalid record')
                lines.append(data[pos:pos+size].decode('gbk'))
                pos += size
            result.append((match.group().split(b'-')[0].decode(), lines))
        except (ValueError, UnicodeError, struct.error):
            continue
    return result


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--observed-tft-bytes', type=int)
    parser.add_argument('--observed-warnings', type=int)
    parser.add_argument('--observed-global-ram', type=int)
    parser.add_argument('--observed-page0-ram', type=int)
    args = parser.parse_args(argv)
    hmi, tft = BASE/'P351_FinalRF_480x272_v10.HMI', BASE/'P351_FinalRF_480x272_v10.tft'
    data = hmi.read_bytes()
    records = saved_events(data)
    audit = {}
    for name, kind in [('shouye.press.txt', 'codesdown'), ('shouye.p1.press.txt', 'codesdown'),
                       ('shouye.t3.press.txt', 'codesdown'), ('shouye.tm0.timer.txt', 'codestimer'),
                       ('shouye.postinit.txt', 'codesloadend')]:
        wanted = (BASE/name).read_text(encoding='utf-8').splitlines()
        passed = any(k == kind and canon(lines) == canon(wanted) for k, lines in records)
        assert passed, ('saved HMI event differs', name)
        audit[name] = dict(passed=passed, sha256=digest(BASE/name), lines=len(wanted))
    for token in (b'int ds=1', b'int a11=0', b'int dsb=32', b'int dsl=10304',
                  b'int dsh=65131', b'int ifvalid=0', b'int pcmpcal=0', b'int soqpcal=0',
                  b'int df=5137000', b'int dpo=60', b'int phalf=0', b'P351Panel16', b'baud=115200'):
        assert token in data, ('missing program initialization/font', token)
    for name in ('rfpmag', 'rfexpect', 'rfmodel', 'rffault', 'rfphase', 'snapshotok', 'crfphase', 'crev'):
        assert ('int '+name+'=0').encode('ascii') in data, ('missing v7 snapshot init', name)
    for line in (BASE/'Program.s.txt').read_text(encoding='utf-8').splitlines():
        if line.strip() and not line.startswith('//'):
            assert line.encode('gbk') in data, ('saved Program.s differs', line)
    assert 1000000 < tft.stat().st_size < 8*1024*1024, 'unexpected TFT size'
    observed = args.observed_tft_bytes is not None
    if observed:
        assert args.observed_tft_bytes == tft.stat().st_size, 'TFT differs from observed native output'
    offline = json.loads((BASE/'verification/offline_tests.json').read_text(encoding='utf-8'))
    assert offline['passed']
    result = dict(passed=True, hardware_tested=False, saved_events_match=True,
                  native_compile_observed=observed, target='TJC4827T143_011R_P20 / 480x272',
                  hmi=dict(bytes=hmi.stat().st_size, sha256=digest(hmi)),
                  tft=dict(bytes=tft.stat().st_size, sha256=digest(tft)), events=audit,
                  program_source_sha256=digest(BASE/'Program.s.txt'),
                  compiler_output_bytes=args.observed_tft_bytes, compiler_warnings=args.observed_warnings,
                  global_ram_bytes=args.observed_global_ram, page0_ram_bytes=args.observed_page0_ram,
                  note='事件由脚本逐项核对；原生编译输出需人工/编辑器实际观察，实屏与射频尚待联调。')
    # 只读复核不能覆盖已观察到的原生编译证据，更不能冒充一次新编译。
    evidence = 'native_build.json' if observed else 'reverification.json'
    (BASE/'verification'/evidence).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    if observed:
        manifest = json.loads((BASE/'control_manifest.json').read_text(encoding='utf-8'))
        manifest.update(native_compiled=True, hmi_or_tft_generated=True,
                        native_build_evidence='verification/native_build.json', hardware_tested=False)
        (BASE/'control_manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    print('PASS saved HMI events + TFT hash/size; native_compile_observed=', observed, '; hardware_tested=False')


if __name__ == '__main__':
    main()
