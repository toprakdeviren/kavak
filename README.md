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
- Token vectors, diagnostic vectors, and span utilities
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
    fprintf(stderr, "%u:%u: %s\n",
            kavak_error_line(result, i),
            kavak_error_col(result, i),
            kavak_error_message(result, i));
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
- `vendor/` — vendored dependency metadata, when present

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

The Unicode path depends on the sibling `decoder` library. Override the
location with `DECODER_ROOT=/path/to/decoder` when needed.

## Release Checklist

- `make test`, `make examples`, `make sanitize`, and `make tsan` pass
- `make wasm` produces the WASM-oriented static archive
- install and `pkg-config` dry runs pass under `DESTDIR`
- public headers and examples compile without private include paths
- source comments contain no internal task markers or private project notes
- dependency and project licenses are present

## License

MIT. See `LICENSE`.
