# TX power target: register 0x0646

How the per-core value in `PHY 0x0646[7:0]` (and `0x0846`, `0x0a46`) is
derived, what the sweep pins down, and what is still open.

Everything here comes from the captures, from the boards' NVRAM, and from the
GPL `brcmsmac` sources in `drivers/net/wireless/broadcom/brcm80211`. The
vendor object is used only where `PROVENANCE.md` already allows it: to read
data tables. No logic is taken from it.

## What the register is

Not a "max index", which is what the port used to call it. The vendor names
the function that writes it `set_target`, and it takes the value as an
argument: it is the **TX power target** in quarter-dBm. The ceiling lives
elsewhere, at `0xb46`, written by the power-control enable path.

## The chain

`brcmsmac` already carries the computation, as
`wlc_phy_txpower_recalc_target()` in `phy/phy_cmn.c`:

```c
wlc_phy_txpower_sromlimit(pi, target_chan, &mintxpwr, &maxtxpwr, rate);
maxtxpwr = min(maxtxpwr, pi->txpwr_limit[rate]);       /* regulatory */
maxtxpwr = (maxtxpwr > pactrl) ? maxtxpwr - pactrl : 0;
maxtxpwr = (maxtxpwr > 6)     ? maxtxpwr - 6     : 0;  /* the margin */
maxtxpwr = min(maxtxpwr, tx_pwr_target[rate]);         /* user target */
tx_pwr_target[rate] = max(maxtxpwr, mintxpwr);         /* floor */
```

and the per-rate SROM limit, as `wlc_phy_txpwr_nphy_srom_convert()` in
`phy/phy_n.c`:

```c
srom_max[rate] = tmp_max_pwr - 2 * nibble(rate);
```

The reduction to a single per-core number, and where the per-rate detail goes:

```c
pi->tx_power_max = max over rates;                       /* -> 0x0646 */
pi->tx_power_offset[rate] = tx_power_max - target[rate];  /* -> table 0x21 */
```

Table 0x21 is the array this driver calls `ppr[24]`. That split explains an
otherwise odd pair of facts measured on the sweep: `ppr` is identical across
all 52 segments and all three widths, while `0x0646` moves with the channel.
Changing channel moves the maximum, not the spacing between rates.

## Three unit scales, and they are easy to conflate

| quantity | unit | source |
| --- | --- | --- |
| `maxp5ga[]`, `0x0646` | quarter-dBm | SROM |
| `mcsbw*po` nibbles | half-dB | SROM, hence the `2 *` |
| `ch->max_power`, locale tables | whole dBm | regulatory, hence `QDB()` = `* 4` |

The `-6` margin is in quarter-dBm and is a saturating subtraction, not a plain
one: `(x > 6) ? x - 6 : 0`. Reading it as a constant margin is what made the
old `maxp5ga[grp] - 6` look like it worked.

## What the port computes now

```
grp  = txpwr_subband(primary channel, width)   /* 5210 at 20/80 MHz, 5250 at 40 */
band = po_band(primary channel)                /* l < 52, m < 100, h */
po   = (width == 40) ? mcsbw40_5g[band] : mcsbw20_5g[band]
nib  = min over the eight nibbles of po
lim  = maxp5ga[core][grp] - 2 * nib
lim  = min(lim, QDB(max_power) - antenna_gain)  /* per 20 MHz sub-channel */
target = lim - 6
```

with one fitted correction of `-2` for the first 40 MHz block of a sub-band
whose `maxp5ga` entry differs from the next one's. See below.

Exact on all 26 sweep configurations plus the agcombo captures.

Two points worth keeping:

- **80 MHz uses the 20 MHz offsets.** The maximum is over every rate, and the
  20 MHz rates stay populated whatever the operating width, so they carry the
  smallest nibble and win. The sweep's 80 MHz configurations agree with the
  20 MHz ones channel for channel.
- **The sub-band boundary is width-dependent**, 5210 at 20 and 80 MHz and 5250
  at 40, and this is settled by a sign argument rather than by fitting. A
  regulatory limit can only lower a ceiling, so any residual where the driver
  comes out *below* the vendor cannot come from the regulatory stage. At
  40 MHz the 5210 boundary leaves ch44 two units low, which nothing downstream
  could raise; 5250 leaves ch36 and ch52 two units high, which a missing clamp
  explains.

`b43_phy_ac_pa5g_group()` keeps its own 5250 boundary for every width, because
it feeds the `pa5ga` coefficients. The two partitions coincide at 40 MHz and
differ at 20; they are kept separate rather than one being bent to fit.

## The open point: two 40 MHz configurations

`ch36` and `ch52` at 40 MHz come out two quarter-dB high. The residual is
regular: the **first** 40 MHz block of each sub-band is high and the second is
exact, which is a function of the block's ordinal position, not of any
frequency.

Both are on the hazardous side. Coming out below the vendor costs range;
coming out above drives the PA harder than the board was characterised for.
That is why no 40 MHz entry is in the validated list, and why the correction
only ever lowers.

### Candidates excluded, each with an argument

| candidate | why not |
| --- | --- |
| the `corr` term at `+0x11cc` | its step is `* 4`; the residual is 2 |
| `bw405g*po` | does not exist in SROM rev 11 — rev 11 replaced base-plus-delta with a full array per width |
| `sb20in40`, `sb40and80` | zero on all three boards, so they cannot shift anything |
| a fixed nibble index | the three binding cases need indices 6, 7 and 0 |
| `min` over nibbles | always 0 in the bands concerned |
| regulatory ceiling | per frequency range; all of U-NII-1 shares one limit |
| `maxpwr40[]` from the locale table | indexed by `CHANNEL_POWER_IDX_5G`, and ch36 and ch44 share index 0 |
| OFDM/MCS cross-limiting | operates on arrays that are per band: ch36 and ch44 share one `srom_max` |
| the vendor CLM | has per-channel granularity and uses it, but the residual is ordinal, not frequency-keyed |
| `ppr` as a witness | identical across all widths, so it carries no information about the per-rate SROM limits |

### What the correction is, honestly

The predicate — lowest 40 MHz block of a sub-band whose `maxp5ga` entry
differs from the next one's — is fitted on two points, and was written after
an unconditional correction was seen to break agcombo. So agcombo does not
confirm it: any predicate false there and true on the d6220's ch36 and ch52
would score identically, and there is one agcombo observation at 40 MHz.

It is also not physically motivated. The grp0/grp1 boundary is at 5250 MHz,
and it is ch44's block that touches it, 5210 to 5250, while ch36's sits well
inside at 5170 to 5210. A "block spills into the neighbouring sub-band"
mechanism would fire on ch44 — the configuration the derivation already gets
right.

### What would settle it

A capture at 40 MHz on **every** channel. The sweep has 7 of the 16 possible,
which is why ordinal and frequency dependence can only be separated by
elimination. With the full set the distinction is direct.

## What the CLM established, and what it did not

The vendor object carries a `CLM DATA 9.6.6` blob: `locales_5g_base` 25 KB,
`locales_5g_ht` 100 KB, `country_definitions` 29 KB. Its
`channel_ranges_20m` table resolves range ids to channel spans, and the 5 GHz
locale tables reference single-channel spans heavily — `36..36` 1350 times,
`44..44` 1093, `40..48` 2080.

So per-channel regulatory granularity exists in the vendor data and is used.
That was read too quickly at first as "the mechanism is regulatory": that the
CLM *can* distinguish ch36 from ch44 does not mean the value we see *comes*
from there, and the ordinal shape of the residual says it does not.

None of that data is needed in the driver. cfg80211 already supplies
`max_power` per channel, which is the same granularity, and the port now looks
it up per 20 MHz sub-channel and takes the minimum over the block — a bonded
block is bounded by its lowest channel, not by its primary.

On the captures this stage does not bind: ch100 receives 86 where a 21 dBm
ceiling would give 84. So the op-for-op match holds only under a regulatory
domain at least as permissive as the one the captures were taken under. That
is a property of the system, not of the driver, but it was not a condition on
the comparison before and now it is.

## The idle-TSSI base index: a latent bug the gate cannot see

Register 0x0645 bits 9:0 carry the per-core idle-TSSI base index. The port
derives it as `read(0x0012) >> 2`, and the shift is right. What is wrong is
*which* reading it shifts.

The vendor samples 0x0012 repeatedly during the measurement and writes the
**mean**. The sweep makes the difference visible only outside 20 MHz:

| configuration | samples in the first block | mean >> 2 | vendor writes |
| --- | --- | --- | --- |
| ch36 at 20 MHz | 1 | 0x206 | 0x206 |
| ch140 at 20 MHz | 1 | 0x208 | 0x208 |
| ch36 at 40 MHz | 1 | 0x20d | 0x20d |
| ch36 at 80 MHz | **256** | 0x208 | 0x208 |
| ch52 at 80 MHz | **256** | 0x209 | 0x209 |
| ch100 at 80 MHz | **256** | 0x209 | 0x208 |

At 20 and 40 MHz the block holds exactly one sample, so the mean and the first
reading coincide and `>> 2` is indistinguishable from the truth. At 80 MHz
there are 256 samples spanning 0x200 to 0x217, and taking the first gives an
arbitrary one of them.

Two further details the captures settle:

- the value is computed **once** and rewritten for iterations 2 and 3. Their
  own sample blocks average to 0x204 while all three writes carry 0x208, so
  the vendor does not recompute per iteration. The port does, which is why it
  emits three different numbers where the vendor emits one three times.
- the sample count is **not** configured by a register. The setup immediately
  before the block -- 0x093a, 0x0925, 0x0739, then 0x0394 = 0x0110 and
  0x0393 = 0x8000 to start -- is byte-identical between 20 and 80 MHz. So the
  count lives in the driver's loop, and porting it means hardcoding 1 or 256
  by width, with nothing in the data explaining the jump.

That last point is why the fix is not in yet: replacing one transcribed
constant with another is not progress, and the count wants an explanation
first.

## Why the read-perturbation test could not find it

`test/consumed_reads.sh` perturbs the oracle's value for one address and asks
whether the emitted trace changes. It reported 0x0012 as consumed, correctly,
and would have reported any dependent write as tracking it. It could not
report this bug, because at 20 MHz -- the only width the gate covers -- the
first reading *is* the mean. The two mechanisms are indistinguishable under
the coverage the test runs at.

That is the fourth way the test produces a false negative, and the worst,
because it is not a matter of sensitivity:

1. **the read's own trace line** carries the value, so a naive diff shows every
   read as consumed; the perturbed address has to be filtered out.
2. **a one-bit flip can be masked away.** 0x0012 is consumed as `>> 2`, which
   discards exactly the bit a 0x0001 flip touches. Several masks are needed.
3. **magnitude.** The RX-IQ accumulators at 0x06c0 and up are sums over 0x4000
   samples feeding a rounded quotient; only a 0xffff perturbation moves the
   output. Reported as discarded until then.
4. **coverage.** Two mechanisms that agree on the configurations the gate runs
   are indistinguishable by construction, whatever the perturbation.

And a category the test conflates with a real bug: a read consumed only on a
branch that is never taken. 0x06a0 and 0x06a1 feed `rx_iq_comp_update`'s
give-up path, which needs the measured power below B43_PHY_AC_MIN_RXIQ_PWR --
never true in any capture.

## Measurement traps hit while doing this

Three times a wrong measurement produced a confident wrong verdict. On a
repository whose criterion is an op-for-op match, the tooling needs the same
scrutiny as the driver.

- `check_channeltab.py` reported 16 mismatches on radio `0x065e` by taking the
  first write in the segment. The vendor writes that register twice, `0x0ff4`
  from the prefregs block and `0x0000` in channel setup. Fixed by locating the
  channel-setup burst by its ordered register signature.
- Adding `b43_actab_fill_r11()` without adding it to the harness `--wrap` list
  dropped 448 `TBL.WR` labels per channel, taking gate 1 from 100% to 98.21%
  and gate 2 from 2 mismatches to 15827. A new driver helper needs a line in
  the harness or the gate lies loudly.
- Dropping the `mask=0x00ff` filter from a verification grep read
  `RAD.WR 0x0646` instead, scoring 0/26 against correct code.

## Which capture can refute what

The three capture sets isolate two axes, and using the wrong one costs time:

| board | chip | driver |
| --- | --- | --- |
| d6220 | 4352 | 7.14.89.14 |
| DSL-3580L | 4352 | 6.30.102.7 |
| agcombo | 4360 | 7.14.43.21 |

d6220 against DSL isolates the **driver version** at constant chip; d6220
against agcombo isolates the **chip** at constant version. The port
reconstructs 7.14, so the DSL cannot refute a 7.14 model — it testifies about
a different algorithm, and it writes the cores in the opposite order. It is
the right capture for telling a version fork from a hardware fact and the
wrong one for anything else.
