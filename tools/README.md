# Nástroje

Pomocné Python skripty (Python 3). Slouží k analýze záznamů sběrnice a generování schémat.

## Dekódování sběrnice TM1640 ze sigrok záznamů (`.sr`)

Soubory `.sr` jsou záznamy z **sigrok / PulseView** (ZIP s `metadata` a `logic-1-*`).
Skripty čekají, že obě linky (CLK, DIN) jsou na prvních dvou kanálech (D0/D1).

| skript | co dělá |
|--------|---------|
| `sr_analyze.py <soubor.sr>` | spočítá počet hran a „high %“ na každém kanálu → pozná živé/mrtvé/kmitající linky |
| `sr_timing.py <soubor.sr>` | shlukne hrany do „burstů“ a ukáže jejich délku a rozestupy (perioda rámce) |
| `sr_decode.py <soubor.sr>` | **brute‑force dekodér** — vyzkouší hrana/pořadí/inverze, najde TM1640 rámec a vypíše vstupní/výstupní napětí, režim a dílky baterie |

```bash
python3 sr_decode.py captures/zaznam.sr
```

> Aby šel záznam dekódovat, musí být **vzorkovací frekvence dostatečně vysoká** (sběrnice
> má ~500 kHz hodiny) — doporučeno ≥ 1–2 MHz. Záznam na 20 kHz je silně podvzorkovaný a
> dekódovat nejde (viz `captures/README.md`).

Stejnou logiku používá i firmware, jen vzorkuje GPIO sám v reálném čase.
Pro rychlou diagnostiku přímo na ESP32 (bez analyzátoru) slouží
`firmware/mhpower_diag/` — vypisuje stav linek a syrové bajty na sériovou linku (115200).

## Generátory schémat

| skript | co dělá |
|--------|---------|
| `draw_wiring.py` | vykreslí schéma zapojení ESP32 ↔ displej (→ `docs/zapojeni_spravne.png`) |
| `draw_chip.py` | vykreslí detail pinů budiče (→ `docs/zapojeni_chip.png`) |

Vyžadují `matplotlib` (`pip install matplotlib`).
