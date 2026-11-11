# Benchmarks

Two benchmark checks pin recorded output from the world generator:
`light-seam` measures generation-time light, and `column-digest` hashes every
decorated block state. `.github/workflows/ci.yml` compares against an explicit
record, not the newest file in `bench/baseline/`. A moved pin is therefore a
deliberate change to review, not a test failure to silence.

## When to re-record

Re-record only when the generator or lighting change is intentional and the new
values are the expected result on every supported toolchain. First inspect the
delta: `bench/seam_track.py` prints the tallies, and the column comparison
prints every changed column. If only one compiler or OS moved, treat it as a
portability bug and fix the code; do not move the pin.

The seam counts are machine-independent light levels, and the column digests
are hashes of generated block states. Record from one representative machine;
CI runs the probes on Linux gcc, Linux clang, macOS and MinGW and requires all
four to agree with each other and with the pinned record.

## Light seam

Build the probe and write a new record:

    cmake --build build --target plusmc-light-seam
    python3 bench/seam_track.py --binary build/plusmc-light-seam --save

`--save` creates `bench/baseline/<date>-<machine>-light-seam-3seeds-r2.json`
(or a `-2` suffix if that filename already exists) and prints the comparison
against the newest existing record. Verify the new pin:

    python3 bench/seam_track.py --binary build/plusmc-light-seam \
        --against bench/baseline/<new-record>.json --fail-on-change

Update both `SEAM_BASELINE` entries in `.github/workflows/ci.yml` (the build
matrix and the Windows job) to the new path.

## Decorated columns

Build the digest probe and record it:

    cmake --build build --target plusmc-column-digest
    ./build/plusmc-column-digest --json \
        bench/baseline/<date>-<machine>-column-digest-3seeds-r2.json

Check a fresh run against that record:

    ./build/plusmc-column-digest --json /tmp/column-digest.json
    python3 tools/compare_column_digests.py \
        --baseline bench/baseline/<new-record>.json /tmp/column-digest.json

Update `COLUMN_BASELINE` in `.github/workflows/ci.yml` to the new path. Commit
the new record and the workflow edit together, and keep the old record in place
so the change stays reviewable.
