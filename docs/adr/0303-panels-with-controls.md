# ADR-0303: Panels with controls

## Status

Accepted, 2026-10-20. Extends the panels of ADR-0291.

## Context

- **A panel could only be read.** What a plugin offered to do was spread
  over Settings, Workspace › Run and the context menus; a panel's one way
  back to its plugin was a link. Listening together (ADR-0302) needed a
  room's name and password typed, a room started and stopped, a party
  turned on, and votes cast -- each in another corner of Melody.
- **A panel was asked only about what plays.** With nothing playing it said
  so and asked nothing -- but a room is started before anything plays.

## Decision

1. **A panel answers controls among its parts**: an input -- `{"input":
   KEY, "label", "value", "placeholder", "secret", "action"}`, its text
   hidden when secret, Enter running its action -- a toggle -- `{"toggle":
   KEY, "label", "on"}` -- a button -- `{"button": LABEL, "action", "value",
   "primary"}`, side by side with the buttons around it -- and an item's
   buttons at its right, `"buttons": [{"text", "action", "value", "on"}]`,
   one that is on drawn as chosen (a vote of one's own).
2. **Pressed, the panel's program runs again**, told `"action": {"id",
   "value"}` -- a button's action and value, a toggle's key and "on" or
   "off" -- and `"inputs"`, every input's text as it stood. Its answer is
   the panel's new content. A plugin acts and draws in one run; there is no
   second protocol.
3. **Typing survives an answer**: an input keeps what was typed in it
   unless the plugin answers a new value for it.
4. **Asked again about the same, a panel keeps what it shows** until the
   answer comes, so a press, or a room changing, does not flicker.
5. **`"always": true`**: a panel of controls is asked with nothing playing
   too, `playing` then null.

## Verification

- `plugins`: controls read, malformed ones refused; `always` read; an
  action and the inputs told.
- `bench-main-window`: a panel asked with nothing playing; text typed,
  a button pressed: run again with the action and the text, which stays
  typed; an item's button, its action and value.
