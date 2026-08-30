# MHpower UPS monitor

## Popis

IoT monitoring zálohovaných zdrojů **MHpower MPU** (300–5000 W) používaných v síti hkfree. Tyto zdroje nemají žádné management rozhraní — jediný výstup je lokální LED displej. Zařízení je postaveno na ESP32, které **pasivně odposlouchává sběrnici displeje (řadič TM1640)**, z rámců dekóduje vstupní/výstupní napětí, stav sítě/baterie, zátěž a alarmy, a publikuje je přes **webové rozhraní** a **SNMP v1**. Do zdroje se nijak nezasahuje — jen se „čte přes rameno" jeho vlastní displej.

Stav všech jednotek se centrálně sbírá do power monitoru spolku (power.hkfree.org), včetně trvalého logu událostí (výpadky sítě, poruchy, restarty).

## Cíl projektu

Trvalý dohled nad záložními zdroji MHpower v síti bez zásahu do zařízení: včasná informace o výpadku napájení, stavu a zdraví baterie, přetížení a poruchách — místo zjištění „až když lokalita spadne". Součástí je odhad zbývající výdrže baterie (Peukertův model + učení z reálného provozu), a to i proaktivně na síti („kdyby teď vypadl proud").

## Přínos pro komunitu

- Monitoring a observabilita páteřní infrastruktury (záložní napájení lokalit)
- Náhrada proprietárního řešení — zdroje management nemají, tímto ho získávají
- Open-source přínos: kompletní reverse engineering protokolu TM1640 displeje MPU, zdokumentovaný a přenositelný
- Sdílení know-how (pasivní odposlech sběrnice, dekódování 7segmentových glyfů, odhad výdrže baterie)
- Energetická efektivita: úsporný režim ESP minimalizuje odběr jednotky

## Rozsah projektu

Projekt ve čtyřech etapách (1. etapa odpovídá limitu ~40 čh dle směrnice):

1. **Proof-of-concept** — odposlech a dekódování TM1640 rámců na jednom kuse, ověření logickým analyzátorem, HW zapojení přes Schmittův invertor 74LVC14A
2. **Firmware pro provoz** — web dashboard, `/api/status`, SNMP v1, OTA aktualizace, odhad výdrže, diagnostika restartů, read-only účet guest
3. **Integrace do power monitoru** — SNMP profil, trvalý log událostí, zobrazení odhadu výdrže
4. **Nasazení na flotilu** — 5 jednotek (Kuklenska, Piletice, Divišova, Čeperka 1, Lipky); kde chybí ethernet, doplněna WiFi brána MikroTik mAP lite s dst-nat

## Odhad pracnosti

~150 člověkohodin celkem:

| Činnost | Odhad |
|---|---|
| Etapa 1 — PoC (odposlech, dekódování, HW zapojení) | ~35 čh |
| Etapa 2 — firmware (web, SNMP, OTA, ladění na živých kusech, ~20 verzí) | ~80 čh |
| Etapa 3 — integrace power monitor | ~15 čh |
| Etapa 4 — rollout 5 jednotek | ~20 čh |

## Požadované zdroje

**Hardware (na jednotku):**
- ESP32 DevKit
- Schmittův invertor 74LVC14A (level-shift 5V→3V3 + čištění hran)
- Pasivní součástky (1k vstup, 100R výstup, bulk kondenzátor)
- Kabeláž, montážní materiál
- MikroTik mAP lite — jen lokality bez ethernetu (WiFi AP + NAT brána)

**Infrastruktura:**
- Síť spolku 10.107.0.0/16 (SNMP polling z power monitoru)
- Power monitor instance (existující, power.hkfree.org)
- Montážní přístup ke zdrojům na lokalitách

## Rozpočet

| Položka | Cena (orientační) |
|---|---|
| Práce ownera | ~150 člověkohodin |
| ESP32 DevKit + součástky | ~250 Kč / jednotka |
| MikroTik mAP lite (jen lokality bez ethernetu) | ~700 Kč / kus |
| **Hardware celkem (5 jednotek + 1× mAP)** | **~1 950 Kč** |

Provozní náklady: zanedbatelné (odběr ESP v úsporném režimu, sdílený polling existujícího power monitoru).

## Owner projektu

**Pavel Vlček (Pája)** — pvlcek@seznam.cz

## Technologický stack

| Vrstva | Technologie |
|---|---|
| Hardware | ESP32 DevKit, 74LVC14A, zástavba do skříně zdroje |
| Sběr dat | Pasivní vzorkování GPIO, SW rekonstrukce TM1640 rámců (~500 kHz, rámec ~192 µs, 2×/s) |
| Firmware | Arduino core (C++), OTA přes web `/update` |
| Web | HTTP server, basic-auth (admin zápis / guest jen čtení), dashboard + `/api/status` |
| SNMP | v1, enterprise MIB `1.3.6.1.4.1.53864.1.1` (idx 1–50) |
| Síť | MikroTik RouterOS (mAP lite: AP + NAT, dst-nat web/SNMP), export konfigurace v repu |
| Integrace | Power monitor (Node.js, power.hkfree.org), polling ~30 s |

## Dopad na infrastrukturu

- Jednotky jsou pasivní posluchači displeje — do zdrojů se nezasahuje
- Jedno nové zařízení v síti na lokalitu (za mAP bránou tam, kde není ethernet)
- Síťový provoz = SNMP polling z power monitoru (~1× za 30 s) + občasný přístup na web
- Žádná nová serverová služba — sběr dat využívá existující power monitor
- OTA aktualizace přes WiFi (nevyžaduje fyzický přístup po prvním flashování)

## Bezpečnostní dopady

| Oblast | Opatření |
|---|---|
| Web | Basic-auth: admin (zápis) / guest (jen čtení) |
| SNMP | Read-only, pouze enterprise větev |
| Credentials | Reálná hesla a WiFi klíče mimo repozitář (placeholdery, sanitizace před pushem) |
| mAP brány | Firewall: správa jen z 10.107.0.0/16, WAN input drop, vypnuté nepoužívané služby |
| Expozice | Jednotky pouze v interní síti 10.107.0.0/16, nejsou vystavené do internetu |
| OTA | Chráněno admin přihlášením |

## GitHub

> https://github.com/Pajaaaa/Mhpower-UPS

Struktura repozitáře:
```
README.md
docs/
  HISTORIE.md
  foto-realizace/
  projektova-prihlaska.md
firmware/
  mhpower_esp32_capture/
network/
  mikrotik-map-mhpower.rsc
tools/
```

Dle směrnice čl. 8.3 plánován přesun/mirror do GitHub organizace HKFree.

## Dokumentace

- `README.md` — princip, zapojení, build, konfigurace, tabulka SNMP OID
- `docs/HISTORIE.md` — changelog verzí firmwaru
- `docs/foto-realizace/` — fotogalerie provedení zástavby
- `network/` — export konfigurace mAP brány
- Wiki stránka projektu (dle doporučené struktury směrnice) — doplnit

## Roadmapa

| Etapa / verze | Stav | Obsah |
|---|---|---|
| Etapa 1 — PoC | ✅ Dokončeno | Odposlech + dekódování TM1640, HW zapojení |
| Etapa 2 — firmware v1.21 | ✅ Dokončeno | Web, SNMP, OTA, odhad výdrže, guest účet, diagnostika |
| Etapa 3 — integrace | ✅ Dokončeno | Power monitor: SNMP profil, log událostí, odhad výdrže |
| Etapa 4 — rollout | ✅ Dokončeno | 5 jednotek nasazeno (v1.21) |
| Oživení Kuklenska | 🔧 Otevřeno | ESP bez napájení, nutný výjezd |
| HW robustnost napájení | 💡 Plánováno | Reset supervisor na EN pin (řeší zaseknutí při měkkém náběhu 5V větve) |
| Dekódování V↑/V↓ | 💡 Plánováno | Bity přepětí/podpětí z ikon displeje (`/api/iconscan` čeká na reálnou událost mimo 207–253 V) |
| Wiki + HKFree org | 💡 Plánováno | Wiki stránka projektu, přesun repa do organizace HKFree |

---

## Known Issues

- Jednotka Kuklenska (10.107.1.212) offline — ESP bez napájení, nutný výjezd na lokalitu
- Měkká 5V větev zdroje: velký bulk kondenzátor zpomalí náběh a ESP se může zaseknout při startu — řešením je reset supervisor na EN pin (plánováno)
- Pasivní odposlech produkuje chybové rámce (běžně i ~75 % timeoutů) — glitche stavových bitů řeší debounce (3 rámce), jde o normální šum, ne poruchu
- SNMP pouze v1 (v2c dotazy firmware ignoruje) a bez standardního system MIB — monitoring musí číst přímo enterprise OID
- Bity přepětí/podpětí ikon displeje zatím nedekódované (síť v HK z pásma 207–253 V prakticky nevyjíždí)
- Repozitář zatím pod osobním účtem, ne v organizaci HKFree (viz Roadmapa)
