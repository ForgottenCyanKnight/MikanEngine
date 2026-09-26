#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""i18n 一致性校验工具（MikanEngine 多语言系统配套，2026-09-26）。

功能：
  1. 扫描 src/ 下所有 Tr("...") 实际使用的 key（按文件统计出现次数）；
  2. 与 engine/i18n/<lang>.json 字典求差集：
     - 缺条目的 key：切到该语言后仍显示中文（Tr 回退原文）——需要补字典；
     - 字典未使用的条目：可清理的死条目；
  3. 统计各源文件中尚未包装 Tr 的硬编码中文字符串（ImGui UI 调用行）。

用法（仓库根目录）：
  python tools/i18n_check.py            # 默认 en-US
  python tools/i18n_check.py zh-CN      # 校验其他语言包
"""

import collections
import glob
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
I18N_DIR = os.path.join(ROOT, 'engine', 'i18n')

ZH = re.compile(u'[\u4e00-\u9fff]')
TR_KEY = re.compile(u'Tr\\s*\\(\\s*"([^"]+)"')
UI_CALL = re.compile(
    u'\\b(Button|MenuItem|BeginMenu|Text|TextUnformatted|TextDisabled|Checkbox|'
    u'RadioButton|Selectable|Combo|SeparatorText|InputText|InputTextWithHint|'
    u'InputInt|SetTooltip|TabItem|Begin|BeginChild|WindowTitle)\\s*\\(')
LITERAL = re.compile(u'"([^"]*[\\u4e00-\\u9fff][^"]*)"')
WRAPPER = re.compile(u'(Tr|WindowTitle|fitSize)\\s*\\($')


def scan_tr_keys():
    """Tr("...") 实际使用的 key → 出现次数。"""
    used = collections.Counter()
    for path in glob.glob(os.path.join(SRC, '**', '*.cpp'), recursive=True):
        for line in io.open(path, encoding='utf-8', errors='replace'):
            for m in TR_KEY.finditer(line):
                key = m.group(1).replace(chr(92)+chr(110), chr(10))
                used[key] += 1
    return used


def scan_unwrapped_ui_strings():
    """ImGui UI 调用行里尚未包装 Tr 的中文字面量 → [(文件, 行号, 行)]。"""
    remaining = []
    for path in glob.glob(os.path.join(SRC, '**', '*.cpp'), recursive=True):
        name = os.path.basename(path)
        for lineno, line in enumerate(io.open(path, encoding='utf-8',
                                              errors='replace'), 1):
            s = line.strip()
            if s.startswith('//') or not ZH.search(s):
                continue
            if 'LOG' in s:
                continue  # 日志行面向开发者，暂不翻译
            if not UI_CALL.search(s):
                continue
            for m in LITERAL.finditer(s):
                prefix = s[:m.start()].rstrip()
                if WRAPPER.search(prefix):
                    continue  # 已包装（Tr/WindowTitle/fitSize 内部自翻译）
                remaining.append((name, lineno, s[:110]))
                break
    return remaining


def main():
    language = sys.argv[1] if len(sys.argv) > 1 else 'en-US'
    dict_path = os.path.join(I18N_DIR, language + '.json')
    with io.open(dict_path, encoding='utf-8') as f:
        dictionary = json.load(f)

    used = scan_tr_keys()
    missing = {k: n for k, n in used.items() if k not in dictionary}
    unused = sorted(k for k in dictionary if k not in used)
    remaining = scan_unwrapped_ui_strings()

    out = io.open(os.path.join(ROOT, 'tools', 'i18n_report.txt'), 'w',
                  encoding='utf-8')
    w = out.write
    w(u'[i18n] 语言: %s\n' % language)
    w(u'[i18n] Tr key 使用: %d 种 / %d 处\n' % (len(used), sum(used.values())))
    w(u'[i18n] 字典条目: %d\n' % len(dictionary))
    w(u'[i18n] 缺条目（切语言后仍显示中文）: %d\n' % len(missing))
    for key, count in sorted(missing.items(), key=lambda kv: (-kv[1], kv[0])):
        w(u'  缺 [%dx] %s\n' % (count, key.replace(u'\n', u'\\n')))
    w(u'[i18n] 字典未使用条目: %d\n' % len(unused))
    for key in unused:
        w(u'  闲 [%s]\n' % key)
    w(u'[i18n] 未包装 Tr 的 UI 中文串: %d\n' % len(remaining))
    for name, lineno, text in remaining:
        w(u'  %s:%d: %s\n' % (name, lineno, text))
    out.close()

    console = io.open(os.path.join(ROOT, 'tools', 'i18n_report.txt'),
                      encoding='utf-8')
    sys.stdout.write(console.read().encode('utf-8', 'replace').decode('utf-8', 'replace'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
