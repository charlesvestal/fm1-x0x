#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""docs/MANUAL.md -> docs/manual.html. The Markdown is the source; run this after editing it.

  tools/manual_html.py [IN.md [OUT.html]] [--fragment]

--fragment writes the page without its <!doctype>/<html>/<head>/<body> wrapper (for hosts that
add their own); the default is a complete document that opens in any browser.

Handles what the manual uses: headings, paragraphs, **bold**, `code`, lists, tables, fenced
code, rules, and the contents list (linked to the sections by number)."""
import html
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
PART_COLOURS = {"909": "#e8860f", "808": "#e0483e", "303A": "#4fae3a", "303B": "#2e9bd6", "BREAK": "#a066e0"}

CSS = """
:root {
  --ground: #f4f5f6; --sheet: #ffffff; --ink: #1c1f23; --muted: #5b626a; --rule: #d9dde1;
  --accent: #c8700a; --code-bg: #eef0f2; --swatch-ring: rgba(0,0,0,.18);
  color-scheme: light;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --ground: #131518; --sheet: #1a1d21; --ink: #e6e8eb; --muted: #9aa1a9; --rule: #2d3137;
    --accent: #f0a043; --code-bg: #24282d; --swatch-ring: rgba(255,255,255,.22);
    color-scheme: dark;
  }
}
:root[data-theme="dark"] {
  --ground: #131518; --sheet: #1a1d21; --ink: #e6e8eb; --muted: #9aa1a9; --rule: #2d3137;
  --accent: #f0a043; --code-bg: #24282d; --swatch-ring: rgba(255,255,255,.22);
  color-scheme: dark;
}
body {
  background: var(--ground); color: var(--ink); margin: 0;
  font: 400 17px/1.6 "Barlow", "Helvetica Neue", Arial, sans-serif;
  padding-inline: 16px; padding-block: 32px 64px;
}
main { max-width: 46rem; margin: 0 auto; }
h1, h2, h3 { font-family: "Barlow Semi Condensed", "Barlow", "Arial Narrow", sans-serif; text-wrap: balance; line-height: 1.15; }
h1 { font-size: 2.3rem; font-weight: 700; margin: 0 0 .3rem; letter-spacing: -.01em; }
h2 { font-size: 1.55rem; font-weight: 600; margin: 3rem 0 .8rem; padding-top: 1rem; border-top: 1px solid var(--rule); }
h2 .num { color: var(--accent); margin-right: .35em; font-variant-numeric: tabular-nums; }
h3 { font-size: 1.2rem; font-weight: 600; margin: 2rem 0 .5rem; }
p, li { max-width: 65ch; }
p { margin: 0 0 1rem; }
ul, ol { padding-left: 1.3rem; margin: 0 0 1rem; }
li { margin: .2rem 0; }
strong { font-weight: 600; }
a { color: var(--accent); text-underline-offset: 2px; }
a:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; border-radius: 2px; }
code, pre { font-family: "IBM Plex Mono", ui-monospace, Menlo, Consolas, monospace; font-size: .86em; }
code { background: var(--code-bg); padding: .08em .35em; border-radius: 4px; }
pre { background: var(--code-bg); padding: 14px 16px; border-radius: 8px; overflow-x: auto; line-height: 1.45; margin: 0 0 1.2rem; }
pre code { background: none; padding: 0; font-size: inherit; }
hr { display: none; }
.table { overflow-x: auto; margin: 0 0 1.3rem; }
table { border-collapse: collapse; width: 100%; font-size: .95rem; font-variant-numeric: tabular-nums; }
th, td { text-align: left; vertical-align: top; padding: .45rem .7rem .45rem 0; border-bottom: 1px solid var(--rule); }
th { font-family: "Barlow Semi Condensed", "Barlow", sans-serif; font-weight: 600; text-transform: uppercase; letter-spacing: .04em; font-size: .82rem; color: var(--muted); }
td:first-child { white-space: nowrap; }
.lede { color: var(--muted); margin-bottom: 1.6rem; }
.status { background: var(--sheet); border: 1px solid var(--rule); border-radius: 8px; padding: 12px 16px; }
.status p { margin: 0; }
.swatch { display: inline-block; width: .7em; height: .7em; border-radius: 3px; margin-right: .4em; vertical-align: .02em; box-shadow: 0 0 0 1px var(--swatch-ring); }
nav.contents ol { columns: 2 14rem; column-gap: 2rem; padding-left: 1.3rem; }
nav.contents li { break-inside: avoid; }
@media (prefers-reduced-motion: no-preference) { html { scroll-behavior: smooth; } }
"""


def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    for name, col in PART_COLOURS.items():          # a part named at the start of a table cell gets its swatch
        t = re.sub(rf"^<strong>{name}</strong>", f'<span class="swatch" style="background:{col}"></span><strong>{name}</strong>', t)
    return t


def slug(title):
    m = re.match(r"(\d+)\.", title)
    return f"s{m.group(1)}" if m else re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")


def convert(md):
    lines = md.splitlines()
    out, i, contents, sections = [], 0, None, {}
    for ln in lines:                                  # section numbers -> ids, for the contents
        m = re.match(r"## (\d+)\. (.+)", ln)
        if m:
            sections[m.group(2).strip()] = f"s{m.group(1)}"
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("```"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            out.append("<pre><code>" + html.escape("\n".join(lines[i + 1:j]), quote=False) + "</code></pre>")
            i = j + 1
            continue
        if ln.startswith("# "):
            out.append(f"<h1>{inline(ln[2:])}</h1>")
        elif ln.startswith("## "):
            t = ln[3:].strip()
            m = re.match(r"(\d+)\. (.+)", t)
            if t == "Contents":
                contents = True
                out.append('<nav class="contents" aria-label="Contents"><h2 id="contents">Contents</h2>')
            elif m:
                out.append(f'<h2 id="s{m.group(1)}"><span class="num">{m.group(1)}</span>{inline(m.group(2))}</h2>')
            else:
                out.append(f'<h2 id="{slug(t)}">{inline(t)}</h2>')
        elif ln.startswith("### "):
            out.append(f"<h3>{inline(ln[4:])}</h3>")
        elif ln.strip() == "---":
            if contents:
                out.append("</nav>")
                contents = None
            out.append("<hr>")
        elif ln.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            head, body = rows[0], [r for r in rows[1:] if not set("".join(r)) <= set("-: ")]
            out.append('<div class="table"><table><thead><tr>' + "".join(f"<th>{inline(c)}</th>" for c in head) +
                       "</tr></thead><tbody>" + "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>"
                                                         for r in body) + "</tbody></table></div>")
            continue
        elif re.match(r"(\d+\.|-) ", ln):
            ordered = ln[0].isdigit()
            items = []
            while i < len(lines) and (re.match(r"(\d+\.|-) ", lines[i]) or (lines[i].startswith("  ") and items)):
                if lines[i].startswith("  "):
                    items[-1] += " " + lines[i].strip()
                else:
                    items.append(re.sub(r"^(\d+\.|-) ", "", lines[i]))
                i += 1
            if contents:
                items = [f'<a href="#{sections.get(t.strip(), slug(t))}">{inline(t)}</a>' for t in items]
            else:
                items = [inline(t) for t in items]
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{t}</li>" for t in items) + f"</{tag}>")
            continue
        elif ln.strip():
            para = [ln]
            while i + 1 < len(lines) and lines[i + 1].strip() and not re.match(r"(#|\||```|---|\d+\. |- )", lines[i + 1]):
                i += 1
                para.append(lines[i])
            text = inline(" ".join(p.strip() for p in para))
            if text.startswith("<strong>Status.</strong>"):
                out.append(f'<div class="status"><p>{text}</p></div>')
            elif out and out[-1].startswith("<h1>"):
                out.append(f'<p class="lede">{text}</p>')
            else:
                out.append(f"<p>{text}</p>")
        i += 1
    return "\n".join(out)


def main():
    args = [a for a in sys.argv[1:] if a != "--fragment"]
    fragment = "--fragment" in sys.argv
    src = Path(args[0]) if args else SRC / "docs" / "MANUAL.md"
    dst = Path(args[1]) if len(args) > 1 else SRC / "docs" / "manual.html"
    body = convert(src.read_text())
    page = ("<title>X0X Manual</title>\n"
            '<meta name="description" content="The manual for X0X, groovebox firmware for the M-VAVE FM-1.">\n'
            '<link rel="preconnect" href="https://fonts.googleapis.com">\n'
            '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>\n'
            '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;600&'
            'family=Barlow+Semi+Condensed:wght@600;700&family=IBM+Plex+Mono:wght@400&display=swap">\n'
            f"<style>{CSS}</style>\n<main>\n{body}\n</main>\n")
    if not fragment:
        head, _, rest = page.partition("<main>")
        page = ('<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
                '<meta name="viewport" content="width=device-width, initial-scale=1">\n' + head +
                "</head>\n<body>\n<main>" + rest + "</body>\n</html>\n")
    dst.write_text(page)
    print(f"{dst}: {len(page)} bytes")


if __name__ == "__main__":
    main()
