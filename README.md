# kavak

`kavak` is an embeddable frontend toolkit for language implementations.
It provides C building blocks for source mapping, tokenization, Pratt-style
expression parsing, AST storage, type arenas, diagnostics, and lightweight
semantic analysis.

The public contract lives in [`include/kavak.h`](include/kavak.h). The
project is still pre-1.0, so API and ABI may change while the descriptor
surface settles.

## What It Provides

- Borrowed source buffers with line/column mapping
- Descriptor-driven lexer configuration for identifiers, numbers, strings,
  comments, operators, indentation, and automatic semicolons
- Built-in UTF-8 decoding and Unicode identifier classification (UAX #31
  XID_Start / XID_Continue), with no external dependency
- Token vectors, diagnostic vectors, and span utilities
- String interning for pointer-identity name comparison
- Pratt expression parser plus recursive-descent helper APIs
- Arena-backed AST and type allocation
- Scope, symbol lookup, narrowing, and name-resolution helpers
- Native and WASM-oriented static library builds

## Basic Use

```c
#include <kavak.h>
#include <my_language.h>

extern const KavakLanguage MY_LANGUAGE;

KavakSession *session = kavak_session_new(&MY_LANGUAGE);
KavakResult *result = kavak_analyze_bytes(session, bytes, len, "main.kv");

if (result) {
  for (uint32_t i = 0; i < kavak_error_count(result); ++i) {
    const char *msg; uint32_t line, col;
    kavak_error_at(result, i, &msg, &line, &col);
    fprintf(stderr, "%u:%u: %s\n", line, col, msg);
  }
  kavak_result_free(result);
}

kavak_session_free(session);
```

## Repository Layout

- `include/kavak.h` — public API
- `src/` — implementation
- `tests/` — unit tests, sanitizer smoke, and benchmarks
- `examples/` — runnable public-header examples
- `scripts/` — Unicode XID table generator and the vendored UCD data it reads

## Build

```sh
make
make test
make examples
make sanitize
make tsan
make wasm
make bench
```

kavak is self-contained — it links no external libraries. Unicode identifier
classification uses a generated table ([`src/unicode_xid.c`](src/unicode_xid.c));
regenerate it after a Unicode version bump with
`python3 scripts/gen_xid_table.py`.

## License

MIT. See `LICENSE`.
