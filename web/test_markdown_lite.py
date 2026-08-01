#!/usr/bin/env python3
"""
web/test_markdown_lite.py -- unit tests for markdown_lite.py, the small
Markdown -> HTML/WordPress-block converter wired into the report page/HTML
export/WordPress export for wspy-analyze's .md output (aianalysis.<model>.md).
Not wired into make test/run_tests.sh, matching this codebase's existing
"web/ is stdlib-only Python, not covered by the C toolchain's test targets"
convention -- run standalone:

    python3 web/test_markdown_lite.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import markdown_lite as md


class ParseBlocksTest(unittest.TestCase):
    def test_heading_levels(self):
        blocks = md.parse_blocks("# One\n## Two\n### Three")
        self.assertEqual([(b["type"], b["level"], b["inline"]) for b in blocks],
                          [("heading", 1, "One"), ("heading", 2, "Two"), ("heading", 3, "Three")])

    def test_paragraph_joins_soft_wrapped_lines(self):
        blocks = md.parse_blocks("This is one\nparagraph split\nacross lines.\n\nSecond paragraph.")
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0]["inline"], "This is one paragraph split across lines.")
        self.assertEqual(blocks[1]["inline"], "Second paragraph.")

    def test_unordered_list(self):
        blocks = md.parse_blocks("- first\n- second\n- third")
        self.assertEqual(blocks, [{"type": "list", "ordered": False,
                                    "items": ["first", "second", "third"]}])

    def test_ordered_list(self):
        blocks = md.parse_blocks("1. first\n2. second")
        self.assertEqual(blocks, [{"type": "list", "ordered": True, "items": ["first", "second"]}])

    def test_gfm_table(self):
        text = "| Metric | Value |\n|---|---|\n| IPC | 1.23 |\n| L3 miss | 4.5% |"
        blocks = md.parse_blocks(text)
        self.assertEqual(len(blocks), 1)
        self.assertEqual(blocks[0]["type"], "table")
        self.assertEqual(blocks[0]["header"], ["Metric", "Value"])
        self.assertEqual(blocks[0]["rows"], [["IPC", "1.23"], ["L3 miss", "4.5%"]])

    def test_table_not_confused_with_paragraph_containing_pipe(self):
        # A single line with a pipe but no following separator row is just a
        # paragraph -- the separator row (---|---) is what marks a real table.
        blocks = md.parse_blocks("this sentence has a | in it, not a table")
        self.assertEqual(blocks[0]["type"], "paragraph")

    def test_fenced_code_block(self):
        blocks = md.parse_blocks("```\nretire: 60.2%\nfrontend: 12%\n```")
        self.assertEqual(blocks, [{"type": "code", "text": "retire: 60.2%\nfrontend: 12%"}])

    def test_fenced_code_block_not_inline_parsed(self):
        # Asterisks/backticks inside a fenced block are literal, not markdown.
        blocks = md.parse_blocks("```\n**not bold** `not code`\n```")
        self.assertEqual(blocks[0]["text"], "**not bold** `not code`")

    def test_horizontal_rule(self):
        blocks = md.parse_blocks("above\n\n---\n\nbelow")
        self.assertEqual([b["type"] for b in blocks], ["paragraph", "hr", "paragraph"])

    def test_mixed_document(self):
        text = (
            "## Executive Summary\n"
            "The run is retire-bound.\n\n"
            "### Metrics\n"
            "| Metric | Value |\n|---|---|\n| IPC | 1.23 |\n\n"
            "- observation one\n- observation two\n"
        )
        blocks = md.parse_blocks(text)
        self.assertEqual([b["type"] for b in blocks],
                          ["heading", "paragraph", "heading", "table", "list"])


class RenderInlineHtmlTest(unittest.TestCase):
    def test_bold(self):
        self.assertEqual(md.render_inline_html("this is **bold** text"),
                          "this is <strong>bold</strong> text")

    def test_italic(self):
        self.assertEqual(md.render_inline_html("this is *italic* text"),
                          "this is <em>italic</em> text")

    def test_inline_code(self):
        self.assertEqual(md.render_inline_html("run `wspy --topdown`"),
                          "run <code>wspy --topdown</code>")

    def test_code_span_protected_from_bold_italic_substitution(self):
        self.assertEqual(md.render_inline_html("`**not bold**`"), "<code>**not bold**</code>")

    def test_html_special_chars_escaped(self):
        self.assertEqual(md.render_inline_html("a < b & c > d"), "a &lt; b &amp; c &gt; d")

    def test_escaping_happens_before_tag_injection_not_after(self):
        # A literal "<script>" in the model's own prose must never survive
        # as a real tag just because inline formatting ran afterward.
        out = md.render_inline_html("**<script>alert(1)</script>**")
        self.assertNotIn("<script>", out)
        self.assertIn("&lt;script&gt;", out)


class ToHtmlTest(unittest.TestCase):
    def test_heading_and_paragraph(self):
        out = md.to_html("# Title\n\nSome text.")
        self.assertEqual(out, "<h1>Title</h1>\n<p>Some text.</p>")

    def test_table_renders_real_table_tags(self):
        out = md.to_html("| A | B |\n|---|---|\n| 1 | 2 |")
        self.assertIn("<table>", out)
        self.assertIn("<th>A</th>", out)
        self.assertIn("<td>1</td>", out)

    def test_list_renders_ul(self):
        out = md.to_html("- one\n- two")
        self.assertEqual(out, "<ul><li>one</li><li>two</li></ul>")

    def test_ordered_list_renders_ol(self):
        out = md.to_html("1. one\n2. two")
        self.assertEqual(out, "<ol><li>one</li><li>two</li></ol>")

    def test_code_block_not_html_escaped_twice(self):
        out = md.to_html("```\n<tag> & \"quote\"\n```")
        self.assertEqual(out, '<pre><code>&lt;tag&gt; &amp; &quot;quote&quot;</code></pre>')

    def test_hr(self):
        self.assertIn("<hr>", md.to_html("above\n\n---\n\nbelow"))


class ToWpBlocksTest(unittest.TestCase):
    def test_heading_becomes_wp_heading_block(self):
        out = md.to_wp_blocks("## Section")
        self.assertIn("<!-- wp:heading", out)
        self.assertIn('"level": 2', out)
        self.assertIn("<h2>Section</h2>", out)
        self.assertIn("<!-- /wp:heading -->", out)

    def test_table_becomes_wp_table_block_not_raw_html(self):
        out = md.to_wp_blocks("| A | B |\n|---|---|\n| 1 | 2 |")
        self.assertIn("<!-- wp:table", out)
        self.assertIn('<figure class="wp-block-table">', out)
        self.assertIn("<table>", out)
        self.assertIn("<!-- /wp:table -->", out)

    def test_list_becomes_wp_list_block(self):
        out = md.to_wp_blocks("- one\n- two")
        self.assertIn("<!-- wp:list", out)
        self.assertIn("<li>one</li>", out)

    def test_multiple_blocks_joined_by_blank_line(self):
        out = md.to_wp_blocks("# Title\n\nBody text.")
        self.assertIn("<!-- /wp:heading -->\n\n<!-- wp:paragraph", out)


if __name__ == "__main__":
    unittest.main()
