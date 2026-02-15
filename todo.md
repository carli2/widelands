# Widelands Hebrews Tribe - TODO

## Goldmine Redesign: Oberflaechenmine + Tiefe Mine

### Oberflaechenmine (hebrews_goldmine)
- [ ] Sprite redesign: flachere Raender, sichtbarer Eingang, Oberflaechencharakter
- [ ] yield: 33.33% (nur obere Schichten erreichbar)
- [ ] Buildcost: clay=6, granite=4, cloth=2, branch=2 (wie bisher)
- [ ] Input: bread_hebrews (wie bisher)
- [ ] 1 Miner
- [ ] Enhancement-Eintrag auf hebrews_goldmine_deep

### Tiefe Goldmine (hebrews_goldmine_deep) - NEU
- [ ] Neuer Ordner: data/tribes/buildings/productionsites/hebrews/goldmine_deep/
- [ ] Sprite: Tiefenillusion (dunkler Schacht, Stuetzkonstruktionen, Fackeln)
- [ ] yield: 100% (erreicht tiefste Schichten)
- [ ] Enhancement von hebrews_goldmine (kein separater Bau)
- [ ] Enhancecost: granite=4, branch=4, cloth=2
- [ ] Input: bread_hebrews + wine (extra Verpflegung fuer tiefere Arbeit)
- [ ] 2 Miner (working_positions)
- [ ] register.lua erstellen
- [ ] In units.lua registrieren
- [ ] AI-Hints setzen

### Sprite-Erstellung

#### Oberflaechenmine (idle)
**Prompt:** "Top-down isometric pixel art of a shallow open-pit gold mine entrance built into a hillside, ancient Hebrew/biblical style. Flat stone edges, visible gold veins in exposed rock face, simple wooden frame around a low cave entrance, clay and stone construction, dry desert terrain. Small pickaxe and oil lamp near entrance. Warm earthy tones, sandstone colors. No background, transparent, single building sprite, Widelands RTS game style, clean edges."

#### Oberflaechenmine (working)
**Prompt:** Wie idle, aber: "Dust particles rising from entrance, faint orange glow from inside the cave, a small pile of freshly mined rock next to the entrance. Subtle animation-ready differences from idle state."

#### Tiefe Goldmine (idle)
**Prompt:** "Top-down isometric pixel art of a deep gold mine with vertical shaft, ancient Hebrew/biblical style. Dark deep shaft opening reinforced with heavy wooden support beams and cross-braces, stone walls around the shaft, two flickering torches mounted on wooden posts illuminating the entrance. Rope-and-pulley winch mechanism over the shaft. More substantial construction than surface mine - larger footprint, higher wooden framework. Warm torch glow contrasting with dark shaft depth. No background, transparent, single building sprite, Widelands RTS game style, clean edges."

#### Tiefe Goldmine (working)
**Prompt:** Wie idle, aber: "Brighter torch flames, rope on winch is taut (pulling ore up), golden ore chunks visible in a basket at the top of the shaft, more dust/smoke particles rising from the shaft. Subtle animation-ready differences from idle state."

#### Nachbearbeitung (beide Minen)
- [ ] KI-Bilder generieren (z.B. mit DALL-E, Midjourney oder Stable Diffusion)
- [ ] Hintergrund entfernen (ImageMagick floodfill, fuzz ~18%)
- [ ] Trimmen und auf Widelands-Canvas skalieren (idle als Referenz: 132x176 @1x)
- [ ] Hotspot bestimmen (Gebaeude-Fussposition, aktuell {66, 88})
- [ ] idle + working + empty Sprites fuer beide Minen
- [ ] Alle 4 Skalierungen erzeugen (0.5, 1, 2, 4)
- [ ] menu.png (50x50) fuer Tiefe Mine separat erstellen

---

## LLM-basierter AI-Client

### Architektur-Ueberblick
Der aktuelle AI-Client (DefaultAI) wird per `think()` alle ~500ms aufgerufen.
Neuer LLM-Client wuerde als Alternative `ComputerPlayer`-Implementierung funktionieren.

### Komponenten
- [ ] **HTTP-Client einbinden** - cpp-httplib (header-only) oder libcurl
- [ ] **LlmAI Klasse** erstellen (erbt von ComputerPlayer)
  - Zustandsmaschine: kWaiting -> kPendingResponse -> kProcessing
  - Async Request-Queue (nicht-blockierend im think()-Tick)
- [ ] **World-State Serializer** - JSON-Builder fuer:
  - Freie Bauplaetze (Groesse, Entfernung, Feinde in Naehe)
  - Ressourcen-Lagerbestaende
  - Gebaeude-Statistik (gebaut/in Bau/Auslastung)
  - Feind-Positionen und Staerke
  - Letzte Aktionen + Ergebnis
  - Neue Nachrichten mit Koordinaten
- [ ] **Tech-Tree Prompt** generieren (einmalig beim Start):
  - Alle Gebaeudetypen mit Kosten, Inputs, Outputs
  - Verfuegbare Aktionen (bauen, abreissen, angreifen, Prioritaeten)
  - Wirtschaftsbeziehungen
- [ ] **Response Parser** - JSON-Antworten -> game().send_player_*() Befehle
  - Validierung: Koordinaten gueltig? Ressourcen vorhanden?
  - Fallback auf DefaultAI bei Fehler/Timeout
- [ ] **Ollama/OpenAI Anbindung**
  - Ollama lokal: 1-4 Ticks Latenz (empfohlen)
  - OpenAI Cloud: 4-20 Ticks Latenz (moeglich aber traeger)

### Verfuegbare Aktionen (send_player_*)
- `send_player_build_building(player, coords, building_id)`
- `send_player_build_flag(player, coords)`
- `send_player_build_road(player, path)`
- `send_player_bulldoze(building/flag)`
- `send_player_attack(building, soldier_count)`
- `send_player_start_stop_building(building)` (Produktion starten/stoppen)
- `send_player_set_ware_target_quantity(...)` (Wirtschaftsziele)
- `send_player_diplomacy(...)` (Buendnisse/Krieg)

### Aufwand-Schaetzung
| Komponente | Aufwand | Schwierigkeit |
|---|---|---|
| HTTP-Client einbinden | 1-2 Tage | Einfach |
| LlmAI Grundgeruest | 2-3 Tage | Mittel |
| World-State Serializer | 3-5 Tage | Mittel |
| Tech-Tree Prompt | 1-2 Tage | Einfach |
| Response Parser + Validierung | 3-4 Tage | Mittel |
| Async-Polling + Fehlerbehandlung | 2-3 Tage | Schwer |
| Tests + Integration | 3-5 Tage | Mittel |
| **Gesamt MVP** | **~3-4 Wochen** | |

### Datenstruktur-Entwurf (JSON pro Tick)
```json
{
  "tick": 12450,
  "game_time_seconds": 6225,
  "player": 2,
  "resources": {"gold_ore": 45, "copper_ore": 12, "log": 34, "branch": 120},
  "buildings": {
    "hebrews_coppermine": {"built": 2, "constructing": 1, "idle": 0},
    "hebrews_tent_small": {"built": 5, "constructing": 0}
  },
  "free_spots": [
    {"x": 123, "y": 456, "size": "big", "mine": false, "dist_warehouse": 15},
    {"x": 130, "y": 460, "size": "mine", "mine": true, "resource": "iron", "dist_warehouse": 22}
  ],
  "enemies": [
    {"player": 1, "military_strength": 450, "nearest_site": {"x": 200, "y": 300}, "relation": "war"}
  ],
  "last_actions": [
    {"action": "build", "building": "hebrews_coppermine", "coords": "125,458", "result": "started"}
  ],
  "messages": [
    {"type": "economy", "text": "Copper mine exhausted", "x": 125, "y": 458}
  ]
}
```

### Schluessel-Dateien im Widelands-Code
- `src/ai/computer_player.h` - Basis-Interface (think()-Methode)
- `src/ai/defaultai.h/cc` - Referenz-Implementierung (~10.000 Zeilen)
- `src/ai/ai_help_structs.h` - Datenstrukturen fuer Beobachter
- `src/logic/single_player_game_controller.cc:76` - think()-Aufruf

---

## Hebrew Shipyard (Werft) - Sprites

- [ ] Aktuell werden Empire-Platzhalterbilder verwendet
- [ ] Eigene Sprites im hebraeischen Architekturstil erstellen
- [ ] Canvas: 384x304 px (4x Scale), Hotspot: (224, 232)
- [ ] 4 Sprites noetig: idle, working, unoccupied, build (4 Frames)

### AI-Prompts

#### Shipyard idle (Hauptbild)

**Prompt:**
```
Top-down isometric pixel art sprite of an ancient Hebrew/Israelite shipyard, medium-sized building for a strategy game (Widelands).

ARCHITECTURE STYLE (match existing Hebrew tribe):
- Mudbrick and clay walls with rough sandstone texture
- Flat or slightly angled roof covered with draped linen/cloth canopy in beige/cream
- Wooden support beams from rough-hewn timber, no sawn planks
- Desert/Levantine feel: warm ochre, terracotta, sand, and cream tones
- NO European medieval elements, NO stone masonry, NO thatched roofs

BUILDING DETAILS:
- Open-sided workshop structure facing the water, cloth awning providing shade
- A partially constructed wooden boat hull resting on log rollers
- Wooden workbench with shipwright tools (adze, mallet, bronze drill, coils of rope)
- Stack of branches/sticks and folded cloth bolts as building materials (NOT sawn planks)
- Clay water jug and oil lamp near the workbench
- Low mudbrick retaining wall on the landward side

TECHNICAL REQUIREMENTS:
- Isometric 2:1 perspective (standard RTS dimetric projection)
- Canvas size: 384x304 pixels (this is the 4x scale version)
- Building footprint centered, hotspot at approximately (224, 232) from top-left
- Fully transparent background (PNG with alpha channel)
- Clean pixel-art edges, no anti-aliasing against background
- Consistent lighting from top-left
- Color palette: warm earth tones matching desert/Levantine setting
- Single static frame, no animation
```

#### Shipyard working

**Prompt:**
```
Same building as idle, but with these differences to show active work:
- Wood shavings and sawdust scattered around the hull
- A Hebrew shipwright figure actively working on the hull with a mallet
- Rope pulled taut across the boat frame
- Oil lamp lit with visible warm glow
- More construction progress visible on the hull (additional planks attached)
- Slight dust/particle effects near the work area
Same canvas size and technical requirements as idle.
```

#### Shipyard unoccupied

**Prompt:**
```
Same building as idle, but showing an abandoned/empty workshop:
- No tools on the workbench (tidied away or absent)
- Cloth canopy slightly drooping/slack
- No lamp lit
- Boat hull looks neglected, same construction progress as idle
- Overall slightly darker/less vibrant tone
Same canvas size and technical requirements as idle.
```

#### Shipyard build (4 Bauphasen)

**Prompt:**
```
Four-frame construction sequence of the shipyard being built, left to right on a single 1536x304 spritesheet (4 columns x 1 row, each frame 384x304):

Frame 1: Foundation only - low clay walls being laid, wooden stakes in ground marking outline
Frame 2: Walls half-height, first support beams erected, construction scaffolding visible
Frame 3: Walls complete, roof beams in place, cloth awning partially draped
Frame 4: Nearly complete - awning fully stretched, workbench placed, boat rollers set up
Same technical requirements as idle (transparent background, isometric, lighting).
```

### Nachbearbeitung
- [ ] KI-Bilder generieren (DALL-E, Midjourney oder Stable Diffusion)
- [ ] Hintergrund entfernen (ImageMagick floodfill, fuzz ~18%)
- [ ] Auf 384x304 Canvas zuschneiden (4x Scale)
- [ ] Skalierungen: `convert idle_4.png -resize 50% idle_2.png` usw. -> 192x152 (2x), 96x76 (1x), 48x38 (0.5x)
- [ ] Playercolor-Overlay `_pc.png` erstellen (transparentes Bild, Dach/Canopy-Flaeche in Magenta #FF00FF)
- [ ] menu.png als 24x24 Icon
- [ ] build-Spritesheet: 1536x304 (4 Frames nebeneinander) + _pc.png

---

## Sonstige offene Punkte
- [ ] Initiation Site umwidmen oder entfernen (Machane erzeugt jetzt Soldaten direkt)
- [ ] Pruefen: Hat der Stamm ueberhaupt Rangers/Foerster? (AI braucht das fuer Waldschutz)
- [ ] Solomon's Harbour testen (Menora -> Log Tausch)
