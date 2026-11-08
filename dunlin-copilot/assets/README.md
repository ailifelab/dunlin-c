# Dunlin Copilot icon

`DunlinCopilot.png` is the transparent master (1254 × 1254 RGBA).
`DunlinCopilot.icns` packages the standard 16/32/128/256/512 point 1×/2×
representations. Xcode and the SwiftPM packaging script both copy this resource
and declare `CFBundleIconFile`. The PNG has zero-alpha corners and a near-opaque
center (alpha 253/255); evidence is in `reports/v51-current/icon-alpha.json`.

Source: the user's 2026-11-08 concept image. Edited with the built-in imagegen
tool, transparent-background mode; the original was not deleted. Packaging sizes
were generated with macOS `sips` and `iconutil`.

Final edit prompt:
“Use case: background-extraction. Edit target: attached Dunlin icon concept.
Remove ONLY the external white/cream background and external cast shadow,
making it truly transparent. Preserve the ENTIRE central dark rounded-square
app icon, its bevel, ivory bird, ivory orbit ring, green orb and internal glow
exactly. Do not remove the dark tile. No redesign, no text, no additions.
Square production icon canvas with the complete icon centered and small
transparent margins; no clipped edges.”
