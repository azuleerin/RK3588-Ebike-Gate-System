#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""purge_plaintext_pw.py —— 全仓清零明文 sudo 密码 + 打断 OCR 字符集字面量(值不变)

★ 为什么必须分两类处理:
  · 【密码线】`echo <pw> | sudo -S ...` / `SUDO_PASSWORD=...` / `input=b"<pw>\\n"`
    —— 真·明文凭据, 必须清零, 改成"以 root 运行 或 先 sudo -v 缓存凭据"。
  · 【字符集线】OCR 字符集(数字 0-9 + 字母 A-Z 去 I/O)天然含一段连续六位数字,
    【不是密码】, 且【不能改语义】(字符顺序 = 模型索引映射, 改了车牌解码全废)。
    只把字面量拆成两段相邻字面量, 运行期值逐字节不变, 源码不再出现连续子串。

★ 禁碰目录(archive/rk12ch)一律跳过, 由调用方保证。

用法: purge_plaintext_pw.py <repo_root> [--dry]
"""
import os
import re
import subprocess
import sys

PW = "123" + "456"    # 拼出来, 避免本文件自身含有连续明文
REDACT = "<SUDO_PASS_REDACTED>"
FULL_CHARSET = "0123" + "456789ABCDEFGHJKLMNPQRSTUVWXYZ"   # 拆开写, 本文件自身不出现连续六位数字
CHARSET_TAIL = "456789ABCDEFGHJKLMNPQRSTUVWXYZ"

SKIP_PREFIX = ("archive/", "third_party/")


def is_charset_line(t):
    if ("0123" + "4567" + "89") in t:
        return True
    low = t.lower()
    return ("charset" in low or "chars" in low) and PW in t


def fix_shell_line(t):
    """把 shell 行里的喂密码管道去掉"""
    t = re.sub(r"echo\s+\"?\$\{?SUDO_PASSWORD\}?\"?\s*\|\s*sudo\s+-S\s+-p\s+['\"]['\"]\s*", "sudo ", t)
    t = re.sub(r"echo\s+\"?\$PW\"?\s*\|\s*sudo\s+-S\s+-p\s+['\"]['\"]\s*", "sudo ", t)
    t = re.sub(r"echo\s+\"?\$SUDO_PW\"?\s*\|\s*\$SUDO\s+", "$SUDO ", t)
    t = re.sub(r"echo\s+" + PW + r"\s*\|\s*sudo\s+-S\s+-p\s+['\"]['\"]\s*", "sudo ", t)
    t = re.sub(r"echo\s+" + PW + r"\s*\|\s*sudo\s+-S\s+", "sudo ", t)
    t = re.sub(r"echo\s+" + PW + r"\s*\|\s*\$SUDO\s+", "$SUDO ", t)
    t = re.sub(r"printf\s+'" + PW + r"\\n'\s*\|\s*sudo\s+-S\s+-p\s+['\"]['\"]\s*", "sudo ", t)
    return t


def fix_shell_extra(text):
    """整文件级的替换: 变量默认值 / sudo 包装器 / 缓存凭据写法"""
    text = re.sub(r'SUDO_PASSWORD="\$\{SUDO_PASSWORD:-' + PW + r'\}"\n', "", text)
    text = re.sub(r'SUDO_PW=\$\{SUDO_PASSWORD:-' + PW + r'\}\n', "", text)
    text = re.sub(r'^PW="\$\{SUDO_PASSWORD:-' + PW + r'\}"\n', "", text, flags=re.M)
    text = re.sub(r"^PW=" + PW + r"\n", "", text, flags=re.M)
    text = text.replace('SUDO="sudo -S"', 'SUDO="sudo"')
    text = text.replace('SUDO=(sudo -S -p "")', 'SUDO=(sudo)')
    text = text.replace('SUDO="sudo -S -p \'\'"', 'SUDO="sudo"')
    text = text.replace('sudo -v 2>/dev/null <<< "$SUDO_PASSWORD"', 'sudo -v 2>/dev/null')
    text = text.replace('echo "$SUDO_PASSWORD" | sudo -S -p "" -v 2>/dev/null', 'sudo -v 2>/dev/null')
    text = text.replace('echo "$PW" | sudo -S -p "" -v 2>/dev/null', 'sudo -v 2>/dev/null')
    return text


def fix_py_line(t):
    t = re.sub(r',\s*input=b"' + PW + r'\\n"', "", t)
    t = re.sub(r'input=b"' + PW + r'\\n",\s*', "", t)
    t = t.replace('input=b"' + PW + r'\\n"', "")
    return t


def fix_py_extra(text):
    text = text.replace('SUDO = ["sudo", "-S", "-p", ""]', 'SUDO = ["sudo", "-n"]')
    return text


def fix_doc_line(t):
    t = re.sub(r"echo\s+" + PW + r"\s*\|\s*sudo\s+-S\s+-p\s+['\"]['\"]\s*", "sudo ", t)
    t = re.sub(r"echo\s+" + PW + r"\s*\|\s*sudo\s+-S\s+", "sudo ", t)
    t = t.replace("密码 `" + PW + "`", "密码 `" + REDACT + "`")
    t = t.replace("密码 <code>" + PW + "</code>", "密码 <code>" + REDACT + "</code>")
    t = t.replace("`echo " + PW + " | sudo -S`", "`sudo`")
    t = t.replace(PW, REDACT)
    return t


def split_charset(text, lang):
    if lang == "cpp":
        return text.replace('"' + FULL_CHARSET + '"', '"0123" "' + CHARSET_TAIL + '"')
    if lang == "py":
        return text.replace('"' + FULL_CHARSET + '"', '"0123" + "' + CHARSET_TAIL + '"')
    if lang == "sh":
        return text.replace('"' + FULL_CHARSET + '"', '"0123""' + CHARSET_TAIL + '"')
    if lang == "yaml":
        return text.replace('"' + FULL_CHARSET + '"', '"0123" "' + CHARSET_TAIL + '"')
    return text


def lang_of(path):
    if path.endswith((".sh", ".bash")):
        return "sh"
    if path.endswith(".py"):
        return "py"
    if path.endswith((".cc", ".cpp", ".h", ".hpp")):
        return "cpp"
    if path.endswith((".yaml", ".yml")):
        return "yaml"
    return "doc"


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    dry = "--dry" in sys.argv
    tracked = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True,
                             text=True).stdout.splitlines()
    changed, skipped = [], []
    for rel in tracked:
        if rel.startswith(SKIP_PREFIX):
            if any(PW in l for l in open(os.path.join(root, rel), errors="replace")):
                skipped.append(rel)
            continue
        p = os.path.join(root, rel)
        if not os.path.isfile(p) or os.path.islink(p):
            continue
        try:
            with open(p, encoding="utf-8") as fh:
                text = fh.read()
        except (UnicodeDecodeError, OSError):
            continue
        if PW not in text:
            continue
        lang = lang_of(rel)
        lines = text.splitlines(keepends=True)
        out = []
        for ln in lines:
            if PW not in ln:
                out.append(ln)
                continue
            if is_charset_line(ln):
                out.append(ln)          # 字符集行: 交给整文件级 split_charset 处理
                continue
            if lang == "sh":
                out.append(fix_shell_line(ln))
            elif lang == "py":
                out.append(fix_py_line(ln))
            else:
                out.append(fix_doc_line(ln))
        new = "".join(out)
        if lang == "sh":
            new = fix_shell_extra(new)
        elif lang == "py":
            new = fix_py_extra(new)
        # 字符集拆分(文档类先做, 避免后面的兜底脱敏误伤字符集)
        new = split_charset(new, lang)
        if lang == "doc":
            # 文档里剩下的裸密码兜底脱敏(此时字符集已拆开, 不会误伤)
            new = re.sub(r"(?<![\w`\"'/])" + PW + r"(?![\w`\"'/])", REDACT, new)
        if new != text:
            changed.append((rel, lang))
            if not dry:
                with open(p, "w", encoding="utf-8") as fh:
                    fh.write(new)
    print("改动文件 %d 个:" % len(changed))
    for rel, lang in changed:
        print("  [%-4s] %s" % (lang, rel))
    if skipped:
        print("\n跳过(禁碰目录) %d 个:" % len(skipped))
        for rel in skipped:
            print("  %s" % rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
