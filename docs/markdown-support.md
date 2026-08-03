# Markdown support in the QML editor

AnyKeep targets GitHub Flavored Markdown (GFM), but the structured QML editor
does not yet implement every construct accepted by GitHub. This document is
the compatibility contract for editing and round trips. A construct is not
considered supported merely because `QTextDocument` happens to render it.

## Support levels

* **Structured** — represented explicitly by `NoteBlockModel`, editable with
  structure-aware navigation, selection, clipboard and undo/redo, and expected
  to survive a Markdown/plain-text/Markdown round trip.
* **Inline** — represented by character formats inside an editable text field
  and serialized back to a canonical Markdown spelling.
* **Unsupported** — may be displayed or rewritten by Qt, but AnyKeep currently
  makes no lossless round-trip guarantee.

## Current contract

| Construct | Level | Canonical form and limitations |
| --- | --- | --- |
| Paragraphs and plain text | Structured | Adjacent compatible paragraphs may share one editor block. |
| ATX headings | Structured | `#` through `######`; shortcuts are `Ctrl+1` through `Ctrl+6`, and `Ctrl+0` converts back to text. |
| Bullet lists | Structured | Nested and mixed list levels are supported; output uses `-`. |
| Ordered lists | Structured | Nested and mixed list levels are supported. Arbitrary start values and delimiter choice are not preserved. |
| Task lists | Structured | `- [ ]` and `- [x]`, including nested mixed lists. |
| Tables | Structured | Rectangular GFM tables with one header row. Inline formatting, links and `<br>` inside cells are supported. Alignment markers, captions, row/column spans and multiple header rows are not. |
| Standalone images | Structured | Markdown image blocks and AnyKeep media references. Images mixed inline with paragraph text are not yet supported. |
| Block quotes | Structured | Single-level block quotes are edited as dedicated blocks. Nested quotes and complex child blocks are not yet supported. |
| Fenced code blocks | Structured | Backtick and tilde fences are accepted. Output uses a backtick fence longer than every backtick run in the literal code. The first info-string token is preserved as the language identifier; supported languages receive syntax highlighting. Code text bypasses inline Markdown, list, table and hard-break normalization. |
| Emphasis | Inline | `*italic*`; editable with `Ctrl+I`. |
| Strong emphasis | Inline | `**bold**`; editable with `Ctrl+B`. |
| Strikethrough | Inline | `~~text~~`; editable with `Ctrl+Shift+S`. |
| Inline code | Inline | Backtick code spans; editable with `Ctrl+Backtick`. |
| Underline | Inline | `<ins>text</ins>` is canonical and editable with `Ctrl+U`. `<u>text</u>` is accepted as an input alias and rewritten as `<ins>` after editing. |
| External links | Inline | `[label](URL)` with editable label and destination; `Ctrl+K` opens the link editor. Inline styles in labels are preserved. |
| Hard breaks in table cells | Inline | Stored internally as a line break and written as `<br>`. |

HTML underline is deliberately normalized to `<ins>` because GitHub documents
that spelling for underlined text. The HTML importer also maps underline
formatting from office applications to `<ins>` instead of allowing
`QTextDocument::toMarkdown()` to misidentify it as `_emphasis_`.

## Known unsupported or lossy constructs

The following need explicit model and editor work before they can be called
supported:

* nested block quotes and complex quote children;
* indented code blocks and GitHub Mermaid diagram rendering;
* thematic breaks;
* reference-style links and reference definitions;
* multi-paragraph list items and other complex list children;
* inline images mixed with surrounding text;
* table column alignment and non-rectangular HTML table features;
* raw HTML other than the small intentional `<ins>`/`<u>` and `<br>` subset;
* footnotes, definition lists, math expressions and GitHub alerts;
* GitHub application features such as mentions, issue references, emoji
  shortcodes and automatic repository links.

Unknown syntax must currently be treated as potentially lossy. Before adding a
construct, add its block or inline representation first, then cover parsing,
QML editing, serialization, selection/clipboard, undo/redo and mode switching
with round-trip tests.

## Recommended implementation order

1. Add thematic breaks, indented code input and nested/complex block quotes.
2. Add inline images to the inline document model rather than emulating them as
   standalone QML blocks.
3. Preserve table alignment and ordered-list start metadata.
4. Add reference links and footnotes only after cross-block reference ownership
   is defined.
5. Keep raw HTML on an explicit allowlist; do not pass arbitrary active HTML
   through the editor or clipboard importer.
