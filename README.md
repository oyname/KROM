# KROM

## Hinweis: Texturen fehlen im Repository

Der Ordner `projects/zweitesProjekt/Assets/Textures/` ist **nicht** im Repository enthalten
(ca. 200 MB, per `.gitignore` ausgeschlossen).

Folgen nach dem Klonen bzw. dem ZIP-Download:

- Das Projekt baut und startet normal.
- Szenen und Materialien, die auf diese Texturen verweisen, werden ohne Texturen bzw. mit
  Fallback-Materialien dargestellt.
- HDR-Umgebungen (`*.hdr`) für Skybox/IBL fehlen ebenfalls.

Wer die Texturen nutzen möchte, legt eigene Dateien mit denselben Namen in
`projects/zweitesProjekt/Assets/Textures/` ab oder weist den Materialien im Editor eigene
Texturen zu.
