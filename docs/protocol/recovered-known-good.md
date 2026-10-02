# Direct historical recovery: pre-bounded snapshot v1

Recovered on 2026-10-02 from this conversation's actual tool records. This is
a source recovery checkpoint, not a bug fix, stability claim, or new physical
test. Branch: `historical/recovered-known-good`, based on original baseline
`bb134825cd34ffde106fb25d4aec5ccec7e5c8f7`.

## Direct evidence and chronological replay

1. 2026-09-26 16:57:40 KST / 07:57:40 UTC:
   `call_CLrKP7eUAE9MPTcg7YaMEnD0` read the complete firmware using
   `Get-Content -LiteralPath`, then printed a Git diff. The source section
   precedes the `diff --git` marker and contains the entire file from includes
   to the last closing brace, including GET17=0. The combined output reports
   truncation in the later output; there is no truncation marker or missing
   section inside the extracted firmware source.
2. 16:59:28 KST / 07:59:28 UTC:
   `call_v6gWUZ8Nflz4yweG3UCRIkWu` applied the actual first DebugSnapshot patch.
3. 17:08:20 KST / 08:08:20 UTC:
   `call_aUuDD1YmsIUWQt7ocxqkeN40` applied incomplete-record selection and renamed
   the snapshot-copy function. Both patch calls have successful tool outputs.

The firmware here was obtained by replaying these two original patches forward
on that recorded source read. Each hunk matched one unique source position.
No current v4 or reverse-reconstructed source was used to fill missing code.

Evidence files are in [recovery-evidence](recovery-evidence/):

- `pre-instrumentation-read.txt`: extracted complete source section as rendered
  in the historical Get-Content output, not an original binary file dump.
- `01-first-snapshot.patch`: exact original patch text.
- `02-incomplete-selection.patch`: exact original patch text. It also contains
  the historical Android .gitignore addition; only firmware hunks were replayed
  to recover this firmware. This recovery does not import an Android project.

The original absolute paths in patch headers are intentional provenance data.
These files are historical records, not instructions to apply to main.

## Recovered behavior

- GET command 17 returns 0, already present in the recorded input source.
- DebugSnapshot version 1, packed size 88, four slots, thirteen flags/timestamps.
- Odd/even generation checks, up to three consistent-copy attempts, most recent
  incomplete CMD9 preferred, latest record as fallback.
- EP0 vendor IN request C0/D9, wValue 464D, wIndex 1, wLength 88.
- CMD9 callback capture, main-loop entry, safeTune entry/return, RSSI entry/return,
  notify state, busy checks, attempts and return values for both notify steps.
- Legacy CMD9 assigns current_freq before calling safeTune(current_freq).
  safeTune still calls rx.setChannel; setup also keeps its legacy initial tune.
- There is no bounded tuning, tune_result, boot-stage field, synthetic boot
  snapshot, timeout, recovery, watchdog, or external library change.
- Historical limitations remain, including unbounded library STC polling,
  callback/main-loop concurrency, and original notify buffer/state behavior.

The generation/flags/timestamps and legacy tune path match the architecture
used for the user's reported successful ~67 ms safeTune / ~0.6 ms RSSI and
flags=0x1FFF captures. This branch was not flashed or physically retested.
The same historical state also captured an intermittent safeTune non-return.

## Confidence, differences, and uncertainty

Directly recovered: all firmware statements, comments and diagnostic fields
come from the complete recorded read and the two actual patches. No missing
runtime code was inferred or improved. The GET17 edit itself was already in
that read; this does not prove which actor originally made that edit.

Byte-level uncertainty: Get-Content is line-oriented, so original encoding,
line endings and EOF bytes are not a reliable binary dump. This recovery uses
UTF-8 without BOM, preserves context line endings from the recorded read, and
uses LF for patch-added lines. These serialization choices are explicit; they
do not add firmware behavior.

Recovered firmware SHA-256:
`2998E187EE8E8525842E42FE889E502353EDCCEF5CE92B5ADDB9C673E276F63B`.

The session recorded a later pre-bounded v1 SHA-256 at 17:46 KST:
`791279FD054F64113AF920096683CB8B6A831202656BF48BD01F6C23743E9DB5`.
It is not equal to this recovery; byte identity is not claimed. Exact binary
correspondence between a particular flashed image and the successful captures
cannot be established from this source read and patch evidence alone.

Compared with reconstruction commit
`849220ec72c7fdb1e764a7c90a08097d3c2b14c4`, firmware text is exactly equal after
CRLF/LF normalization. The raw difference consists of line endings. The key
distinction is provenance: direct chronological replay here versus reversing
later v4 changes there. This branch includes evidence rather than a restored
Android app; the existing reconstruction's format-v1 reader remains available.
The current format-v4 Android parser is incompatible with this 88-byte format.

Compared with baseline, differences are the observed pre-instrumentation
comment/source presentation, GET17=0, and the original snapshot instrumentation.
No additional firmware feature or correction was introduced.

## Preservation and validation

- main/v4 remains at `13557959317cb7b19febe6a64f72e70677e90f48`.
- historical/working-snapshot-v1 remains at
  `849220ec72c7fdb1e764a7c90a08097d3c2b14c4`.
- Build uses the isolated Arduino-Pico 6.1.1 archive environment, bundled
  Adafruit TinyUSB 3.7.7, installed PU2CLR SI470X 1.0.5, Waveshare RP2040 Zero,
  and Adafruit TinyUSB USB stack. Dependency versions are not changed.
- External SI470X.cpp / SI470X.h hashes are checked unchanged.
- Firmware compile succeeded: 85,796 bytes flash and 17,836 bytes global RAM;
  the packed 88-byte snapshot static assertion passed. Build artifacts remain
  outside the repository in the isolated temporary build environment.
- No flash/install, merge, push, reset, rebase or existing-branch modification.
