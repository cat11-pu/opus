# Game Voice Mode — Latency Budget, Edge Cases, Loss Stats

## Latency budget (<= 40 ms end-to-end)

| Stage                | Cost   |
|----------------------|--------|
| Frame size           | 20 ms  |
| Jitter/FEC lookahead | 20 ms  |
| **Total codec path** | **40 ms** |

Network transit and OS audio buffering are outside this budget; the
interface guarantees the codec+jitter path never exceeds 40 ms.
In-band FEC needs exactly one frame of lookahead (the LBRR copy of
frame N rides in packet N+1), which is why the jitter depth is fixed
at 2 slots and cannot be raised without breaking the budget.

## Loss resilience contract

- 1 lost packet: recovered bit-exactly-ish via in-band FEC (LBRR),
  audible as slightly lower bandwidth, no gap.
- 2–3 consecutive losses: first frame may be FEC-recovered if the
  burst is followed by a received packet; remaining frames use Opus
  PLC. Speech stays intelligible.
- 4+ consecutive losses: PLC fades to comfort noise after
  `GV_PLC_FADE_AFTER` frames; counted as `bursts_gt3`.
- Recovery is sample-continuous: `gv_dec_pull` always emits exactly
  960 samples per slot, so there is never a timeline discontinuity,
  resample, or sample drop when normal decode resumes.

## Edge cases

### Sequencing
1. **Out-of-order within window** — packet N+1 arrives before N.
   `gv_dec_push` stores both; slot N decodes normally when pulled.
2. **Duplicate packet** — same `seq` pushed twice. Second push is
   dropped, counted in `packets_duplicate`.
3. **Late packet** — arrives after its slot was already emitted via
   FEC/PLC. Dropped, counted in `packets_late`. Never retro-patches
   output (would break sample continuity).
4. **Seq wrap-around** — `uint16` 65535 → 0 is a legal successor;
   comparisons use modular arithmetic, not `<`.
5. **Large seq jump (resync)** — gap larger than `GV_JITTER_DEPTH`
   implies stream restart or clock discontinuity; caller should invoke
   `gv_dec_reset`. The jitter buffer never fast-forwards on its own.

### FEC/PLC interaction
6. **FEC for a slot already emitted** — cannot happen: pull order is
   slot-sequential, and a slot is only PLC'd once the lookahead window
   has passed.
7. **FEC packet present but no LBRR data** — encoder may omit
   redundancy (e.g. DTX, or CELT-only frames); falls through to PLC,
   counted in `plc_frames`, not `fec_recovered`.
8. **Loss immediately after a burst** — PLC state carries over; Opus
   decoder state is continuous because PLC frames also advance it.
9. **Burst of exactly 3 then recovery** — worst guaranteed case:
   frame 1 FEC-recovered (from packet 4's LBRR if burst is 3 and
   packet 4 arrives), frames 2–3 PLC, frame 4 normal decode. Output
   remains intelligible and sample-aligned.

### Signalling / lifecycle
10. **DTX vs loss** — a gap with no packets where the sender signalled
    DTX (zero-length keepalive or seq continuity) is emitted as
    `GV_SRC_DTX` comfort noise and is NOT counted as loss.
    Distinguishing requires the sender to advance `seq` during DTX
    (send nothing, but the next real packet's seq reveals the gap was
    intentional) — see note below.
11. **First packet / stream join** — decoder anchors slot 0 to the
    first received seq; no PLC storm before the first packet.
12. **Decoder underrun (pull with empty buffer)** — emits PLC,
    counted in `plc_frames`; repeated underruns grow `cur_burst`.
13. **Clock drift** — if the playback clock and packet arrival clock
    drift by more than one frame, caller drops or inserts one slot via
    `gv_dec_reset` or by tolerating one PLC frame; the interface does
    not resample.
14. **Invalid input** — `len == 0` with data, `len > 4000`, NULL
    pointers, or non-960-sample encode calls are rejected with
    `GV_ERR_BAD_ARG`, never partially applied.

> Note on (10): pure Opus DTX sends nothing, so a receiver cannot
> distinguish "sender silent" from "network black hole" from seq
> alone. Recommended: sender emits one 2-byte SID/keepalive packet per
> 400 ms during DTX so gaps are attributable. If keepalives are not
> used, all silent slots are conservatively counted as loss.

## Loss statistics (`GVLossStats`)

- **Counters** — `packets_received/lost/late/duplicate`,
  `fec_recovered`, `plc_frames`, `dtx_frames`: monotonic until
  `gv_dec_reset_stats`.
- **Burst analysis** — `loss_events`, `max_burst`, `cur_burst`,
  `bursts_le3` vs `bursts_gt3`: separates "recoverable, intelligible"
  loss patterns from degraded ones. A healthy game link should show
  `bursts_gt3 == 0`.
- **Derived rates** — `loss_rate = lost / total_slots`;
  `fec_recovery_rate = fec_recovered / (fec_recovered + plc_frames
  within single-loss events)`, i.e. how often redundancy actually
  saved a recoverable slot.

### Acceptance targets
- 3-packet burst: `bursts_le3` increments, output intelligible,
  `plc_frames <= 2` per event (frame 1 covered by FEC).
- Steady 10% random loss: `fec_recovery_rate >= 0.9`,
  `loss_rate` matches injected rate within ±1%.
- Post-recovery continuity: sample index of decoded speech after a
  burst differs from the loss-free reference by 0 samples.
