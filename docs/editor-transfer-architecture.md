# Editor transfer architecture

This document defines the clipboard and drag-and-drop boundary of the QML note
editor.  It applies to transfer between AnyKeep notes and to import/export with
other applications.

## Goals

* Preserve the Markdown structure supported by the editor: text, headings,
  lists, task lists, tables, images, audio recordings and links.
* Make an internal AnyKeep-to-AnyKeep transfer lossless, including media
  references and nested list structure.
* Interoperate usefully with browsers, office applications and plain-text
  editors without trusting their input.
* Keep QML responsible for interaction and geometry only.  Parsing, MIME
  choice, media cloning and model mutation belong in C++.

## Canonical payload

`NoteFragment` is an editor-independent selected-content DTO.  Its first
version is encoded as CBOR under the MIME type:

```text
application/vnd.anykeep.fragment+cbor
```

The root contains a schema identifier and a version.  The decoder rejects
unknown versions and invalid geometry, nesting and media references.  A
fragment has one of three selection shapes:

* `Inline` — a range within one text-bearing block;
* `BlockSequence` — complete blocks and partial boundary blocks;
* `TableCells` — a rectangular cell selection.

The fragment lists its referenced `MediaReference` entries.  Optional media
bytes make a payload portable when the source local media store is unavailable.
This is deliberately separate from the note file format and from QML objects.

## Export formats

Every copy or drag advertises several representations of the same selection:

| MIME type | Consumer and intent |
| --- | --- |
| `application/vnd.anykeep.fragment+cbor` | lossless internal transfer |
| `text/markdown` | GFM-compatible textual transfer |
| `text/html` | browsers and rich-text applications |
| `text/plain` | universal fallback |
| `text/tab-separated-values` | rectangular table selection |
| `image/png`, image data | a single image selection |
| `text/uri-list` | external file/URL transfer where meaningful |

The custom format is preferred only after a successful schema/version check.
Copy must not silently fall back to an incomplete private payload when a public
representation can be produced.

## Import precedence

Paste and drop choose one supported representation in this order:

1. validated AnyKeep fragment;
2. `text/markdown`;
3. TSV/CSV;
4. HTML;
5. image data;
6. URL or file list;
7. plain text.

The image fallback is considered only after the structured formats. In
particular, office suites may expose a bitmap preview alongside HTML or TSV;
AnyKeep must import the table rather than the preview.

HTML is parsed through `QTextDocument` and traversed into the supported model;
it is never converted using regular expressions.  Unsupported styling is
reduced to text.  Remote images are not downloaded on paste: their source URL
may be retained as a link only if the user has an explicit import action.
Underline from HTML and office applications is preserved as the documented
GitHub `<ins>text</ins>` spelling. `<u>` is accepted on input but is not emitted
as the canonical form. Presentation underline belonging to an ordinary HTML
link is not mistaken for explicit underlined content.

Excel and LibreOffice Calc commonly expose both HTML and TSV. A rectangular
TSV payload takes priority over HTML as well as a bitmap preview: it maps
directly to a QML table without allowing QTextDocument to create an embedded
rich-text table. Otherwise the HTML table is converted to a note table when
its dimensions are valid. The HTML path walks `QTextDocument` frames and
extracts `QTextTable` cells explicitly; `QTextDocument::toMarkdown()` is not
used to flatten a table because Qt may serialize it as ordinary text.

## Media ownership

Pasting an internal fragment into a different note creates fresh attachment
UUIDs and rewrites all `anykeep-media:` URLs.  The immutable blob ID may be
shared by `LocalMediaStore`; destination-specific remote metadata is cleared.
Moving inside one note keeps attachment IDs.  The note save path owns pruning:
it removes manifest entries only after the final model/document no longer
references them.

Media import has two policies:

* small single-image selections may include bytes in the internal payload;
* large images and audio recordings carry references first and copy bytes only
  when the destination cannot resolve the source blob.

The exact threshold is a controller setting, not a QML constant.

## Controller and model boundary

`NoteTransferController` will own the format registry and expose operations
such as `mimeDataForSelection`, `insertMimeData` and `beginDrag`.  It receives
structured selection DTOs from `NoteBlockModel` and applies one atomic model
edit per successful import.  It also produces the media mapping used to update
the note manifest.

The model must provide extraction and replacement primitives rather than let
QML delete individual delegates.  QML reports cursor/selection/drop location,
opens dialogs and renders feedback.  It must not parse clipboard HTML or own
attachment IDs.

### Current implementation boundary

The implementation supplies the versioned fragment codec, whole-block and
cross-block range extraction, MIME export/import, media cloning and `Select all
→ Copy`. A selection confined to one editor remains an inline
`QTextDocumentFragment`, so copying a few words does not unexpectedly create a
list or table. A selection spanning editors is resolved by `NoteBlockModel`:
list item kinds/indentation, a rectangular table range, headings and complete
image/audio blocks retain their structure. Copy puts private CBOR, Markdown, HTML and
plain text on the system clipboard. Referenced media travels with the private
payload; a sole image up to 8 MiB also carries its bytes, allowing PNG export
outside AnyKeep.

Markdown text and heading blocks already have a precise insertion path: an
imported structural fragment splits the current text block at the selection and
atomically replaces that range. A TSV or table fragment pasted into one table
cell writes a rectangular range and expands the table right/down as necessary.
A list fragment pasted into a flat list item preserves item types, checked
state and relative indentation. Replacing an item with nested descendants is
currently rejected rather than risking reparenting them; that case needs an
explicit subtree selection DTO. A media-bearing fragment pasted into a
text/heading block is cloned before insertion and its new references are
appended to the destination note manifest in the same editor operation.
Keyboard and context-menu paste share one entry point. The `QQuickWidget`
event filter invokes it before `TextArea` sees the key event, preventing Qt's
native rich-text importer from racing the block-model operation.

## Drag-and-drop behaviour

* A drag starts only after the normal platform drag threshold.
* Same-note move is an atomic extract/insert transaction and adjusts the
  destination after removing the source range.
* Cross-note move is copy-first.  Source deletion happens only after the
  destination confirms a successful internal drop.
* An external drag is always a copy; a move request is never used to delete
  data outside AnyKeep.
* Drops into tables use a rectangular replace/expand operation.  Other drops
  insert before, after or split the target text block at the cursor.

## Failure handling and observability

Bad custom CBOR, malformed HTML, unreadable files, media-store failures and
unsupported MIME data leave the note unchanged.  The controller returns a
user-facing reason and emits structured debug logging with MIME choice,
fragment version, counts and media policy, but never logs note contents.

Unit tests cover codec compatibility and malformed input.  Model tests cover
selection extraction and atomic replacement.  QML integration tests cover MIME
precedence, same-note move, cross-note media cloning, table paste and external
format export.
