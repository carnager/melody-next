# ADR-0273: Locate in the layout shown

## Status

Accepted, 2026-10-05. Amends ADR-0254, whose views sent locate back to the
artist tree.

## Context

Locate artist and Locate album found a file under its artist in the artist
tree, and so -- as ADR-0254 decided -- switched any other layout -- a genre view, Folders, Recently
added -- to the artist tree, and saved that as the layout chosen. A person
who browses by genre lost their layout every time they located something.

The artist tree was the only layout locate knew a way through. A view's
nodes are labels its levels make, which only the engine can evaluate.

## Decision

- **Locate finds the file in the layout shown**, which stays:
  - the artist tree: its artist, and for an album the album under it;
  - Recently added: its album, for either;
  - Folders: each folder it is under, down to its own;
  - a view: its node at each level, as the engine finds them; an album at
    the last level, an artist at the level whose label is the artist's
    name, or the last when none is.
- **The engine says where a file is in a view.** A view's `catalogue.query`
  that names a file (`path`) answers the way to it: its node at each level
  below `view_path`, the first it is under as the levels order them, each
  as its parent lists it -- with `located: true`, so a client is not
  misled by an older engine answering a node's children instead. A file the
  view leaves out has an empty way.
- The tree loads as it does when opened by hand; rows on the way are
  opened, further pages fetched while the way is not among them, and the
  last made current.

## Consequences

- Protocol level 7. An older engine cannot locate in a view; the panel says
  so and leaves the layout as it is.
- A file under several nodes ($each) is found under the first.
