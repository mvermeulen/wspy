"""
web/markdown_lite.py -- a small, dependency-free (stdlib-only, see CLAUDE.md's
web/ entry) Markdown -> HTML/WordPress-block converter, for wspy-analyze's own
output (aianalysis.<model>.md / aiprompt.critique.<model>.md): gpt-oss:20b and
friends write markdown-flavored prose by default (primed by the prompt
template's own markdown headers, and now explicitly asked for -- see
prompts/perf_analysis*.tmpl), and until this module existed the report page/
HTML export/WordPress export all just html.escape()'d it into a <pre> block,
so a model's **bold**/table syntax showed up as literal asterisks and pipes
instead of being rendered.

Deliberately not a full CommonMark implementation -- covers what a short
narrative/structured-report LLM response actually uses: ATX headings (#..######),
paragraphs, bold/italic/inline-code spans, single-level bullet/numbered lists,
GFM pipe tables, fenced code blocks, and horizontal rules. No nested lists,
blockquotes, or link syntax -- none of those show up in this tool's actual
prompt/response shape, and adding parser surface for markdown a model never
produces here isn't worth the risk of misrendering something it does.

Two consumers:
  - to_html(text): a single HTML string for the report page and the HTML
    export format.
  - to_wp_blocks(text): one Gutenberg block-comment per markdown block
    (heading/paragraph/list/table/code/separator), matching how every other
    content type in server.py's render_export_wordpress() already works --
    stays natively editable in the WP block editor afterward, not one opaque
    raw-HTML blob.
"""
import html
import json
import re

_ATX_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*$")
_HR_RE = re.compile(r"^(-{3,}|\*{3,}|_{3,})\s*$")
_TABLE_SEPARATOR_RE = re.compile(r"^\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?$")
_UL_ITEM_RE = re.compile(r"^\s*[-*+]\s+(.*)$")
_OL_ITEM_RE = re.compile(r"^\s*\d+\.\s+(.*)$")
_FENCE_RE = re.compile(r"^```")


def _split_table_row(line):
    """One GFM table row -> list of cell strings -- strips one optional
    leading/trailing '|' (the outer pipes GFM tables conventionally have but
    don't require) without treating an escaped '\\|' inside a cell as a
    separator (rare in this tool's actual output, but cheap to get right)."""
    stripped = line.strip()
    if stripped.startswith("|"):
        stripped = stripped[1:]
    if stripped.endswith("|") and not stripped.endswith("\\|"):
        stripped = stripped[:-1]
    cells = re.split(r"(?<!\\)\|", stripped)
    return [c.strip().replace("\\|", "|") for c in cells]


def parse_blocks(text):
    """Markdown source -> ordered list of block dicts:
      {"type": "heading", "level": 1-6, "inline": str}
      {"type": "paragraph", "inline": str}
      {"type": "list", "ordered": bool, "items": [str, ...]}
      {"type": "table", "header": [str, ...], "rows": [[str, ...], ...]}
      {"type": "code", "text": str}
      {"type": "hr"}
    "inline"/list items/table cells still contain raw markdown inline syntax
    (bold/italic/code spans) -- render_inline_html() below resolves that;
    kept separate so to_html()/to_wp_blocks() can each wrap the same parsed
    structure in their own tags without re-parsing."""
    lines = text.splitlines()
    blocks = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue

        if _FENCE_RE.match(line):
            code_lines = []
            i += 1
            while i < n and not _FENCE_RE.match(lines[i]):
                code_lines.append(lines[i])
                i += 1
            i += 1  # skip closing fence (or EOF -- an unterminated fence just runs to the end)
            blocks.append({"type": "code", "text": "\n".join(code_lines)})
            continue

        m = _ATX_HEADING_RE.match(line)
        if m:
            blocks.append({"type": "heading", "level": len(m.group(1)), "inline": m.group(2)})
            i += 1
            continue

        if _HR_RE.match(line):
            blocks.append({"type": "hr"})
            i += 1
            continue

        if "|" in line and i + 1 < n and "|" in lines[i + 1] and _TABLE_SEPARATOR_RE.match(lines[i + 1].strip()):
            header = _split_table_row(line)
            i += 2  # header + separator row
            rows = []
            while i < n and "|" in lines[i] and lines[i].strip():
                rows.append(_split_table_row(lines[i]))
                i += 1
            blocks.append({"type": "table", "header": header, "rows": rows})
            continue

        ul_m = _UL_ITEM_RE.match(line)
        ol_m = _OL_ITEM_RE.match(line)
        if ul_m or ol_m:
            ordered = ol_m is not None
            item_re = _OL_ITEM_RE if ordered else _UL_ITEM_RE
            items = [(ul_m or ol_m).group(1)]
            i += 1
            while i < n:
                m2 = item_re.match(lines[i])
                if not m2:
                    break
                items.append(m2.group(1))
                i += 1
            blocks.append({"type": "list", "ordered": ordered, "items": items})
            continue

        para_lines = [line.strip()]
        i += 1
        while i < n and lines[i].strip() and not (
                _ATX_HEADING_RE.match(lines[i]) or _HR_RE.match(lines[i]) or _FENCE_RE.match(lines[i])
                or _UL_ITEM_RE.match(lines[i]) or _OL_ITEM_RE.match(lines[i])):
            para_lines.append(lines[i].strip())
            i += 1
        blocks.append({"type": "paragraph", "inline": " ".join(para_lines)})

    return blocks


_CODE_SPAN_RE = re.compile(r"`([^`]+)`")
_BOLD_RE = re.compile(r"\*\*(.+?)\*\*|__(.+?)__")
_ITALIC_RE = re.compile(r"\*(.+?)\*|_(.+?)_")


def render_inline_html(text):
    """One block's inline markdown (bold/italic/code spans) -> safe HTML.
    Escapes the raw text first, then layers formatting on top -- code spans
    are pulled out into placeholders before bold/italic substitution runs so
    e.g. a literal '**' inside `a code span` is never mistaken for emphasis,
    then spliced back in verbatim."""
    escaped = html.escape(text)

    spans = []

    def _stash_code(m):
        spans.append(f"<code>{m.group(1)}</code>")
        return f"\x00{len(spans) - 1}\x00"

    escaped = _CODE_SPAN_RE.sub(_stash_code, escaped)
    escaped = _BOLD_RE.sub(lambda m: f"<strong>{m.group(1) or m.group(2)}</strong>", escaped)
    escaped = _ITALIC_RE.sub(lambda m: f"<em>{m.group(1) or m.group(2)}</em>", escaped)
    escaped = re.sub(r"\x00(\d+)\x00", lambda m: spans[int(m.group(1))], escaped)
    return escaped


def to_html(text):
    """Full markdown source -> one HTML string (headings/paragraphs/lists/
    tables/code/hr), for the report page (render_block_content()) and the
    HTML export format."""
    parts = []
    for b in parse_blocks(text):
        t = b["type"]
        if t == "heading":
            parts.append(f'<h{b["level"]}>{render_inline_html(b["inline"])}</h{b["level"]}>')
        elif t == "paragraph":
            parts.append(f'<p>{render_inline_html(b["inline"])}</p>')
        elif t == "list":
            tag = "ol" if b["ordered"] else "ul"
            items = "".join(f"<li>{render_inline_html(item)}</li>" for item in b["items"])
            parts.append(f"<{tag}>{items}</{tag}>")
        elif t == "table":
            head = "".join(f"<th>{render_inline_html(c)}</th>" for c in b["header"])
            body = "".join(
                "<tr>" + "".join(f"<td>{render_inline_html(c)}</td>" for c in row) + "</tr>"
                for row in b["rows"]
            )
            parts.append(f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>")
        elif t == "code":
            parts.append(f'<pre><code>{html.escape(b["text"])}</code></pre>')
        elif t == "hr":
            parts.append("<hr>")
    return "\n".join(parts)


def _wp_block(name, inner_html, attrs=None):
    """Local copy of server.py's own _wp_block() helper (Gutenberg block
    comment wrapping) -- kept here rather than imported so this module stays
    a leaf (server.py imports this, not the other way around)."""
    attrs_json = f" {json.dumps(attrs)}" if attrs else ""
    return f"<!-- wp:{name}{attrs_json} -->\n{inner_html}\n<!-- /wp:{name} -->"


def to_wp_blocks(text):
    """Full markdown source -> one Gutenberg block-comment sequence (joined
    by blank lines, ready to drop straight into render_export_wordpress()'s
    own parts list) -- a heading becomes a real wp:heading block, a table a
    real wp:table block, etc., so the result stays editable as native blocks
    in the WP editor rather than landing as one opaque raw-HTML blob."""
    parts = []
    for b in parse_blocks(text):
        t = b["type"]
        if t == "heading":
            level = b["level"]
            parts.append(_wp_block(
                "heading", f'<h{level}>{render_inline_html(b["inline"])}</h{level}>', {"level": level}))
        elif t == "paragraph":
            parts.append(_wp_block("paragraph", f'<p>{render_inline_html(b["inline"])}</p>'))
        elif t == "list":
            tag = "ol" if b["ordered"] else "ul"
            items = "".join(f"<li>{render_inline_html(item)}</li>" for item in b["items"])
            parts.append(_wp_block(
                "list", f'<{tag} class="wp-block-list">{items}</{tag}>',
                {"ordered": True} if b["ordered"] else None))
        elif t == "table":
            head = "".join(f"<th>{render_inline_html(c)}</th>" for c in b["header"])
            body = "".join(
                "<tr>" + "".join(f"<td>{render_inline_html(c)}</td>" for c in row) + "</tr>"
                for row in b["rows"]
            )
            table_html = (f'<figure class="wp-block-table"><table><thead><tr>{head}</tr></thead>'
                           f'<tbody>{body}</tbody></table></figure>')
            parts.append(_wp_block("table", table_html))
        elif t == "code":
            parts.append(_wp_block(
                "code", f'<pre class="wp-block-code"><code>{html.escape(b["text"])}</code></pre>'))
        elif t == "hr":
            parts.append(_wp_block("separator", '<hr class="wp-block-separator"/>'))
    return "\n\n".join(parts)
