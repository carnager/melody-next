# Melody icons

ADR-0287: the application is Melody, and its icon is the **vinyl bird**,
`melodies-vinyl-bird-v7.svg` -- a blackbird whose body is a record. The
**blackbird**, `melodies-blackbird-v2.svg`, is the mascot, for the About box.
Both are traced vector sources, not used by the build yet; the rename wires
them in and retires the crate icons below.

The crate icons, drawn for the name Melodies before:

| File | Size | Use |
|---|---|---|
| `melodies.svg` | 1024, scalable | 48 px and up: launchers, docks, the about box |
| `melodies-32.svg` | 32 | 24 and 32 px, drawn on that grid |
| `melodies-16.svg` | 16 | 16 px, drawn on that grid |
| `melodies-symbolic.svg` | 16 | Panels and trays: one colour (#2e3436), recoloured by the desktop's theme |
| `melodies-monochrome.svg` | 256 | A flat single-colour mark (`currentColor`), for the web site and the like |

In a freedesktop icon theme they go to `hicolor/scalable/apps/melodies.svg`,
`hicolor/32x32/apps/melodies.svg`, `hicolor/16x16/apps/melodies.svg` and
`hicolor/symbolic/apps/melodies-symbolic.svg`.

The symbolic icon is drawn on the 16 px grid, back to front: each sleeve shows
only where the one in front leaves it, with a pixel of gap, so in one colour
the shapes stay apart.
