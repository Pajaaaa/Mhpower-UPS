# Historie vývoje

Jak projekt vznikal — od prvního záznamu logickým analyzátorem až po finální ESP32
firmware se Schmittovým invertorem. Sepsáno zpětně z vývojových konverzací (3.–6. 6. 2026)
a doplněno o milníky z gitu (16.–17. 6. 2026). Reálná hesla/SSID tu nejsou — v repu jsou
jen placeholdery (viz [README → První konfigurace](../README.md#první-konfigurace)).

## Přehled cesty (TL;DR)

```
reverse engineering displeje (logický analyzátor + PulseView)
      │
      ├─ Wemos D1 / ESP8266           → nestíhal rychlý CLK (neúspěch)
      ├─ ATmega2560 (SPI slave)       → čtení šlo na první dobrou ✓
      │     + ESP8266 web na combo desce → padal po pár vteřinách
      │
      └─ ESP32 WROOM (dělá vše)       → finální platforma
            ├─ čtení: interrupt/RMT selhalo → blokový capture + offline dekód ✓
            ├─ web dashboard + filtrace dat + odhad výdrže + kondice baterie
            ├─ SNMP v1 (integrace do power monitoru)
            └─ HW: dělič → 74LVC14A Schmittův invertor (odrušení) ✓
```

Hlavní ponaučení je v sekci [Co se ukázalo](#co-se-ukázalo).

---

## 1. Reverse engineering displeje (3. 6.)

Zdroj **MHpower MPU‑500‑12** má displej řízený budičem **TM1640** (dvouvodičová sériová
sběrnice CLK + DIN, ~500 kHz, bit0 první). Cíl: pasivně odposlechnout sběrnici a vyčítat
stav na dálku, bez zásahu do zdroje.

Postup:
1. Na CLK/DIN připojen **8kanálový logický analyzátor 24 MHz**, záznam v **PulseView (sigrok)**.
2. Data nebyla na `D1` (zůstal v log. 1 — chyba zapojení), ale na **`D2`**; clock na `D0`.
3. Pořízena série záznamů `zdroj.sr` … `zdroj9.sr` pro různé stavy displeje (síť 228/228,
   výpadek 000/230, nabíjení, zátěž, přetížení, vybitá baterie) — vždy s fotkou displeje
   pro porovnání, které bity se mění.

**Dekódovaný rámec TM1640:**

```
0x40  0xC0  [10 bajtů paměti displeje]  0x8B
 │     │                                 └─ příkaz jasu (displej zapnutý, jas ~3)
 │     └─ nastav adresu 00h
 └─ zápis dat s autoinkrementem adresy
```

**Mapa paměti displeje** (`mem[0..9]`):

| bajt | význam |
|------|--------|
| `mem[0..2]` | vstupní (horní) napětí — 3 číslice |
| `mem[3..5]` | výstupní (dolní) napětí — 3 číslice |
| `mem[6]` | režim: síť / běh na baterii / alarm / přehřátí |
| `mem[7]` | úroveň zátěže (0–5 dílků) / přetížení |
| `mem[8]` | pevné ikony (V, Hz) — typicky `0x63` |
| `mem[9]` | dílky baterie (0–5) / animace nabíjení |

**Mapa číslic** (7‑segment, hodnota bajtu → zobrazená číslice), odvozená porovnáním záznamů:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | prázdné |
|---|---|---|---|---|---|---|---|---|---------|
|`0x77`|`0x41`|`0x6E`|`0x6D`|`0x59`|`0x3D`|`0x3F`|`0x61`|`0xFF`|`0x00`|

> Číslici `9` se nepodařilo zachytit (pro napětí 220–230 V není potřeba).

**Režim `mem[6]`:** `0x22` = síť · `0x42` = výpadek / běh na baterii · `0x46` = baterie + alarm /
nízká baterie · `0x43`/`0x83`/`0xC3` = teplotní alarm (přehřátí, doměřeno 5. 6.).

**Zátěž `mem[7]`:** `0x20`=0 · `0x60`=1 · `0x61`=2 · `0x65`=3 · `0x67`=4 · `0x77`=5 dílků ·
`0x7F` = 5 dílků + přetížení (dílky 3 a 4 doměřeny až 5. 6. přes `/api/raw`).

**Baterie `mem[9]`:** `0x10`=0 · `0x30`=1 · `0x70`=2 · `0x71`=3 · `0x75`=4 · `0x77`=5 dílků.
Cyklování `10→30→70→71→75→77` = **animace nabíjení**; stabilní `0x77` = plná.

---

## 2. Volba hardwaru — tři pokusy

### Wemos D1 / ESP8266 — neúspěch

První pokus o pasivní čtení CLK/DIN přímo na ESP8266. Řešilo se převedení úrovní 5 V → 3,3 V
**odporovým děličem** (zkoušeno 10k/22k, debugoval se i poměr a omylem prohozené CLK/DIN
ověřené analyzátorem přímo na pinech Wemosu). Výsledek: ESP8266 přes přerušení ani polling
**nestíhal zachytit celý TM1640 burst** (~104 hran každých 0,5 s) → `frames:0`.

### ATmega2560 (SPI slave) — čtení šlo hned

Klíčový posun: na desce **MEGA+WiFi R3** (ATmega2560 + ESP8266 v jednom) přečetla ATmega
sběrnici přes **hardwarový SPI slave** (`CLK→pin52 SCK`, `DIN→pin51 MOSI`, `SS pin53→GND`) —
fungovalo na první dobrou, protože bajty skládá hardware. ATmega posílala dekódovaný stav
přes Serial. Postupně se přidalo:

- **Kalibrace napětí baterie** podle dílků (při běhu na baterii): `>13 V`=5 dílků, `13 V`=4,
  `12,7 V`=3, `12,3 V`=2, `11,9 V`=1, `11,5 V` = poslední dílek bliká (kritická).
- `batteryPercent`, `loadPercent`, `loadWattsEstimate` (podle zvoleného výkonu zdroje).
- **Odhad výdrže** na baterii a **učení kondice** baterie z výpadků (předchůdce dnešní logiky).

### ESP8266 web na combo desce — nestabilní

ESP8266 mělo číst JSON z ATmegy (přes `Serial3`) a dělat web + SNMP. Web ale **padal po pár
vteřinách** (fragmentace heapu z `String`, zahlcení UART, malá RAM). Zkoušely se: krátký
`MHP,…` CSV formát místo JSON, statické buffery, minimalistické HTML, vypnutý UART — bez
trvalého úspěchu. Padlo rozhodnutí přejít na **ESP32**.

---

## 3. Přechod na ESP32 a boj se čtením (4. 6.)

Cílem bylo nechat **ESP32 WROOM** dělat všechno (čtení displeje + web + SNMP + OTA), bez ATmegy.
Piny: **CLK → GPIO18**, **DIN → GPIO23**. ADC měření baterie se zavrhlo (stav se bere jen z displeje).

Vyčítání bylo nečekaně těžké — postupně se zkoušelo a zamítlo:

- **přerušení na CLK** + okamžitý `digitalRead(DIN)` — ztrácelo hrany / špatné zarovnání,
- **RMT** jako logický analyzátor — dva nezávislé RMT RX kanály (CLK a DIN) **nemají společnou
  časovou nulu**, takže se data nedala spolehlivě zarovnat,
- detekce **START/STOP** (styl TM1637) — na tomto displeji generovala falešné starty,
- automatické zkoušení až **64 variant** (hrana × pořadí bitů × inverze × fázový posun).

**Průlom:** místo honění hran v reálném čase ESP32 **navzorkuje celý burst do bufferu a
dekóduje ho offline** (jako analyzátor uvnitř čipu), s brute‑force výběrem správné varianty.
Tato „known good" verze se uložila jako referenční základ.

---

## 4. Web, filtrace dat a kompletace mapy (5. 6.)

Nad ověřené čtení se vrátil **web dashboard** (heslo do webu, OTA přes `/update`) a doladilo se:

- **Filtrace dat** — vadné rámce se zahazují (čítač `filtered`), přepnutí síť/baterie se
  potvrzuje až po několika shodných rámcích, drží se poslední dobré hodnoty (proti glitchům).
- **Detekce nabíjení** podle běžících kostiček; později zpřesněno na pravidlo *síť + baterie
  není plná (`0x77`) → nabíjení* (animace jen jako potvrzení).
- **Kritická baterie** = skutečné blikání posledního dílku (`0x10`↔`0x30`) → `0 %`.
- **Doměření mapy zátěže** (dílky 3/4/5 a přetížení) a **teplotního alarmu** přes diagnostický
  `/api/raw`, kdy uživatel měnil displej a firmware četl syrové bajty.
- UI/identita: pojmenování zdroje (na dashboardu i v SNMP), **dynamický titulek** podle typu
  zdroje (300/500/700/800 W), editovatelný web login, stránka nastavení přejmenovaná na
  **„systém"**, footer `Pavel Vlcek v1.0 hkfree.org`.

---

## 5. SNMP a integrace do power monitoru (5.–6. 6.)

Přidán vlastní **SNMP v1 responder** (UDP/161, community v nastavení, výchozí `public`) bez
externí knihovny. Base OID **`1.3.6.1.4.1.53864.1.1`** (privátní enterprise), hodnoty z dashboardu
jako jednotlivé indexy (kompletní tabulka je v [README → SNMP](../README.md#snmp)).

Monitoring obstarává samostatný **power monitor** (projekt *edgepower*). Při ladění se opravilo:

- ESP odpovídá na **`…<idx>`** i na standardní scalar **`…<idx>.0`** (power monitor čeká `.0`)
  a vrací **stejný OID**, na který se ptal,
- v jedné smyčce se **odbaví víc čekajících UDP paketů** (proti timeoutům při dotazu na víc OID),
- **rebinding UDP/161** každých 30 s (řešilo „web žije, SNMP mlčí").

> Otevřené: hromadný GET na více OID v jednom paketu vrací jen jeden varbind — power monitor
> proto čte MHpower OID **po jednom**.

---

## 6. Odstranění rušení — Schmittův trigger (5.–16. 6.)

Při **nabíjení** rušil toroid/měnič dlouhé vodiče CLK/DIN → data se sekala (`DATA OFF`),
i když ESP signál fyzicky vidělo. Dělič (zkoušen 10k/22k → 6,8k/5,56k) to nevyřešil.

Finální HW oprava: oddělovací **Schmittův invertor `74LVC14A` / `SN74LVC14ADR`** napájený z
**3,3 V** (vstupy 5V‑tolerantní), `1k` na vstupu a `100R` na výstupu, blokovací `100 nF`.
Hystereze vyčistí zašuměné hrany a zároveň udělá level‑shift 5 V → 3,3 V. Děliče se vyhodily.
Detaily zapojení jsou v [README → Hardware a zapojení](../README.md#hardware-a-zapojení).

> Signály vyjdou z invertoru **invertované** — nevadí, firmware si polaritu (edge + invert)
> při dekódování najde sám, takže stačí **jedno hradlo** na linku.

---

## 7. Verze a údržba (16.–17. 6.)

Od tohoto bodu už vede historii **git**. Stručný přehled commitů:

| commit | datum | co přibylo |
|--------|-------|------------|
| `dbe0466` | 16. 6. | Initial commit — firmware, dokumentace, nástroje |
| `21c0bf6` | 16. 6. | dokumentace zapojení přes 74LVC14A (ne dělič) |
| `47cf098` | 16. 6. | stabilita, úspora odběru, mDNS, log událostí, rozšíření SNMP, úklid |
| `e0b4866` | 16. 6. | NTP server editovatelný v nastavení |
| `7696b14` | 16. 6. | účinnost a klid měniče napevno v kódu (pryč z nastavení) |
| `c4430f1` | 16. 6. | podlimitní kondice/výdrž se zapíše do logu událostí |
| `4524c7c` | 16. 6. | `CAPTURE_SAMPLES` zpět na 18000 (10000 ořezávalo rámec → err ~25 %) |
| `813f9a6` | 16. 6. | odstraněn mDNS (zmenšení binárky kvůli OTA) |
| `8075099` | 17. 6. | mDNS zpět + záchranný AP hotspot při nepřipojené WiFi |
| `046c7e5` | 17. 6. | info o záchranném hotspotu na stránce systém |
| `634c6c7` | 17. 6. | **bezpečnost OTA** + opravy (auth, XSS, WDT, mDNS, baterie, pojistka) |
| `6b889b8` | 17. 6. | oprava falešného „nabíjení" z glitche dekódu ikony baterie |
| `c1c4a54` | 17. 6. | **v1.4** (footer) |
| `6eaaaeb` | 17. 6. | pojistka proti zaseknutému web serveru (`maintainWeb`) + **v1.5** |
| `e2db484` | 17. 6. | OTA přežije odmítnutý/vadný upload (WDT fix) + **v1.6** |
| `c53f99a` | 17. 6. | HW ID desky (MAC) na web + SNMP idx 45 — odlišení kusů v racku + **v1.7** |
| `090063d` | 18. 6. | debounce alarmu/přetížení proti glitchům jednoho rámce (`confirmFlag`) — konec falešných „summary_error" v monitoringu + **v1.8** |
| `8ec3672` | 18. 6. | odchyt neznámých číslic displeje přes `/api/digitscan` (zatím chybí segmentový vzor „9") — záchyt běží před filtrem napětí, zapíše jen 1 neznámou mezi 2 platnými + **v1.9** |

Dvě provozní věci z tohoto období:

- **Velikost binárky** přerostla OTA slot → přechod na partition schéma **Minimal SPIFFS**
  a první nahrání přes USB (`merged.bin` od adresy `0x0`); další update už zase OTA aplikačním
  `…ino.bin`. Pozor na rozdíl app‑only vs. merged image — viz [README → Sestavení a nahrání](../README.md#sestavení-a-nahrání).
- **Brownout** (restarty z propadu napájení, ne SW/WiFi): potvrzeno z `reset:` diagnostiky.
  Snížení odběru (TX 5 dBm, modem‑sleep, 160 MHz, BT off) nestačilo → vyřešeno **HW**: bulk
  kondenzátor na napájení + pevný 5V zdroj. Po přidání kondíku už `reset:` není `brownout`.

---

## Co se ukázalo

- **ESP8266 je na rychlý TM1640 clock slabé.** ESP32 je správná platforma — ale ne přes
  „chytré" přerušení/RMT, nýbrž přes **blokový capture celého burstu + offline dekód**, jak to
  dělal logický analyzátor a SPI slave na ATmeze. Jednoduchý a robustní princip vyhrál.
- **Dělič nestačí na rušení.** Při nabíjení dělá toroid/měnič bordel; finální fix je
  **Schmittův invertor**, ne software.
- **OTA chce aplikační `…ino.bin`**, ne `merged.bin`. Když binárka přeroste slot, řeší to
  **Minimal SPIFFS** + USB flash.
- **Brownout je hardwarový problém** — řeší ho kondenzátor a pevný zdroj, ne ladění firmwaru.
- **Stav z displeje je hrubý** (napětí po voltech, baterie/zátěž po dílcích) — proto se data
  **filtrují, debouncují a drží poslední dobrý stav**; cílem je spolehlivý dohled, ne přesnost.

---

## Zdroj

Sepsáno z exportu vývojových konverzací (Codex, 3.–17. 6. 2026) a z git logu tohoto repozitáře.
Surový export obsahuje i reálné údaje, **proto není součástí repozitáře** — tato historie je
jeho destilát bez citlivých hodnot.
