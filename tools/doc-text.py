#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# doc-text.py: the plain text manual (doc/<lang>/gimageview.txt, shown in
# [Help | Document | Plain Text]) made from the HTML manual, so that the
# two say the same.  Run it after changing the HTML manual:
#
#     tools/doc-text.py doc/ja > doc/ja/gimageview.txt
#     tools/doc-text.py doc/en > doc/en/gimageview.txt
#
# The pages are taken from index.html along the "next" links.  Pictures
# are left out (their captions too); links become their text.

import html
import re
import sys
import unicodedata
from html.parser import HTMLParser

WIDTH = 78      # the lines; punctuation may hang over by 2 (80)

# a line does not start with these (kinsoku)
NO_START = set('、。，．・：；？！）」』】〉》〕］｝ー々ゝゞぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ' ')]},.:;!?%')
NO_END = set('（「『【〈《〔［｛([{')


def cwidth(ch):
    return 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1


def swidth(s):
    return sum(cwidth(c) for c in s)


def is_wide(ch):
    return cwidth(ch) == 2


def tokens(text):
    """words of Latin text, single characters of wide text, spaces"""
    out = []
    for m in re.finditer(r'\s+|[^\s]', text):
        t = m.group(0)
        if t.isspace():
            out.append(' ')
        elif out and not is_wide(t) and out[-1] != ' ' and not is_wide(out[-1][-1]) \
                and not (out[-1][-1] in '/-' and len(out[-1]) > 1):
            out[-1] += t
        else:
            out.append(t)
    return out


def wrap(text, width, first, rest):
    """text in lines of width columns, first and rest: their prefixes"""
    text = re.sub(r'\s+', ' ', text).strip()
    if not text:
        return []
    lines, cur, prefix = [], '', first
    avail = lambda p: width - swidth(p)
    for tk in tokens(text):
        if tk == ' ':
            if cur:
                cur += ' '
            continue
        if swidth(cur) + swidth(tk) <= avail(prefix) or not cur.strip():
            cur += tk
            continue
        # kinsoku: carry the last character, or keep the punctuation on
        if tk[0] in NO_START and swidth(cur) + swidth(tk) <= avail(prefix) + 2:
            cur += tk
            continue
        carry = ''
        if cur and cur[-1] in NO_END:
            carry, cur = cur[-1], cur[:-1]
        lines.append(prefix + cur.rstrip())
        prefix, cur = rest, carry + tk
        while swidth(cur) > avail(prefix):   # a word longer than a line
            cut, w = 0, 0
            for i, c in enumerate(cur):
                w += cwidth(c)
                if w > avail(prefix):
                    break
                cut = i + 1
            lines.append(prefix + cur[:cut])
            cur = cur[cut:]
    if cur.strip():
        lines.append(prefix + cur.rstrip())
    return lines


class Node:
    def __init__(self, tag, attrs, parent):
        self.tag, self.attrs, self.parent, self.kids = tag, dict(attrs), parent, []


VOID = {'br', 'img', 'hr', 'meta', 'link', 'col', 'input'}
CLOSES_P = {'div', 'ul', 'ol', 'dl', 'pre', 'table', 'h1', 'h2', 'h3', 'h4', 'h5', 'p',
            'hr', 'blockquote'}


class Tree(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node('root', {}, None)
        self.cur = self.root

    def handle_starttag(self, tag, attrs):
        # a block closes an open <p> (as browsers do)
        if tag in CLOSES_P:
            n = self.cur
            while n is not self.root and n.tag in INLINE:
                n = n.parent
            if n.tag == 'p':
                self.cur = n.parent
        n = Node(tag, attrs, self.cur)
        self.cur.kids.append(n)
        if tag not in VOID:
            self.cur = n

    def handle_startendtag(self, tag, attrs):
        self.cur.kids.append(Node(tag, attrs, self.cur))

    def handle_endtag(self, tag):
        n = self.cur
        while n is not self.root and n.tag != tag:
            n = n.parent
        if n is not self.root:
            self.cur = n.parent

    def handle_data(self, data):
        self.cur.kids.append(data)


INLINE = {'a', 'span', 'b', 'i', 'em', 'strong', 'code', 'tt', 'kbd', 'sup', 'sub',
          'font', 'small', 'big', 'u', 'img', 'br', 'acronym', 'abbr', 'q'}


def classes(n):
    return n.attrs.get('class', '').split()


class Render:
    def __init__(self):
        self.out = []

    def blank(self):
        if self.out and self.out[-1] != '':
            self.out.append('')

    def emit(self, lines):
        self.out.extend(lines)

    def inline_text(self, n, pre=False):
        if isinstance(n, str):
            return n
        if n.tag == 'img':
            return ''
        if n.tag == 'br':
            return '\n'
        return ''.join(self.inline_text(k, pre) for k in n.kids)

    def para(self, n, indent, first=None, rest=None):
        """the text of n wrapped; <br> makes new lines"""
        text = self.inline_text(n)
        first = ' ' * indent if first is None else first
        rest = ' ' * indent if rest is None else rest
        lines = []
        for i, part in enumerate(text.split('\n')):
            lines += wrap(part, WIDTH, first if i == 0 else rest, rest)
        return lines

    def flow(self, kids, indent):
        """children: inline runs become paragraphs, blocks are rendered"""
        run = []

        def flush():
            if run:
                tmp = Node('p', {}, None)
                tmp.kids = list(run)
                lines = self.para(tmp, indent)
                if lines:
                    self.emit(lines)
                    self.blank()
                run.clear()
        for k in kids:
            if isinstance(k, str) or k.tag in INLINE:
                run.append(k)
            else:
                flush()
                self.block(k, indent)
        flush()

    def block(self, n, indent):
        t = n.tag
        cl = classes(n)
        if t in ('script', 'style', 'head', 'title'):
            return
        if t == 'div' and ('figure' in cl or 'mediaobject' in cl or 'navheader' in cl
                           or 'navfooter' in cl):
            return
        if t in ('h1', 'h2', 'h3', 'h4', 'h5'):
            self.blank()
            text = re.sub(r'\s+', ' ', self.inline_text(n)).strip()
            if text:
                self.emit(wrap(text, WIDTH, '', ''))
                if t in ('h1', 'h2'):
                    self.emit(['=' * min(WIDTH, swidth(text))])
                self.blank()
            return
        if t == 'p':
            lines = self.para(n, indent)
            if lines:
                self.emit(lines)
                self.blank()
            return
        if t == 'hr':
            self.blank()
            self.emit(['-' * WIDTH])
            self.blank()
            return
        if t == 'pre':
            text = self.inline_text(n).strip('\n')
            for line in text.split('\n'):
                self.emit([(' ' * (indent + 2) + line).rstrip()])
            self.blank()
            return
        if t in ('ul', 'ol'):
            depth = 0
            p = n.parent
            while p is not None:
                if isinstance(p, Node) and p.tag in ('ul', 'ol'):
                    depth += 1
                p = p.parent
            num = 0
            for li in n.kids:
                if not isinstance(li, Node) or li.tag != 'li':
                    continue
                num += 1
                mark = ('%d. ' % num) if t == 'ol' else ('・ ' if depth % 2 == 0 else '□ ')
                self.item(li, indent, ' ' + mark)
            return
        if t == 'dl':
            for k in n.kids:
                if not isinstance(k, Node):
                    continue
                if k.tag == 'dt':
                    lines = self.para(k, indent)
                    if lines:
                        self.emit(lines)
                elif k.tag == 'dd':
                    self.flow(k.kids, indent + 4)
                    self.blank()
            return
        if t == 'table':
            for tr in self.rows(n):
                cells = [re.sub(r'\s+', ' ', self.inline_text(c)).strip() for c in tr]
                line = '  '.join(c for c in cells if c)
                if line:
                    self.emit(wrap(line, WIDTH, ' ' * indent, ' ' * (indent + 2)))
            self.blank()
            return
        # div, li content, body, ...
        self.flow(n.kids, indent)

    def dt_followed_by_dd(self, dl, dt):
        kids = [k for k in dl.kids if isinstance(k, Node)]
        i = kids.index(dt)
        return i + 1 < len(kids) and kids[i + 1].tag == 'dd'

    def rows(self, table):
        out = []

        def walk(n):
            for k in n.kids:
                if isinstance(k, Node):
                    if k.tag == 'tr':
                        out.append([c for c in k.kids if isinstance(c, Node) and c.tag in ('td', 'th')])
                    else:
                        walk(k)
        walk(table)
        return out

    def item(self, li, indent, mark):
        """a list item: the first paragraph after the mark, the rest under it"""
        hang = ' ' * (indent + swidth(mark))
        kids = list(li.kids)
        # the first inline run (or first <p>) goes on the mark's line
        first, rest = [], []
        i = 0
        while i < len(kids) and (isinstance(kids[i], str) or kids[i].tag in INLINE):
            first.append(kids[i])
            i += 1
        if not ''.join(self.inline_text(k) if isinstance(k, Node) else k for k in first).strip():
            first = []
            while i < len(kids) and isinstance(kids[i], str):
                i += 1
            if i < len(kids) and isinstance(kids[i], Node) and kids[i].tag == 'p':
                first = kids[i].kids
                i += 1
        rest = kids[i:]
        tmp = Node('p', {}, None)
        tmp.kids = first
        lines = self.para(tmp, 0, ' ' * indent + mark, hang)
        if lines:
            self.emit(lines)
        if any(not (isinstance(k, str) and not k.strip()) for k in rest):
            self.blank()
            self.flow(rest, len(hang))
        self.blank()


def parse(path):
    t = Tree()
    t.feed(open(path, encoding='utf-8').read())
    return t.root


def find(n, pred):
    if isinstance(n, Node):
        if pred(n):
            return n
        for k in n.kids:
            r = find(k, pred)
            if r:
                return r
    return None


def next_page(root):
    link = find(root, lambda n: n.tag == 'link' and n.attrs.get('rel') == 'next')
    return link.attrs['href'] if link else None


def main():
    d = sys.argv[1].rstrip('/')
    r = Render()
    page = 'index.html'
    seen = set()
    while page and page not in seen:
        seen.add(page)
        root = parse('%s/%s' % (d, page))
        body = find(root, lambda n: n.tag == 'body')
        if page == 'index.html':
            # the title, the abstract and the contents (not the revision history)
            book = find(body, lambda n: n.tag == 'div' and 'article' in classes(n)) or body
            for k in book.kids:
                if isinstance(k, Node) and k.tag == 'div' and 'titlepage' in classes(k):
                    # the title and the abstract (not the authors)
                    for x in (find(k, lambda n: n.tag == 'h1'),
                              find(k, lambda n: n.tag == 'div' and 'abstract' in classes(n))):
                        if x:
                            r.block(x, 0)
                elif isinstance(k, Node) and k.tag == 'div' and 'toc' in classes(k):
                    toc = Node('div', {}, None)
                    toc.kids = [x for x in k.kids
                                if not (isinstance(x, Node) and x.tag == 'dl'
                                        and find(x, lambda n: n.tag == 'a'
                                                 and n.attrs.get('href') == 'changelog.html'))]
                    r.block(toc, 0)
                elif isinstance(k, Node):
                    r.block(k, 0)
            r.blank()
            r.emit(['-' * WIDTH])
            r.blank()
        else:
            r.block(body, 0)
        page = next_page(root)
    while r.out and r.out[-1] == '':
        r.out.pop()
    sys.stdout.write('\n'.join(line.rstrip() for line in r.out) + '\n')


if __name__ == '__main__':
    main()
