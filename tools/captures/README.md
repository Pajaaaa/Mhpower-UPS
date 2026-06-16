# Ukázkové záznamy sběrnice (sigrok `.sr`)

Dva reálné záznamy sběrnice TM1640 z ladění — názorně ukazují, na co si dát pozor.

## `zaznam.sr` — 24 MHz (správná vzorkovací frekvence)
- Vzorkováno **24 MHz**, ~2 s.
- Aktivní je **jen jedna linka** (D1, přesně 208 hran/rámec = 104 bitů → samotné hodiny).
- 4 rámce, každý ~192 µs, rozestup přesně **502 ms** (≈ obnova displeje 2×/s).
- Pořízeno **před opravou zapojení** — druhá linka (data) tehdy nešla, proto se nedá
  dekódovat (chybí DIN), ale krásně potvrzuje časování sběrnice.

## `data.sr` — 20 kHz (podvzorkováno)
- Vzorkováno jen **20 kHz**.
- Aktivní jsou **obě linky** (D0=CLK i D1=DIN, bursty padají na stejné indexy) — pořízeno
  **po opravě zapojení**, takže obě linky žijí.
- Jenže 20 kHz je na ~500 kHz sběrnici asi 1000× pomalé: jeden 192µs rámec se navzorkuje
  jen do **2–4 vzorků** → dekódovat z toho nejde.

## Poučení
Pro dekódování sigrok záznamem potřebuješ **vysokou vzorkovací frekvenci (≥ 1–2 MHz)
A obě linky (CLK + DIN)** současně. Firmware na ESP32 tohle obchází tím, že vzorkuje GPIO
sám v těsné smyčce o mnoha MHz.

Vyzkoušej:
```bash
python3 ../sr_analyze.py zaznam.sr   # ukáže, které kanály jsou aktivní
python3 ../sr_timing.py  zaznam.sr   # časování rámců
python3 ../sr_decode.py  data.sr     # pokus o dekódování (zde selže — podvzorkováno)
```
