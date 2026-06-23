# Fotky skutečného provedení

Fotografie reálné realizace — zástavba ESP32 do zdroje MHpower a instalace v provozu.
Pořízeno 23. 6. 2026.

| foto | co je vidět |
|------|-------------|
| [01 — vnitřek, panel displeje odklopený](01-vnitrek-panel-displeje-odklopeny.jpg) | Otevřená skříň zdroje, čelní panel s deskou displeje odklopený doleva. Nahoře hlavní výkonová deska, dole toroidní transformátor. ESP32 je přidělané na zadní straně desky displeje. |
| [02 — detail ESP32 + TM1640 + level-shifter](02-detail-esp-tm1640-levelshifter.jpg) | Detail: ESP32 (WROOM) napájené na desku displeje, vidět budič **TM1640** (SOIC), bulk elektrolytický kondenzátor (proti brownoutu) a malá deska **74LVC14A** Schmittova invertoru se vstupními 1k odpory. |
| [03 — detail zapojení, jiný úhel](03-detail-esp-zapojeni-jiny-uhel.jpg) | Tentýž uzel z druhé strany — ESP32 dev board s USB-C, level-shifter, propojky CLK/DIN na TM1640. |
| [04 — ESP namontovaný ve skříni](04-esp-namontovany-ve-skrini.jpg) | ESP sestava přimontovaná v rohu kovové skříně na desce displeje, svazek vodičů. |
| [05 — sestaveno, displej svítí](05-sestaveno-displej-sviti.jpg) | Hotový kus na stole, displej svítí (výstup 230 V, baterie plná), skříň odkrytá. Vedle olověná baterie MHD GE9-12 (12 V / 9 Ah). |
| [06 — dva kusy + baterie](06-dva-kusy-a-baterie.jpg) | Dva invertery MHD vedle sebe, jeden displej svítí, vedle dvě olověné baterie s kabeláží. |
| [07 — instalace v rozvaděči](07-instalace-rozvadec.jpg) | Reálná instalace ve venkovním rozvaděči: baterie MHD GE100-12 (100 Ah), jističe/svodiče, inverter, napájení. |
| [08 — instalace + MikroTik mAP](08-instalace-mikrotik-map.jpg) | Detail instalace: inverter s rozsvíceným displejem, nad ním bílá brána **MikroTik mAP lite** (NAT před ESP), baterie, zásuvky. |

Souvislosti: zapojení přes Schmittův invertor viz [`../zapojeni_spravne.png`](../zapojeni_spravne.png)
a [HISTORIE → Schmittův trigger](../HISTORIE.md). Síťová brána mAP popsána v
[`../../network/README.md`](../../network/README.md).
