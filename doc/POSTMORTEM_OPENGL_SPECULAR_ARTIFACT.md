# Postmortem: OpenGL Specular Artifact

## Symptom

In OpenGL erschien sporadisch ein kleines schwarzes Rechteck im Bild. Es war nicht stabil an eine Weltposition gebunden, sondern hing von Kamerawinkel und Bewegung ab. In den meisten Debug-Views verschwand es, in `F9` Direct Specular war es sichtbar.

## Was es nicht war

Der Fehler war:

- kein Cursor
- kein Shadow-Artefakt
- kein AO-, IBL- oder Normalmap-Einzelproblem
- kein asset-spezifisches Loch im Mesh

Die Toggles `Num1` bis `Num4` änderten nichts. Damit waren IBL, Shadows, AO und Normalmap als alleinige Ursache ausgeschlossen.

## Eingrenzung

Die entscheidende Beobachtung war: Nur `F9` zeigte den Fehler. Damit lag der Fehler im direkten Specular-Term des PBR-Shaders, nicht im finalen Tonemap, nicht im Diffuse-Term und nicht in den Materialtexturen.

## Root Cause

Der OpenGL-PBR-Specular-Pfad war numerisch nicht robust genug. Bei bestimmten Kamerawinkeln konnte der Halbvektor `V + L` degenerieren oder Derivate aus `ApplySpecularAA()` konnten ungueltige Werte erzeugen.

Daraus konnten NaN/Inf-Zwischenwerte im GGX-Specular entstehen. OpenGL zeigte diese als schwarzes blockartiges Artefakt. DX11/Vulkan waren hier toleranter oder verhielten sich anders, wodurch der Fehler dort nicht sichtbar wurde.

## Fix

Der Specular-Pfad wurde allgemein stabilisiert:

- `ApplySpecularAA()` prueft ungueltige Varianz und faellt auf die urspruengliche Roughness zurueck.
- `EvalSpecularGGX()` bricht bei `NoL <= 0` frueh ab.
- Der Halbvektor `V + L` wird vor Normalisierung auf Laenge geprueft.
- NaN-Specular wird abgefangen.
- Das Ergebnis wird auf nicht-negative Werte begrenzt.
- Die Aenderung wurde in GLSL und HLSL gespiegelt, damit OpenGL/DX/Vulkan denselben Shader-Vertrag behalten.

Zusaetzlich wurden allgemeine OpenGL-State-Probleme bereinigt:

- Sampler-State wird pro Frame sauber zurueckgesetzt.
- Pass-SRVs bekommen Clamp-Sampler.
- `shadowMapRaw` nutzt wieder den korrekten Shadow-Slot.

## Betroffene Dateien

- `assets/pbr_lit.opengl.fs.glsl`
- `assets/pbr_lit.ps.hlsl`
- `addons/opengl/GLShaderReflector.cpp`
- `addons/opengl/OpenGLCommandList.cpp`
- `examples/framework/SkullScene.cpp`

## Verifikation

- OpenGL-Build lief erfolgreich durch.
- Smoke-Run startete und kompilierte die Shader erfolgreich.
- Die bekannten Warnungen waren nicht ursächlich fuer das Artefakt.

## Lesson Learned

Bei Rendering-Artefakten zuerst isolieren, welcher Render-Term betroffen ist. In diesem Fall war `F9` der Durchbruch. Kameraabhaengige, kurz aufpoppende Bloecke sind oft kein Mesh- oder Texturproblem, sondern ein numerischer Shader-Fehler oder ein State-Leak.

Debug-Views fuer einzelne Lighting-Terme sind dafuer besonders wertvoll.
