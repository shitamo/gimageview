#!/usr/bin/env python3
"""Regenerate po/gimageview.pot from po/POTFILES.in without xgettext.

    python3 tools/po-update.py

Only _("..."), N_("...") and gettext("...") with plain string literals are
extracted (enough for GImageView).  The .desktop strings come from
etc/gimageview.desktop.in.
"""
import os, re, sys, time

S = r'"(?:[^"\\\n]|\\.)*"'
PAT = re.compile(r'\b(N_|_|gettext)\s*\(\s*((?:' + S + r'\s*)+)\)')
ESC = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'"}


def cstr(s):
    return ''.join(re.sub(r'\\(.)', lambda m: ESC.get(m.group(1), '\\' + m.group(1)), p[1:-1])
                   for p in re.findall(S, s))


def strip_comments(t):
    return re.sub(r'/\*.*?\*/|//[^\n]*|(' + S + r')',
                  lambda m: m.group(1) if m.group(1) else re.sub(r'[^\n]', ' ', m.group(0)),
                  t, flags=re.S)


def extract(files):
    res, order = {}, []
    def add(mid, ref):
        if mid not in res:
            res[mid] = []
            order.append(mid)
        res[mid].append(ref)
    for f in files:
        if f.endswith('.desktop.in'):
            for n, line in enumerate(open(f, encoding='utf-8'), 1):
                m = re.match(r'(Name|GenericName|Comment)=(.*)', line.rstrip('\n'))
                if m:
                    add(m.group(2), f'{f}:{n}')
            continue
        t = strip_comments(open(f, encoding='utf-8', errors='replace').read())
        for m in PAT.finditer(t):
            mid = cstr(m.group(2))
            if mid:
                add(mid, f'{f}:{t.count(chr(10), 0, m.start()) + 1}')
    return res, order


def quote(s):
    s = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t')
    parts = s.split('\n')
    lines = [p + '\\n' for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    if len(lines) <= 1:
        return '"' + (lines[0] if lines else '') + '"'
    return '""\n' + '\n'.join('"' + l + '"' for l in lines)


def refs(rs):
    out, cur = [], '#:'
    for r in rs:
        if len(cur) + 1 + len(r) > 79 and cur != '#:':
            out.append(cur)
            cur = '#:'
        cur += ' ' + r
    out.append(cur)
    return '\n'.join(out)


def write(path, header, res, order, trans):
    with open(path, 'w', encoding='utf-8') as o:
        o.write(header)
        for mid in order:
            o.write('\n' + refs(res[mid]) + '\n')
            if '%' in mid and re.search(r'%[-0-9.]*l?[sdcuxf]', mid):
                o.write('#, c-format\n')
            o.write('msgid ' + quote(mid) + '\n')
            o.write('msgstr ' + quote(trans(mid)) + '\n')


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    files = [l.strip() for l in open('po/POTFILES.in')
             if l.strip() and not l.startswith('#')]
    res, order = extract(files)
    date = time.strftime('%Y-%m-%d %H:%M%z')

    pot_header = f'''# GImageView message catalog template.
# This file is distributed under the same license as the GImageView package.
#
#, fuzzy
msgid ""
msgstr ""
"Project-Id-Version: GImageView\\n"
"Report-Msgid-Bugs-To: \\n"
"POT-Creation-Date: {date}\\n"
"PO-Revision-Date: YEAR-MO-DA HO:MI+ZONE\\n"
"Last-Translator: FULL NAME <EMAIL@ADDRESS>\\n"
"Language-Team: LANGUAGE <LL@li.org>\\n"
"MIME-Version: 1.0\\n"
"Content-Type: text/plain; charset=UTF-8\\n"
"Content-Transfer-Encoding: 8bit\\n"
'''
    write('po/gimageview.pot', pot_header, res, order, lambda m: '')

    print(f'{len(order)} messages')


main()
