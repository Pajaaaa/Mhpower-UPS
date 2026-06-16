# MHpower UPS monitor (ESP32)

Monitoring zálohovaných zdrojů **MHpower (MPU‑500 a příbuzné)** pomocí ESP32. Firmware
**pasivně odposlouchává sběrnici displeje (TM1640)**, dekóduje z ní vstupní/výstupní napětí,
stav sítě/baterie, zátěž a alarmy, a publikuje je přes **webové rozhraní** a **SNMP**.
Do zdroje se nijak nezasahuje — jen se „čte přes rameno“ jeho vlastní displej.

> Vstupní/výstupní data čte z displeje, takže přesnost je omezená rozlišením displeje
> (napětí po celých voltech, zátěž a baterie v 6 dílcích). Cílem je spolehlivý přehled
> a alarmy, ne laboratorní přesnost.

---

## Obsah
- [Jak to funguje](#jak-to-funguje)
- [Funkce](#funkce)
- [Hardware a zapojení](#hardware-a-zapojení)
- [Sestavení a nahrání](#sestavení-a-nahrání)
- [První konfigurace](#první-konfigurace)
- [Webové rozhraní](#webové-rozhraní)
- [Výpočet výdrže baterie](#výpočet-výdrže-baterie)
- [SNMP](#snmp)
- [Diagnostika výpadků](#diagnostika-výpadků)
- [Úspora odběru](#úspora-odběru)
- [Struktura repozitáře](#struktura-repozitáře)

---

## Jak to funguje

Displej MHpowru řídí budič **TM1640** po dvouvodičové sériové sběrnici (CLK + DIN).
ESP32 se na tyto dvě linky připojí jako **pasivní posluchač** a v těsné smyčce
(s vypnutými přerušeními) navzorkuje GPIO. Z navzorkovaných hran pak softwarově
zrekonstruuje TM1640 rámec.

**Reálný signál** (ověřeno logickým analyzátorem): hodiny ~500 kHz, jeden rámec trvá
~192 µs a posílá se zhruba **2× za sekundu**.

**Formát rámce** (po dekódování bajtů):

```
0x40  0xC0  [10 bajtů paměti displeje]  0x88|jas
 │     │     └─ mem[0..9]                └─ příkaz jasu (0x8X)
 │     └─ nastav adresu 0
 └─ zápis dat, autoinkrement adresy
```

Mapování paměti displeje na zobrazené hodnoty:

| bajt | význam |
|------|--------|
| `mem[0..2]` | vstupní napětí — 3 číslice (7‑segment) |
| `mem[3..5]` | výstupní napětí — 3 číslice |
| `mem[6]`    | režim / ikony (síť, běh na baterii, přehřátí) |
| `mem[7]`    | úroveň zátěže (0–5 dílků / přetížení) |
| `mem[8]`    | ikony |
| `mem[9]`    | dílky baterie (0–5) |

Dekodér si nevynucuje konkrétní polaritu/pořadí — **brute‑force** vyzkouší varianty
(náběžná/sestupná hrana, LSB/MSB, inverze, fázový posun) a vybere tu, která dá platný
rámec se smysluplnými hodnotami. Díky tomu je odolný vůči záměně CLK/DIN i invertujícímu
děliči.

---

## Funkce

- **Pasivní čtení** displeje MHpower přes TM1640 (žádný zásah do zdroje).
- **Webový dashboard** (auto‑refresh): vstup/výstup, síť/baterie, zátěž, výkon, alarmy,
  výdrž na baterii, kondice baterie.
- **Odhad výdrže** s učením energie na jednotlivé dílky baterie (viz níže).
- **SNMP v1** (UDP/161) — 38 OID pro integraci do monitoringu (Zabbix, LibreNMS, …).
- **OTA aktualizace** firmwaru přes web (s progress barem).
- **Diagnostika běhu** — důvod posledního restartu, uptime, heap, fragmentace, TX výkon,
  takt CPU (kvůli ladění výpadků bez sériáku).
- **Úsporný režim** — snížený WiFi výkon, WiFi modem‑sleep, CPU 160 MHz, vypnutý Bluetooth.

---

## Hardware a zapojení

- **ESP32** (DevKit / WROOM).
- Displej MHpower s budičem **TM1640**.

| TM1640 | → | ESP32 |
|--------|---|-------|
| CLK | přes dělič napětí | **GPIO18** |
| DIN | přes dělič napětí | **GPIO23** |
| GND | přímo | **GND** |

> ⚠️ Linky displeje mohou být na vyšší úrovni než 3,3 V — **použij odporový dělič**, ať
> nezničíš vstupy ESP32. Zem musí být společná.

Schémata zapojení: [`docs/zapojeni_spravne.png`](docs/zapojeni_spravne.png),
[`docs/zapojeni_chip.png`](docs/zapojeni_chip.png).

---

## Sestavení a nahrání

Závislosti: **arduino‑esp32** (testováno na core 2.0.x i 3.x), standardní knihovny
(`WiFi`, `WebServer`, `WiFiUdp`, `Update`, `Preferences`).

### Arduino IDE
1. Otevři `firmware/mhpower_esp32_capture/mhpower_esp32_capture.ino`.
2. Vyber svou ESP32 desku a **Partition Scheme s OTA** (default „4MB with spiffs“ stačí).
3. **Sketch → Export Compiled Binary** → vznikne `…ino.bin` vedle sketche.

### arduino‑cli
```bash
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:esp32 -e firmware/mhpower_esp32_capture
# výsledek: build/esp32.esp32.esp32/mhpower_esp32_capture.ino.bin
```

### Nahrání (OTA)
Ve webu **systém → Firmware a údržba → Nahrát firmware** vyber **aplikační** image
`mhpower_esp32_capture.ino.bin` (ne `.merged.bin`/`.bootloader.bin`/`.partitions.bin`).
Průběh ukáže progress bar; po dokončení se ESP32 sám restartuje.

První nahrání (bez OTA) udělej přes USB klasicky z IDE / `arduino-cli upload`.

---

## První konfigurace

V repo verzi jsou přihlašovací údaje jen **placeholdery** — nastav je před prvním flashem
v `struct AppSettings`:

```cpp
char wifiSsid[33] = "WIFI_SSID";       // tvoje WiFi
char wifiPass[65] = "WIFI_PASSWORD";
char webPass[33]  = "changeme";        // heslo do webu (uživatel "admin")
```

Po připojení už jde vše měnit ve webu (**systém**) a ukládá se do flash (NVS) —
hodnoty v kódu slouží jen jako výchozí při čistém zařízení.

---

## Webové rozhraní

- `/` — **monitor** (dlaždice, auto‑refresh přes `/api/status`).
- `/settings` — **systém**: pojmenování, WiFi, web login, SNMP community, typ zdroje
  (300/500/700/800 W), kapacita a datum instalace baterie, účinnost a klid měniče,
  minimální výdrž, práh kondice; OTA a restart.
- `/api/status` — JSON se všemi hodnotami (chráněno HTTP Basic auth).

Dlaždice **Diagnostika** ukazuje:
`rámců N / heap (min, blok) / err / filtr / cap / tout | běh / reset: … / TX … dBm / CPU … MHz / dílky x/5`.

| pole | význam |
|------|--------|
| `err` | rámce, které selhaly při dekódování |
| `filtr` | rámce zahozené jako nesmyslné (sanity filtr) |
| `cap` / `tout` | úspěšné navzorkování / timeout (sběrnice byla v klidu) |
| `běh` | uptime od startu |
| `reset:` | důvod posledního restartu |
| `dílky x/5` | kolik dílků baterie už má naučenou energii |

---

## Výpočet výdrže baterie

Odhad zbývající výdrže běží ve dvou vrstvách:

**1) Prior (vždy k dispozici)** — z kapacity v Ah:
```
využitelná Wh = Ah × 12 V × 0,85 × kondice
reálný odběr  = výstupní výkon / účinnost měniče + klidová spotřeba měniče
kapacita      = korigovaná Peukertem podle vybíjecího proudu (k = 1,15, C20)
```
plus učení kondice z plných výbojů (EMA) a klouzavý průměr odběru pro stabilní odhad.

**2) Ukotvení na dílky (přesnější, učí se za běhu)** — měnič sám zná reálné napětí, proud
i teplotu a jeho výsledkem jsou **dílky baterie**. Firmware měří, **kolik reálné energie
[Wh] se spotřebuje na každý dílek**, a učí si tabulku Wh/dílek (EMA, ukládá do flash).
Výdrž = (zbytek aktuálního dílku + naučené spodní dílky) / odběr. Peukert i účinnost jsou
v tom měření už přirozeně zahrnuté.

> Bezpečné chování: dokud nejsou naučené **≥ 3 dílky**, jede prior (Ah+Peukert). Tabulka
> se maže při změně kapacity nebo data instalace baterie (= nová baterie).

---

## SNMP

SNMP **v1**, UDP **161**, community dle nastavení (výchozí `public`).
Base OID: **`1.3.6.1.4.1.53864.1.1`**, dotazuj se `…1.1.<index>.0` (GET i GETNEXT/walk).

| idx | typ | hodnota | idx | typ | hodnota |
|----:|-----|---------|----:|-----|---------|
| 1 | str | název zařízení | 20 | int | chyby dekódování |
| 2 | int | online (1/0) | 21 | int | filtrované rámce |
| 3 | int | vstupní napětí [V] | 22 | int | capture timeouty |
| 4 | int | výstupní napětí [V] | 23 | int | typ zdroje [W] |
| 5 | int | frekvence [Hz] | 24 | int | kapacita baterie [Ah×10] |
| 6 | int | síť přítomna (1/0) | 25 | int | kondice baterie [%] |
| 7 | int | běh na baterii (1/0) | 26 | int | běh na baterii [s] |
| 8 | str | stav zdroje | 27 | int | zbývající výdrž [s] |
| 9 | int | alarm (1/0) | 28 | int | poslední běh na baterii [s] |
| 10 | int | přehřátí (1/0) | 29 | str | IP adresa |
| 11 | int | zátěž [%] | 30 | str | syrová paměť displeje (hex) |
| 12 | int | odhad výkonu [W] | 31 | int | nabíjení (1/0) |
| 13 | int | úroveň zátěže (0–5) | 32 | int | kritická baterie (1/0) |
| 14 | int | přetížení (1/0) | 33 | int | nízká baterie (1/0) |
| 15 | int | dílky baterie (0–5) | 34 | int | baterie plná (1/0) |
| 16 | str | stav baterie | 35 | int | odhad napětí baterie [V×10] |
| 17 | int | WiFi RSSI [dBm] | 36 | int | stáří posledního rámce [ms] |
| 18 | int | volný heap [B] | 37 | int | varování kondice (1/0) |
| 19 | int | počet rámců | 38 | int | varování výdrže (1/0) |

Příklad: `snmpwalk -v1 -c public <IP> 1.3.6.1.4.1.53864.1.1`

---

## Diagnostika výpadků

Zařízení nemá sériovou konzoli připojenou trvale, proto se diagnostika ukazuje ve webu.
Klíčové je pole **`reset:`** (z `esp_reset_reason()`):

| hodnota | příčina | co s tím |
|---------|---------|----------|
| `brownout` | propad napájení (proudová špička WiFi) | pevnější zdroj/kondenzátor; nižší TX výkon |
| `task-watchdog` / `int-watchdog` | smyčka nepustila systém | řešeno `delay(1)`/`yield()` ve smyčce |
| `panika/exception` | pád kódu | poslat výpis přes USB sériák |
| `power-on` | normální zapnutí | — |

`běh` (uptime) odliší restart (skočí na 0) od pouhého výpadku WiFi (uptime běží dál).
`minFreeHeap` a `maxAllocHeap` odhalí únik paměti / fragmentaci.

---

## Úspora odběru

Nakonfigurováno v kódu:

- **WiFi TX výkon** snížen na `WIFI_POWER_5dBm` (konstanta `WIFI_TX_POWER` nahoře v souboru) —
  AP bývá hned u ESP, takže to neovlivní dosah, ale sníží proudové špičky i odběr.
- **WiFi modem‑sleep** (`WIFI_PS_MIN_MODEM`) — rádio spí mezi beacony.
- **CPU 160 MHz** místo 240 MHz (`setCpuFrequencyMhz(160)`).
- **Bluetooth vypnutý** (`btStop()`).

Aktuální TX výkon a takt CPU vidíš v dlaždici Diagnostika.

---

## Struktura repozitáře

```
firmware/
  mhpower_esp32_capture/   hlavní firmware (web + SNMP + OTA + baterie + diagnostika)
  mhpower_diag/            samostatný sériový diagnostický skeč sběrnice TM1640
  legacy/mhpower_wemos/    starší varianta (Wemos/ESP8266)
docs/                      schémata zapojení
tools/                     generátory schémat + dekodéry sigrok záznamů (viz tools/README.md)
tools/captures/            ukázkové záznamy sběrnice (.sr) + vysvětlení
```

---

## Nástroje

Viz [`tools/README.md`](tools/README.md) — Python skripty pro dekódování sigrok (`.sr`)
záznamů sběrnice TM1640 a pro generování schémat zapojení.

---

## Autor

Pavel Vlček, hkfree.org. Firmware pro vlastní potřebu monitoringu MHpower zdrojů.
