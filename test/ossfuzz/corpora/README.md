# OSS-Fuzz seed corpora

Seed inputs for the OSS-Fuzz harnesses in `test/ossfuzz/`, kept next to the
code they exercise:

- `corpora/<target>/` — raw seed files for fuzz target `<target>`
- `../dicts/<target>.dict` — optional dictionary for `<target>`

The OSS-Fuzz build packages these automatically at build time (it zips
corpora for each built target to `$OUT/<target>_seed_corpus.zip` and copies
matching `.dict` files into `$OUT`), so adding or updating seeds here needs
no corresponding change in the oss-fuzz repository.

All files in these directories are fuzzing inputs only — they are not used
by the regular test suites.
