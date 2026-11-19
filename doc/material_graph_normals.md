# Normal-Maps im Materialgraphen kombinieren

Für PBR-Lit- und Legacy-Lit-Materialien gibt es den Knoten **Normal Blend**.

1. Zwei **Textur**-Knoten anlegen und die Normal-Maps hineinziehen.
2. Über Rechtsklick im Graphen **Normal Blend** hinzufügen.
3. Die erste Textur mit **Basis**, die zweite mit **Detail** verbinden.
4. Den Ausgang von **Normal Blend** mit **Normal** am Material verbinden.

Die Regler an Basis und Detail steuern die jeweilige Stärke von 0 bis 4.
Bei 1 bleibt die Eingangsnormal unverändert, bei 0 entfällt die Ebene.
Unverbundene Eingänge und leere Normal-Textur-Knoten liefern eine flache Normale.
Der Regler **Normal Strength** am Material wirkt zusätzlich auf das Gesamtergebnis.

Normal Blend verwendet Reoriented Normal Mapping (RNM). Dadurch bleibt die
Detailstruktur auf der durch die Basisnormalen beschriebenen Oberfläche erhalten.
Die Texturen dürfen unterschiedliche UV-Kachelung und UV-Quellen verwenden;
ihre Normalen werden vor der Kombination in einen gemeinsamen Tangentenraum
gebracht. Auch gespiegelte Kachelung wird berücksichtigt.

**Mix** kann ebenfalls an Normal angeschlossen werden, um zwischen zwei
Normal-Maps zu überblenden, etwa mit einer Maske. Math-Knoten sind ebenfalls
anschließbar. Ihre Werte werden am Normal-Eingang als kodierte RGB-Normalen
(0 bis 1, flach = 0.5 / 0.5 / 1) interpretiert und anschließend normalisiert.
Normal Blend legt dagegen die Details beider Ebenen übereinander.

Texturen im berechneten Normal-Zweig werden linear und mit der Normal-Map-
Importbehandlung geladen. RGB- und BC5-Normalen rekonstruieren Z aus XY;
die Texturen sollten Tangentenraum-Normalen der oberen Halbkugel enthalten.
Mix-Masken und UV-Steuertexturen bleiben gewöhnliche lineare Daten.

Die bestehenden Grenzen gelten weiter: höchstens 16 Graph-Instruktionen und
vier Textur-/UV-Slots. Eine Textur, die sowohl als Farbe als auch als Normale
verwendet wird, benötigt getrennte Slots und gegebenenfalls zusätzliche
Instruktionen. Ein einzelner Textur-Knoten direkt an Normal verwendet weiterhin
den bisherigen Materialpfad. Benutzerdefinierte Shader werten den Graphen
weiterhin nicht aus.

Die Auswertung ist in den HLSL- und GLSL-Shadern vorhanden, einschließlich
Forward+ unter DX11/Vulkan. Das Dateiformat behält bestehende Knoten-IDs bei;
Normal Blend ist als neuer Knotentyp angehängt.
