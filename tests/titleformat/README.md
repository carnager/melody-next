# `tkfmt-1` executable corpus

`tkfmt-corpus/` contains repository-owned examples that define Melody's
formatting language together with `docs/title-formatting.md`. Every case records
the host, context values, exact source and output, and a rationale.

Run it with:

```sh
./build/dev/tests/trackknife_titleformat_corpus_runner tests/titleformat/tkfmt-corpus
```

The older `corpus/` and `probes/` files are retained as historical research from
the superseded foobar2000 compatibility effort. They do not define `tkfmt-1`,
are not executed, and must not be used to claim external compatibility.
