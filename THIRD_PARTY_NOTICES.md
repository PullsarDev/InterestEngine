# Third-party notices

The test-only `test::Random::Next()` in `tests/test_support.hpp` uses the SplitMix64
sequence published by Sebastiano Vigna. It is also used by the benchmark harness;
it is not part of the production library. The wrapper uses explicit per-instance
state rather than the reference implementation's global state.

The following notice is retained from the [primary reference implementation](https://prng.di.unimi.it/splitmix.c):

```text
Written in 2015 by Sebastiano Vigna (vigna@acm.org)

To the extent possible under law, the author has dedicated all copyright
and related and neighboring rights to this software to the public domain
worldwide.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.
THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```
