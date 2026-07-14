# RETRACE-TODO — ri-ancoraggio intervalli trace per funzione

Il trace attach d6220 originale è stato rimosso. Le intestazioni per-funzione
citano solo i testimoni vivi: **agcombo 4360** e **down-to-bss-up**. Gli
intervalli d6220 vanno ri-ancorati contro le 4 catture nuove: `...-ch36`,
`...-ch44`, `...-ch36-40`, `...-vht`.

Metodo: fingerprint di sequenza (`localize_functions.py`); l'intervallo
agcombo è il riferimento noto. Localizzare in ch36 (canonica), poi
verificare i delta su ch44/ch36-40/vht (`decorrelation-ch-vs-bw.csv`).

| file | funzione | agcombo 4360 | re-trace |
|---|---|---|---|
| phy_ac.c:129 | `mode_init` | 16-354 | [ ] |
| phy_ac.c:534 | `txpwrctrl_setup` | 10004-10013 | [ ] |
| phy_ac.c:887 | `classifier` | 4091, 9987 | [ ] |
| phy_ac.c:909 | `clip_det` | 4096-4098, 9992-9994 | [ ] |
| phy_ac.c:940 | `rxcore_setstate` | 9032-9033 | [ ] |
| phy_ac.c:1065 | `set_regtbl_on_femctrl` | 4287-4388 | [ ] |
| phy_ac.c:1118 | `set_analog_tx_lpf` | 4403-4535 | [ ] |
| phy_ac.c:1198 | `set_pdet_on_reset` | 310-320, 4266-4279 | [ ] |
| phy_ac.c:1217 | `analog_on_reset` | 4287-5163 | [ ] |
| phy_ac.c:1328 | `rfseq_tbl_init` | 5321-5805 | [ ] |
| phy_ac.c:1364 | `set_reg_on_reset` | 4186-4205 | [ ] |
| phy_ac.c:1421 | `channel_setup` | 6310-6315 | [ ] |
| phy_ac.c:1504 | `coeff_bank_init` | TODO | [ ] |
| phy_ac.c:1597 | `chan_tables` | TODO | [ ] |
| phy_ac.c:1669 | `rxgainctrl_regs` | TODO | [ ] |
| phy_ac.c:1712 | `adc_reset` | 8831-8969 | [ ] |
| phy_ac.c:1857 | `set_channel` | 5810-6348 | [ ] |
| phy_ac.c:2020 | `op_software_rfkill` | 16-354 | [ ] |
| radio_2069.c:865 | `channel_setup` | 4112-4177 | [ ] |
| radio_2069.c:1041 | `rccal` | 174-258 | [ ] |
| radio_2069.c:1103 | `afecal` | 8784-8829 | [ ] |
| radio_2069.c:1221 | `init` | 86-97 | [ ] |
| radio_2069.c:1304 | `pwron` | 148-172 | [ ] |
