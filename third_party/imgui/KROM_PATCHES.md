# KROM patches for Dear ImGui

The 22 vendored files in the base commit were compared against the official
[v1.92.9b-docking tag](https://github.com/ocornut/imgui/tree/v1.92.9b-docking).
All 22 matched the upstream Git blobs after applying the repository's line
ending rules.

## Editor tab appearance

`imgui_widgets.cpp` contains the patch. Each block starts with a `KROM-PATCH`
comment.

- `ImGui::SetKromTabAppearance()` attaches the palette parameters to the current
  context and releases them when the context shuts down. The editor calls the
  setter from `addons/editor/EditorTheme.cpp` on initial setup and theme changes.
- `ImGui::TabItemBackground()` shades the vertices of the existing rounded fill
  and preserves their alpha. The same function draws center docking previews.
- `ImGui::TabItemEx()` tints each state on hover and draws the selected tab's
  bottom line using the existing focused or dimmed overline color and size.
- `ImGui::TabItemLabelAndCloseButton()` applies the text color and centers
  labels that fit. The existing clipping, close action, and unsaved marker
  still use ImGui's layout and input handling.

When updating ImGui, compare the new vendor baseline against the matching
upstream docking tag. Port the marked blocks to the corresponding tab
functions, keeping the context opt-in and the original drawing path for
contexts that skip the setter. Confirm that the gradient helper still
preserves vertex alpha and that the docking preview still calls the tab
background function. Build all three editors and check both themes, the
selection and hover states, the close and unsaved glyphs, and docking.
