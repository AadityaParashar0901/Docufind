# DocuFind

DocuFind is a small C++17 command-line document search engine. It recursively indexes file names, directory names, and text contents, then groups query results by match type: filename matches first, content matches next, and directory-only matches last.

## Features

- Recursive directory indexing
- File-name, directory-path, and text-content fields
- Case-insensitive tokenization
- OR search by default, with optional AND mode
- Explainable ranking by match location and number of matched query terms
- Persistent index that can be searched without rescanning source files
- Optional PDF text extraction through Poppler's `pdftotext`
- Supports common text, markup, configuration, and source-code extensions
- Direct command-line interface with no interactive menu

## Build

Requires a C++17 compiler and CMake.

```bash
cmake -S . -B build
cmake --build build
```

The executable is `build/docufind` on Linux/macOS or `build/Debug/docufind.exe` for many Windows Visual Studio builds.

## Use

Syntax:

```text
docufind <index-file> <option> <directory-or-query>
```

Build an index from a directory:

```bash
./build/docufind docufind.index --build ./documents
```

Search the index (OR mode, any query term may match):

```bash
./build/docufind docufind.index --search "machine learning"
```

Search in AND mode (every unique query term must match):

```bash
./build/docufind docufind.index --search "machine learning" --and
```

Short aliases are also available: `-b` for `--build`, and `-s` for `--search`.

Show usage:

```bash
./build/docufind --help
```

## PDF support (optional)

Install Poppler utilities so `pdftotext` is available on PATH. On Ubuntu/Debian:

```bash
sudo apt install poppler-utils
```

DocuFind will then extract PDF text during indexing. Without `pdftotext`, PDF files are skipped while other supported files continue to index.

## Ranking model

Results are grouped into clear tiers:

1. **Filename matches**: at least one query term occurs in the filename.
2. **Content matches**: no filename match, but at least one query term occurs in file contents.
3. **Directory matches**: the match comes only from directory names.

Within each tier, results with more distinct filename hits, then more content hits, then more total matched query terms appear first. Ties are broken by relative path for stable output. This is a deterministic ranking policy, not a probability estimate. It is intentionally simple and easy to explain.

## Limitations

- Indexing is a snapshot; rerun indexing to pick up filesystem changes.
- No stemming, stop-word removal, phrase search, or fuzzy matching yet.
- The index stores per-document term counts and paths, not source file contents.
- PDF support depends on an external `pdftotext` executable.
