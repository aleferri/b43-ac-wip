# Census of discarded reads

The port emits the stock driver's reads to keep the trace aligned. At some
sites the value read goes into a `dummy`, a `discard` or a log with no
consumer, and the write that follows carries a constant taken from `cold01`.
On the D6220 at ch36 the constant is right by construction; on another board,
channel or run it is an invented value. This file says how such sites are
found and which ones remain.

## Method

Two tools, cross-checked.

**`reverse-tools/reads.py consumers`** reads the folded vendor capture and, for
each read, looks in the next 600 operations for the write that consumes its
value:

| relation | meaning |
|---|---|
| **copy** | the same value written elsewhere |
| **rmw(=)** | the same cell rewritten unchanged (save/restore) |
| **rmw(+k)**, **rmw(xor k)** | the same cell rewritten with an additive or bit relation |
| **poll** | a re-read with no write in between |
| **unused** | none of the above |

Trivial values (0, 1, `0xffff`) are not used as tracers.

**`test/unit/read_perturb.py`** runs the cold flow with one read at a time
perturbed in the oracle, table cells included (`AC_READ_PERTURB_KIND=tbl:<id>`).
It then checks whether the emitted operations change: **CONSUMED** if they do,
**DISCARDED** if not.

- One mask is not enough. A bit the consumer drops is invisible, so the default
  runs sixteen masks, one per bit.
- Fewer masks miss real consumers. Bit 11 alone decides `PHY 0x0140`, which the
  port consumes as `(read & 0x0800) | 0x05f4`.

To redo the census:

```sh
GATE_TMP=/tmp/gate test/unit/gates.sh            # keeps /tmp/gate/merged
python3 reverse-tools/reads.py consumers /tmp/gate/merged > /tmp/census.txt
python3 test/unit/read_perturb.py /tmp/census.txt /tmp/gate/merged | sort
```

`GATE_TMP` is rewritten on every run. An audit made after running the gate on
another segment reads that segment's files.

### Two limits of the perturbation test

- **It works per address, not per site.** A register consumed by one function
  and discarded by another shows as CONSUMED, and the second site stays
  invisible. To find such sites, count reads per function with
  `AC_FN_MARKERS=1`.
- **A fully rewritten field cannot be perturbed.** If the stock driver's
  read-modify-write covers every bit of the word read, no mask reaches the
  output, and the read stays DISCARDED by construction. The high word of the
  TX-LPF (`0x07[0x0362-0x037a]`) is such a case, and the code does consume it.

## Families already closed

Each of these keeps the value read in named state, and writes from that state
instead of from a constant.

- **bbmult.** The table `0x20` entries and the IQLOCAL `0x63/0x67/0x6b` cells
  are held as `bbmult_cal[core]`, `bbmult_meas` and `bbmult_saved[core]`.
- **Per-core gain registers `0x0720-0x0747`.**
  - `idle_tssi_meas()`, `rxgain_perchan_tail()` and `b43_radio_2069_afecal()`
    save and restore them locally.
  - `rx_gain_regs_program()` saves 14 registers per core, and tempsense restores
    them (`rxgain_saved`).
  - `rxgain_config_readback()` saves 26 per core plus three RFSEQ rows, and
    `rxiqcal_teardown_apply_defaults()` restores them (`rxgain_cfg_saved`,
    `rfseq_gain_saved`).
- **RFSEQ, table `0x07`.** Closed with the family above. The TX-LPF high words
  and the DACBUF stage 8 are read-modify-writes that keep the prior contents.
- **Cross-core copies and broadcast aliases.** `0x06d8 → 0x16d8`,
  `0x0601 → 0x1601` and `0x?dc` are copies. The others are coincidences of value.
- **Radio AFE cal.** `RAD 0x0122` (+stride) is armed with `read | 0x000f` and
  restored.

## What remains

**Uncertain relation.**

- PHY `0x0393` `xor 0x8000` and `0x0394` `+0x105/+0x106`: the per-index power
  registers. The additive relation may be a coincidence between small values;
  the port writes constants.
- PHY `0x0550` rewritten `xor 0x0dd1` once, save/restore once.
- RAD `0x0020/0x0022/0x003a` and their `0x02xx`/`0x04xx` strides: one
  save/restore each, only on the long segments.
- Read-modify-writes the harness cannot perturb, because they are `PHY.MOD` in
  the port. They need a look per site: PHY `0x0070` `mod 0xe000`, `0x016c`
  `mod 0x0040`, `0x03a9` `mod 0x007f`, `0x040f` `mod 0x0200`, and
  `0x0678/0x0878/0x0a78` `mod 0x0001`.

**Shared memory of the MAC**, outside the PHY but of the same kind:

- `OBJ 0x0314 → 0x00cc` (value `0x0045`);
- the `0x030a/0x030e/0x0310/0x0312` save/restore;
- `OBJ 0x0a3a/0x0a56/0x0a72/0x0a8e`. Here the table is genuinely transcribed
  from the D6220 in `b43_phy_ac_cck_rate_po()`, and declared as such;
- `OBJ 0x0782 → 0x0048`.

## Order of work: which constants are already wrong elsewhere

**Variance.** A discarded read is harmless while the constant the port writes
is the same on every configuration. The first filter is therefore the variance
of the value the stock driver reads, over the 43 cold segments: of 57 discarded
`PHY`/`RAD`/`OBJ` keys, 32 are constant and 25 vary.

**The port's site.** Variance alone gives false positives, so the second filter
is the port's site. Three cases look the same to the variance filter:

- the port writes a **constant** where the stock driver writes a function of the
  read. This is the real debt;
- the port writes a value **derived** from SROM, width or channel, and the
  stock read is a peek before its own computed write. There is nothing to close;
- the following write is a `MOD`, which on hardware keeps the untouched bits.
  The read has no consumer.

**Values that vary because they are measurements or polls** are correctly
discarded: `OBJ 0x0308/0x030c` (noise sample), `PHY 0x0013`, `PHY 0x0380`,
`PHY 0x0270`, and `OBJ 0x0000/0x0002` (the `b43_validate_chipaccess`
self-test).

**Constant on all 43.** These are fragile per board, not per channel, and sit
at the bottom of the queue:

- `OBJ 0x0040`=2, `OBJ 0x024e/0x026e/0x028e/0x02ae`=0,
  `OBJ 0x030a/0x030e/0x0310/0x0312`=0, `OBJ 0x0314`=`0x45`;
- `PHY 0x016c`=0, `PHY 0x0394`=`0x0b`, `PHY 0x03a9`=`0x100`, `PHY 0x040f`=`0x9ff`,
  `PHY 0x0416`=1, `PHY 0x0417`=0, `PHY 0x0678/0x0878/0x0a78`=8,
  `PHY 0x06dd/0x08dd/0x0add`=`0x604`, `PHY 0x0b3e`=0;
- `RAD 0x0020/0x0022/0x003a` and their `0x02xx` strides = 0, `RAD 0x0144/0x0344`=3,
  `RAD 0x090b`=`0x100`.

Per-table variance is read with `reads.py risk`, which indexes by `id[off]`.

## Closing one

The work is the same for every family:

1. Keep the value read in state with a name.
2. Write from it — copy, save/restore or sum — instead of the constant.

The `cold01` gate does not move by construction. The proof is the perturbation
test going from DISCARDED to CONSUMED, and the hot segments, where `cold01`'s
constants are already wrong.
