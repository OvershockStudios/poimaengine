# Retired build storage

`scripts/build_storage.py` reports expendable compiler intermediates in **explicitly selected retired CMake build roots**. It does not choose retired roots by age, remove entire builds, clear dependency caches or delete game/project content. It needs only Python 3.11 or later.

Stop every build, editor, test and other process using a selected tree before planning, and keep it idle through apply. A missing lock or unchanged timestamp does **not** prove that no process is using the files. No consumer may retain these intermediates, including a thin archive outside the selected roots; scanning the selected trees cannot discover such external references. The helper neither discovers nor contains processes. Add active or retained paths explicitly with `--protect`; current `CMakePresets.json` and `CMakeUserPresets.json` output directories are protected automatically.

Report first (this writes nothing):

```sh
python scripts/build_storage.py --retired-build build/old-checkpoint --protect build/current-proof
```

Once the report is appropriate, write a new plan outside every selected root:

```sh
python scripts/build_storage.py --retired-build build/old-checkpoint --protect build/current-proof --write-plan .cache/retired-build-plan.json
```

Review its exact selected roots, candidate paths, sizes and skipped paths. The command prints the plan's SHA256. Within 15 minutes, apply that **same** plan with the printed hash:

```sh
python scripts/build_storage.py --apply-plan .cache/retired-build-plan.json --expected-plan-sha256 PRINTED_SHA256
```

Plans are created exclusively; an existing plan is never overwritten. Changed inputs, a new preset/protection configuration or an expired plan require a new report and new plan filename. Repeat `--retired-build` for up to eight distinct, nonoverlapping roots. Paths use checkout-relative forward slashes. `--checkout` selects a different checkout explicitly when necessary.

The helper admits roots below `build/` with a CMake cache naming this Poima checkout and a `CMakeFiles` directory. Candidates must be literal outputs of a known C/C++/ASM compiler rule in CMake's generated `build.ninja`, inside a `CMakeFiles/<target>.dir` or root `obj` namespace. A `.o` suffix or an `obj` directory alone is insufficient. Sources, owned sources, SDKs, assets, content, evidence, reports, original/licensed inputs, fixtures, reference directories and vendor `-src` trees remain protected; legitimate compiled `src` paths inside a target directory are allowed. Unknown graph rules/variables, Makefile-only builds and unproven objects are reported as skipped.

Recognized formats are bounded ELF relocatables, ordinary/bigobj COFF relocatables and narrowly named CMake GCC/Clang PCH files. Executables, libraries, sources, reports, arbitrary `.o` data, unknown formats and unsupported PCH formats stay. Binary recognition checks structure/signatures; it does not attest who originally produced a file. Reviewed retired-root selection and generated-output ownership remain necessary. COFF bigobj identification follows the [LLVM COFF format definition](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/BinaryFormat/COFF.h).

Any thin archive anywhere in a selected root blocks the whole root, because it can retain external object paths. Any symlink, junction/reparse ancestry, hardlinked/special file, `.ninja_lock` marker or overlap with a protected/preset path also blocks it. Unsupported preset include/inheritance/variable syntax fails closed instead of guessing. Plans are bounded to 16 MiB, selected trees to 50,000 entries each and individual candidates to 512 MiB. The JSON report includes explicit skips and potential bytes, rather than promising a storage reduction in advance.

Apply authenticates the plan into memory, verifies the full selected membership and candidate identities/hashes before the first deletion, then hashes and rechecks each candidate immediately before unlink. It removes only planned files and never removes directories. The final JSON lists deletions and skips; an error after some deletions records the completed subset and stops. Deletion has no undo, and interrupted apply is not transactional. Rebuild a retired tree if those intermediates are needed again.

These are portable path/identity checks, **not held-handle or atomic compare-and-unlink protection**. A live writer can race a check, and another process may hold an object or reference it from outside the selected trees without a thin archive. The tool cannot establish safety against such consumers or adversarial filesystem changes. Explicit retirement, no live writers/consumers and the protection list are mandatory; the tool does not make active builds safe to clean.

Synthetic filesystem tests (no engine/compiler execution):

```sh
python -B tests/build_storage_test.py
```
