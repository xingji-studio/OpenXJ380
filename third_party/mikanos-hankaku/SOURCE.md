# MikanOS hankaku font source

`font/hankaku.bin` is generated from the hankaku bitmap font distributed with
MikanOS. The source text is vendored here so the bundled binary font can be
verified without depending on the network.

- Upstream project: <https://github.com/uchan-nos/mikanos>
- Upstream commit: `b5f7740c04002e67a95af16a5c6e073b664bf3f5`
- Upstream source file: `kernel/hankaku.txt`
- Source URL: <https://raw.githubusercontent.com/uchan-nos/mikanos/b5f7740c04002e67a95af16a5c6e073b664bf3f5/kernel/hankaku.txt>
- Upstream license file: <https://raw.githubusercontent.com/uchan-nos/mikanos/b5f7740c04002e67a95af16a5c6e073b664bf3f5/LICENSE>
- License: Apache-2.0

Recorded hashes:

```text
826e59f411c348ac19a6eaa539acbeb573fa8908d3cb6ac8845aa8aba5d48575  third_party/mikanos-hankaku/hankaku.txt
c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4  third_party/mikanos-hankaku/LICENSE
317e04a76e42f35eae509cd47dba86f36578cb4c479afd08363ea91ed397ce5f  font/hankaku.bin
```

Conversion rule: for every 8-character bitmap row in `hankaku.txt`, shift one
byte left and append `1` for `@` or `0` for `.`. The regression test
`test_mikanos_hankaku_source_material_is_complete` rebuilds all 4096 bytes and
compares them with `font/hankaku.bin`.
