#!/usr/bin/env python3
"""Summarize one SD log run on the development computer, without device access.

Only metadata hints are classified. Nothing is called supported, usable or
passed based on file names, library strings or USB enumeration alone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

LIMIT = 32 * 1024 * 1024
SECTIONS = re.compile(r'^=== (.*?) ===$')
NEGATIVE = re.compile(r'unknown:|not found|no such|unavailable|incomplete|failed|not supported|permission denied|cannot open|cannot access|\berror:', re.I)


def read_log(path):
    if not path.exists():
        return None
    if path.is_symlink() or not path.is_file() or path.stat().st_size > LIMIT:
        raise ValueError('log must be a regular file no larger than 32 MiB: ' + str(path))
    raw = path.read_bytes()
    records = []
    section = 'preamble'
    sample = None
    for number, line in enumerate(raw.decode('utf-8', errors='replace').splitlines(), 1):
        match = SECTIONS.match(line.strip())
        if match:
            heading = match.group(1)
            if re.fullmatch(r'sample \d+', heading):
                sample = int(heading.split()[1])
                section = 'preamble'
            else:
                section = heading
            continue
        records.append({'line': number, 'section': section, 'sample': sample, 'text': line})
    return {'name': path.name, 'sha256': hashlib.sha256(raw).hexdigest(),
            'bytes': len(raw), 'records': records}


def evidence(logs, names, pattern):
    found = []
    regex = re.compile(pattern, re.I)
    for log in logs:
        if log is None:
            continue
        for item in log['records']:
            line = item['text'].strip()
            if item['section'] not in names or NEGATIVE.search(line):
                continue
            # A candidate path printed before ls is not proof of its existence.
            if line.startswith(('binary=', 'config=', 'resolved_config=', 'directory=')):
                continue
            if regex.search(line):
                # Keep references, not copies of addresses, command arguments,
                # identity strings or phone serial numbers in the summary.
                found.append({'log': log['name'], 'line': item['line'],
                              'section': item['section'], 'sample': item['sample']})
                if len(found) >= 12:
                    return found
    return found


def condition(key, title, refs, remaining):
    return {'id': key, 'title': title,
            'status': 'metadata_observed' if refs else 'unknown',
            'evidence': refs, 'remaining': remaining, 'runtime_passed': False}


def summarize(run):
    if run.is_symlink() or not run.is_dir():
        raise ValueError('run path must be an actual directory')
    logs = [read_log(run / name) for name in ('inventory.txt', 'observation.txt')]
    completed = read_log(run / 'completed.txt')
    available = [log for log in logs if log is not None]
    library_sections = {'candidate_library_names', 'selected_binary_metadata'}
    device_sections = {'device_names', 'all_device_names_only', 'nvidia_dsp_nodes_only'}
    report = {
        'schema': 1, 'scope': 'SD metadata; no runtime feature acceptance',
        'carplay_ready': False, 'run': str(run.resolve()),
        'logs': [{k: log[k] for k in ('name', 'bytes', 'sha256')} for log in available],
        'completion_marker_present': completed is not None,
        'command_errors': [], 'conditions': [], 'usb_observation': [],
    }
    if completed:
        report['completion_sha256'] = completed['sha256']
        report['completion_codes'] = {}
        for item in completed['records']:
            match = re.fullmatch(r'([a-z_]+)=(\d+)', item['text'].strip())
            if match:
                report['completion_codes'][match.group(1)] = int(match.group(2))
            match = re.fullmatch(r'collection_state=([a-z_]+)', item['text'].strip())
            if match:
                report['collection_state'] = match.group(1)
    completion_refs = []
    if completed:
        for item in completed['records']:
            if re.fullmatch(r'(?:script_exit|runner_exit|inventory_exit|observation_exit)=\d+|collection_state=(?:collectors_returned|partial_or_interrupted)', item['text'].strip()):
                completion_refs.append({'log': completed['name'], 'line': item['line'],
                                        'section': item['section'], 'sample': None})
    for log in available:
        for item in log['records']:
            if 'unknown:' in item['text'] or '[command incomplete/failed' in item['text']:
                report['command_errors'].append({'log': log['name'], 'line': item['line'],
                    'section': item['section'], 'sample': item['sample']})
    ref = lambda names, pattern: evidence(available, names, pattern)
    items = [
        ('qnx', 'QNX 运行环境', ref({'system'}, r'\b(QNX|Neutrino)\b'),
         '仍需匹配 SH4 SDK、ELF/依赖审计及正常启动和退出。'),
        ('usb', 'USB/AMI 基础接口', ref(device_sections | library_sections | {'processes', 'ami_config_names_only'}, r'io-usb|libusbdi|io-fs-ipod|iofs-usb-ipod|deva-ctrl-ipod|MMI3GMedia|/dev/usb'),
         '仍需确认 AMI 对应的控制器、接口占用、iPhone 配置切换和 bulk 收发。'),
        ('phone', 'Apple USB 枚举线索', ref({'usb_descriptors'}, r'\bApple\b|(?:vendor|vid)[^\n]{0,32}(?:0x)?05ac\b'),
         '枚举或 AMI 音乐不能证明 USBMUX、iAP2、认证或 CarPlay 会话建立。'),
        ('ncm', 'NCM/IPv6 线索', ref(library_sections | {'network', 'usb_descriptors'}, r'\b(ncm|cdc|inet6|IPv6)\b'),
         '仍需 NCM 控制协商、NTB 收发、QNX 网络桥及实际 IPv6 连接。'),
        ('display', '原屏/GF/layer manager', ref(device_sections | library_sections | {'processes'}, r'libgf|layermanager|io-display|devg-NV'),
         '仍需 GF 查询、LM/VFB 契约、呈现首帧、焦点和正常退出恢复。'),
        ('decoder', '视频解码/DSP', ref(library_sections | device_sections | {'processes', 'nvidia_dsp_image_identity'}, r'io-media-nvidia|nvdsprm|nvminirm|NvDspCodecs|H\.?264|H264BPDEC'),
         '仍需实际 AVC 输入与 buffer 契约、profile/level、连续解码和性能。'),
        ('audio', '原车音频接口', ref(device_sections | library_sections | {'processes'}, r'libasound|snd_pcm|io-audio|/dev/(snd|audio|pcm)'),
         '仍需 PCM 参数、路由、音乐/提示/电话混音与结束恢复。'),
        ('microphone', '麦克风接口线索', ref(library_sections | {'selected_driver_configs'}, r'snd_pcm_plugin_read|\bcapture\b|microphone|\bmic\b'),
         '仍需原车麦克风可读 PCM、回声消除及 Siri 会话。'),
        ('input', '按键/旋钮接口线索', ref(device_sections | library_sections | {'processes'}, r'/dev/input|lib.*dsi|\binput\b|GEM|gem_'),
         '仍需事件订阅与焦点；不能通过读取原厂 IPC 通道来猜测安全映射。'),
        ('crypto', '加密运行库线索', ref(library_sections, r'lib(crypto|ssl)'),
         '仍需所需密码算法及可被当前 iOS 接受的 accessory 身份；库存在不能产生身份。'),
        ('sd', 'SD 启动/返回记录', completion_refs,
         '完成标记只说明 runner 结束；原生接收器、停止、恢复与卸载仍需实机记录。'),
    ]
    report['conditions'] = [condition(*item) for item in items]
    observation = logs[1]
    if observation:
        samples = {}
        for item in observation['records']:
            if item['sample'] is None:
                continue
            sample = samples.setdefault(item['sample'], {'lines': [], 'unknown': False,
                'query_exit': None, 'query_exit_line': None, 'truncated': False})
            if item['section'] == 'usb_descriptors':
                text = item['text'].strip()
                match = re.fullmatch(r'usb_query_exit=(\d+|unknown)', text)
                if match:
                    sample['query_exit'] = int(match.group(1)) if match.group(1).isdigit() else None
                    sample['query_exit_line'] = item['line']
                if 'metadata line cap reached' in text:
                    sample['truncated'] = True
                if NEGATIVE.search(text):
                    sample['unknown'] = True
                if text and not text.startswith(('timestamp=', 'elapsed_seconds=')):
                    sample['lines'].append(text)
        previous = None
        for number, sample in sorted(samples.items()):
            valid = sample['query_exit'] == 0 and bool(sample['lines']) and not sample['unknown'] and not sample['truncated']
            digest = hashlib.sha256('\n'.join(sample['lines']).encode()).hexdigest() if valid else None
            report['usb_observation'].append({'sample': number, 'status': 'metadata_observed' if valid else 'unknown',
                'usb_query_exit': sample['query_exit'], 'query_exit_line': sample['query_exit_line'],
                'metadata_truncated': sample['truncated'],
                'descriptor_text_sha256': digest,
                'text_changed_since_previous_valid': (digest != previous) if valid and previous else None})
            if valid:
                previous = digest
    report['limitations'] = [
        'metadata_observed 是线索，不是通过；unknown 也不是不支持。',
        'USB 文本变化需要看原日志判断，不能单独证明 iPhone 插拔或配置切换。',
        '本工具不执行车机 API、不解析配对身份、不上传日志。',
    ]
    return report


def markdown(report):
    lines = ['# MMI3G 采集摘要', '', '本报告只索引静态/枚举线索。**不能据此认定 CarPlay 可用。**', '',
             '完成标记：' + ('存在' if report['completion_marker_present'] else '缺失'),
             '采集状态：' + report.get('collection_state', '未知（请看完成文件）'),
             '不完整命令记录：' + str(len(report['command_errors'])), '',
             '| 条件 | 当前证据 | 下一步 |', '|---|---|---|']
    for item in report['conditions']:
        refs = ', '.join(ref['log'] + ':' + str(ref['line']) for ref in item['evidence'][:3])
        label = '观察到元数据' if item['status'] == 'metadata_observed' else '未知'
        lines.append('| ' + item['title'] + ' | ' + label + ('；' + refs if refs else '') + ' | ' + item['remaining'] + ' |')
    lines += ['', 'USB 观察快照：' + str(len(report['usb_observation'])), '']
    lines += ['- ' + note for note in report['limitations']]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path, help='carplay-diagnostics/logs/run-N directory')
    parser.add_argument('--output-dir', type=Path, help='new directory for JSON and Markdown; default is stdout JSON')
    args = parser.parse_args()
    report = summarize(args.run)
    output = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=False)
        (args.output_dir / 'report.json').write_text(output)
        (args.output_dir / 'report.md').write_text(markdown(report))
        print(args.output_dir.resolve())
    else:
        print(output, end='')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
