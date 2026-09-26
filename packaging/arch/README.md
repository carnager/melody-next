# Arch Linux packages

Each directory here is one AUR package, as published:

| Package | What |
| --- | --- |
| `trackknife-git` | the desktop app |
| `melody-git` | the engine (`melodyd-git`) and speakers (`melody-agent-git`), plus a metapackage |
| `melody-cli-git` | `melody-cli`, built without the engine |
| `melody-watch-git` | `melody-watch`, for a NAS that cannot run the engine (ADR-0232) |

They are the AUR repositories' files, kept here so the packaging changes
with the code. Change a package in both places: here, and in its AUR
repository (`ssh://aur@aur.archlinux.org/<package>.git`), with `.SRCINFO`
regenerated there by `makepkg --printsrcinfo > .SRCINFO`.

No binary is built by two packages. The split used to be one PKGBUILD
building everything, and the AUR's `melody-git` once built `melody-cli-git`
too: installed beside the standalone package, their debug packages both
held `melody-cli.debug`.

To build one from this repository, as the AUR would:

```sh
cd packaging/arch/melody-git
makepkg -si
```

Every package builds from the repository's HEAD on GitHub, not from this
checkout.
