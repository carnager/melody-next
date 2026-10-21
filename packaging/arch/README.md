# Arch Linux packages

Each directory here is one AUR package, as published:

| Package | What |
| --- | --- |
| `melody-git` | the desktop app, Melody (ADR-0287: Trackknife's `trackknife-git` before) |
| `melodyd-git` | the engine (`melodyd-git`) and speakers (`melody-agent-git`) |
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
cd packaging/arch/melodyd-git
makepkg -si
```

Every package builds from the repository's HEAD on GitHub, not from this
checkout.

## Melody's own repository

`repo/publish` builds these packages on one machine and publishes them as a
signed pacman repository, so the others install binaries with `pacman -Syu`
instead of compiling. It builds what is on GitHub, exactly as above, and does
nothing when GitHub has not moved on since it last published.

Once, where it is served -- a folder the publishing user may write, and the
web server pointed at it (Caddy here):

```sh
sudo install -d -o $USER -g http /srv/http/melody-repo
```

```
repo.example.org {
    root * /srv/http/melody-repo
    file_server browse
}
```

Once, where it is built -- in `~/.config/melody/repo.conf`:

```sh
MELODY_REPO_TARGET=server:/srv/http/melody-repo
MELODY_REPO_KEY=0123ABCD        # gpg --list-secret-keys --keyid-format short
```

Then `packaging/arch/repo/publish`, by hand, or every hour by the user timer:

```sh
cp packaging/arch/repo/melody-repo.{service,timer} ~/.config/systemd/user/
systemctl --user enable --now melody-repo.timer
```

Signing from the timer needs the key unlocked in gpg-agent, or a key of its
own without a passphrase.

Once on each machine that installs from it -- trust the key, and add the
repository to `/etc/pacman.conf`:

```sh
curl -s https://repo.example.org/melody.asc | sudo pacman-key --add -
sudo pacman-key --lsign-key 0123ABCD
```

```
[melody]
Server = https://repo.example.org
```

The packages keep their AUR names (`melody-git`, `melodyd-git`, ...), so one
installed from the AUR is replaced by the repository's on the next
`pacman -Syu`. Leave `--devel` out of `yay -Syu` then, or yay rebuilds them
from the AUR.
