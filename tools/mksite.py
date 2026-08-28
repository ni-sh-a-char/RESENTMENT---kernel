#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the RESENTMENT documentation site into site/.

Renders the Markdown in docs/ and the top-level policy documents into the same
shell as the hand-written landing page, so the whole site is static HTML that
works with JavaScript disabled and needs no build tooling beyond a Python that
ships with the machine.

The Markdown subset here is exactly what this project's documents use:
headings, fenced code, tables, lists, block quotes, rules, and inline emphasis,
code, links and images. That is a deliberate ceiling - a general Markdown
implementation is a dependency, and this is forty lines of parsing.

  python tools/mksite.py            # build into site/
  python tools/mksite.py --serve    # build, then serve it on :8000
"""

import argparse
import html
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
OUT = os.path.join(ROOT, "site")

# Source document, output name, title, and the group it belongs to in the nav.
PAGES = [
    ("docs/ARCHITECTURE.md", "architecture.html", "Architecture",      "Design"),
    ("docs/SMP.md",          "smp.html",          "Multiprocessing",   "Design"),
    ("docs/USERSPACE.md",    "userspace.html",    "Ring 3",            "Design"),
    ("docs/KAALKA.md",       "kaalka.html",       "Kaalka",            "Design"),
    ("docs/GRAPH.md",        "graph.html",        "The runtime graph", "Design"),
    ("docs/AI.md",           "ai.html",           "The AI subsystem",  "Design"),
    ("docs/SHE.md",          "she.html",          "The SHE language",  "Design"),
    ("docs/BUILDING.md",     "building.html",     "Building",          "Guides"),
    ("docs/PORTING.md",      "porting.html",      "Porting",           "Guides"),
    ("CONTRIBUTING.md",      "contributing.html", "Contributing",      "Guides"),
    ("ROADMAP.md",           "roadmap.html",      "Roadmap",           "Project"),
    ("CHANGELOG.md",         "changelog.html",    "Changelog",         "Project"),
    ("SECURITY.md",          "security.html",     "Security",          "Project"),
    ("GOVERNANCE.md",        "governance.html",   "Governance",        "Project"),
]

# Where a relative link in a Markdown document should point once rendered.
LINK_MAP = {}
for _src, _out, _t, _g in PAGES:
    LINK_MAP[_src] = _out
    LINK_MAP[os.path.basename(_src)] = _out
    LINK_MAP["../" + _src] = _out
LINK_MAP["README.md"] = "index.html"
LINK_MAP["LICENCE"] = "https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/LICENCE"


# ----------------------------------------------------------------- inline

def slug(text):
    s = re.sub(r"[^a-z0-9\s-]", "", text.lower())
    return re.sub(r"[\s-]+", "-", s).strip("-") or "section"


def map_link(href):
    if href.startswith(("http://", "https://", "#", "mailto:")):
        return href
    anchor = ""
    if "#" in href:
        href, anchor = href.split("#", 1)
        anchor = "#" + anchor
    key = href.lstrip("./")
    if key in LINK_MAP:
        return LINK_MAP[key] + anchor
    if href in LINK_MAP:
        return LINK_MAP[href] + anchor
    # Anything else is a path in the repository; point at the source of truth.
    return ("https://github.com/ni-sh-a-char/RESENTMENT---kernel/blob/main/"
            + key + anchor)


def inline(text):
    """Escape, then apply inline Markdown. Code spans are protected first so
    that emphasis inside them is left alone."""
    spans = []

    def stash(m):
        spans.append(m.group(1))
        return "\x00%d\x00" % (len(spans) - 1)

    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text, quote=False)

    text = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)",
                  lambda m: '<img src="%s" alt="%s">'
                            % (html.escape(m.group(2), quote=True),
                               html.escape(m.group(1), quote=True)),
                  text)
    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)",
                  lambda m: '<a href="%s"%s>%s</a>'
                            % (html.escape(map_link(m.group(2)), quote=True),
                               ' target="_blank" rel="noopener"'
                               if m.group(2).startswith("http") else "",
                               m.group(1)),
                  text)
    text = re.sub(r"&lt;(https?://[^&\s]+)&gt;",
                  lambda m: '<a href="%s" target="_blank" rel="noopener">%s</a>'
                            % (m.group(1), m.group(1)), text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![\w*])\*([^*\n]+)\*(?![\w*])", r"<em>\1</em>", text)

    def unstash(m):
        return "<code>%s</code>" % html.escape(spans[int(m.group(1))], quote=False)

    return re.sub(r"\x00(\d+)\x00", unstash, text)


# ------------------------------------------------------------------ block

def render(md):
    """Markdown to (html, [(level, id, title)]) for the table of contents."""
    lines = md.replace("\r\n", "\n").split("\n")
    out, toc = [], []
    i, n = 0, len(lines)
    seen = {}

    def open_para(buf):
        if buf:
            out.append("<p>%s</p>" % inline(" ".join(buf)))
            del buf[:]

    para = []

    while i < n:
        line = lines[i]

        # fenced code
        m = re.match(r"^```(\w*)\s*$", line)
        if m:
            open_para(para)
            lang = m.group(1)
            i += 1
            body = []
            while i < n and not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            out.append('<div class="code"><button class="copy" '
                       'type="button" aria-label="Copy">copy</button>'
                       '<pre><code class="lang-%s">%s</code></pre></div>'
                       % (lang or "text",
                          html.escape("\n".join(body), quote=False)))
            continue

        # heading
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            open_para(para)
            lvl = len(m.group(1))
            title = m.group(2).strip()
            sid = slug(re.sub(r"[`*\[\]()]", "", title))
            if sid in seen:
                seen[sid] += 1
                sid = "%s-%d" % (sid, seen[sid])
            else:
                seen[sid] = 0
            if lvl <= 3:
                toc.append((lvl, sid, re.sub(r"[`*]", "", title)))
            out.append('<h%d id="%s">%s<a class="anchor" href="#%s" '
                       'aria-hidden="true">#</a></h%d>'
                       % (lvl, sid, inline(title), sid, lvl))
            i += 1
            continue

        # horizontal rule
        if re.match(r"^\s*(---+|\*\*\*+)\s*$", line):
            open_para(para)
            out.append("<hr>")
            i += 1
            continue

        # table
        if line.lstrip().startswith("|") and i + 1 < n and \
           re.match(r"^\s*\|[\s:\-|]+\|\s*$", lines[i + 1]):
            open_para(para)

            def cells(row):
                return [c.strip() for c in row.strip().strip("|").split("|")]

            head = cells(line)
            i += 2
            rows = []
            while i < n and lines[i].lstrip().startswith("|"):
                rows.append(cells(lines[i]))
                i += 1
            t = ['<div class="table-wrap"><table><thead><tr>']
            t += ["<th>%s</th>" % inline(c) for c in head]
            t.append("</tr></thead><tbody>")
            for r in rows:
                t.append("<tr>" + "".join("<td>%s</td>" % inline(c)
                                          for c in r) + "</tr>")
            t.append("</tbody></table></div>")
            out.append("".join(t))
            continue

        # block quote
        if line.startswith(">"):
            open_para(para)
            body = []
            while i < n and lines[i].startswith(">"):
                body.append(lines[i][1:].lstrip())
                i += 1
            out.append("<blockquote>%s</blockquote>"
                       % render("\n".join(body))[0])
            continue

        # lists, including nesting by indentation
        if re.match(r"^\s*([-*+]|\d+\.)\s+", line):
            open_para(para)
            out.append(read_list(lines, i))
            i = read_list.end
            continue

        if not line.strip():
            open_para(para)
            i += 1
            continue

        para.append(line.strip())
        i += 1

    open_para(para)
    return "\n".join(out), toc


def read_list(lines, start, indent=0):
    """One list, consuming continuation lines and nested lists."""
    i, n = start, len(lines)
    ordered = bool(re.match(r"^\s*\d+\.\s+", lines[i]))
    items = []
    while i < n:
        m = re.match(r"^(\s*)([-*+]|\d+\.)\s+(.*)$", lines[i])
        if not m:
            if lines[i].strip() and lines[i].startswith(" " * (indent + 2)) and items:
                items[-1].append(lines[i].strip())
                i += 1
                continue
            if not lines[i].strip() and i + 1 < n and \
               re.match(r"^\s*([-*+]|\d+\.)\s+", lines[i + 1]):
                i += 1
                continue
            break
        this_indent = len(m.group(1))
        if this_indent < indent:
            break
        if this_indent > indent and items:
            nested = read_list(lines, i, this_indent)
            items[-1].append("\x01" + nested)
            i = read_list.end
            continue
        items.append([m.group(3)])
        i += 1

    read_list.end = i
    tag = "ol" if ordered else "ul"
    parts = ["<%s>" % tag]
    for it in items:
        text = " ".join(p for p in it if not p.startswith("\x01"))
        nested = "".join(p[1:] for p in it if p.startswith("\x01"))
        parts.append("<li>%s%s</li>" % (inline(text), nested))
    parts.append("</%s>" % tag)
    return "".join(parts)


read_list.end = 0


# ------------------------------------------------------------------ build

def nav_html(current):
    groups = []
    for _src, out, title, group in PAGES:
        if not groups or groups[-1][0] != group:
            groups.append((group, []))
        groups[-1][1].append((out, title))

    parts = ['<a class="nav-home%s" href="index.html">Overview</a>'
             % (" current" if current == "index.html" else "")]
    for group, items in groups:
        parts.append('<div class="nav-group"><span class="nav-label">%s</span>'
                     % html.escape(group))
        for out, title in items:
            parts.append('<a href="%s"%s>%s</a>'
                         % (out, ' class="current"' if out == current else "",
                            html.escape(title)))
        parts.append("</div>")
    return "\n".join(parts)


def toc_html(toc):
    if len(toc) < 3:
        return ""
    parts = ['<nav class="toc" aria-label="On this page">',
             '<span class="toc-label">On this page</span>']
    for lvl, sid, title in toc:
        if lvl == 1:
            continue
        parts.append('<a class="lvl%d" href="#%s">%s</a>'
                     % (lvl, sid, html.escape(title)))
    parts.append("</nav>")
    return "\n".join(parts)


def build():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)

    template = open(os.path.join(WEB, "template.html"), encoding="utf-8").read()

    for asset in ("style.css", "site.js", "favicon.svg", "og.svg"):
        src = os.path.join(WEB, asset)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(OUT, asset))

    # The link preview is the generated PNG rather than the SVG beside it:
    # every social network rasterises, and several refuse SVG outright.
    card = os.path.join(ROOT, "media", "social-preview.png")
    if os.path.exists(card):
        shutil.copy(card, os.path.join(OUT, "social-preview.png"))

    # The landing page is hand-written; only its nav is substituted.
    index = open(os.path.join(WEB, "index.html"), encoding="utf-8").read()
    index = index.replace("{{nav}}", nav_html("index.html"))
    open(os.path.join(OUT, "index.html"), "w", encoding="utf-8").write(index)

    built = 1
    for src, out, title, _group in PAGES:
        path = os.path.join(ROOT, src)
        if not os.path.exists(path):
            print("  skipped (missing): %s" % src)
            continue
        md = open(path, encoding="utf-8").read()
        body, toc = render(md)
        page = (template
                .replace("{{title}}", html.escape(title))
                .replace("{{nav}}", nav_html(out))
                .replace("{{toc}}", toc_html(toc))
                .replace("{{source}}", src)
                .replace("{{content}}", body))
        open(os.path.join(OUT, out), "w", encoding="utf-8").write(page)
        built += 1

    # GitHub Pages runs Jekyll by default, which would ignore anything it does
    # not recognise. This turns that off.
    open(os.path.join(OUT, ".nojekyll"), "w").close()

    print("built %d pages into %s" % (built, os.path.relpath(OUT, ROOT)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serve", action="store_true",
                    help="serve the built site on http://localhost:8000")
    args = ap.parse_args()

    rc = build()
    if rc or not args.serve:
        return rc

    import http.server
    import socketserver
    os.chdir(OUT)
    with socketserver.TCPServer(("", 8000),
                                http.server.SimpleHTTPRequestHandler) as httpd:
        print("serving on http://localhost:8000  (ctrl-c to stop)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
