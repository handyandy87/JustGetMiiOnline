# JustGetMiiOnline

An Aroma plugin that picks which account server your Wii U signs in against, which
Miiverse it talks to, and which network serves SpotPass. Protarium's own Inkay stays
installed and in charge of everything else. I move those three things and nothing else.

**This only works with Protarium's version of Inkay, and you want their latest build:
[Protarium-Network/Inkay-GitHub-Release](https://github.com/Protarium-Network/Inkay-GitHub-Release).**

What you get out of it:

- Mix and match. Any account server with any Miiverse, including pairings no single Inkay
  build offers, without installing a different Inkay for each combination you want to try.
- Two account servers, Protarium and Pretendo. Three Miiverses, Protaverse, Juxt and
  Roseverse.
- A toast at every boot naming the account server and Miiverse in use, so you always know
  what you're on without opening anything.

## Installing

**You need Protarium's build of Inkay**, from
[Protarium-Network/Inkay-GitHub-Release](https://github.com/Protarium-Network/Inkay-GitHub-Release).
This plugin rides on top of theirs and does nothing without it.

Download `JustGetMiiOnline.wps` from this repository's
[Releases](https://github.com/handyandy87/JustGetMiiOnline/releases) page, then copy it to
`sd:/wiiu/environments/aroma/plugins/`, alongside Protarium's own Inkay plugin and module.
All three coexist.

### Extra setup for Roseverse

Skip all of this unless you're planning to use Roseverse.

**If you don't have a Roseverse account yet, set one up before this plugin goes anywhere
near your SD card.** The order matters:

1. Install Inkay-Roseverse.
2. Create your Roseverse account and check it actually works.
3. Swap Inkay-Roseverse out for Protarium's Inkay. Only one Inkay can be installed at a
   time, so this is a replacement rather than an addition.
4. Install this plugin.

Don't install this plugin until that account is set up and working.

**If you already have a Roseverse account**

Two things have to be on the SD card.

**Rose's module.** Download `Inkay-Roseverse.zip` from Project Rose's
[latest release](https://github.com/Project-Rose/Inkay-Roseverse/releases/latest), not the
Source code ZIP, and unzip it. You'll get two files, `Inkay-pretendo.wms` and
`Inkay-pretendo.wps`. Only the `.wms` is needed, so ignore the `.wps`. Rename
`Inkay-pretendo.wms` to `roseverse.wms` and put it at `sd:/wiiu/roseverse.wms`. That's the
`wiiu` folder, not the modules folder, and that's deliberate: a second module exporting the
same symbols under the same name as the one already installed is exactly the collision this
whole approach avoids. Then hit **Import from Roseverse** in the menu, and
**Roseverse Import Status** will tell you how it went, and what to fix if it didn't.

Once it reads **Imported**, you can delete `sd:/wiiu/roseverse.wms`. The import keeps what it
read in this plugin's settings, so the file only matters again if you import from a newer
copy of Rose's module. Leave your user key below where it is, though: that one gets read
every time.

**Your user key.** If you've already set Roseverse up on this console and used it, you've
got one and there's nothing to do here. Otherwise grab it from
[Project Rose's FAQ](https://miiverse.projectrose.cafe/guide/faq) and follow their steps:

- Download the Wii U user key.
- Put your Wii U SD card in your device, or set up FTPiiU if you've got it.
- Put the `.txt` key file in `sd:/wiiu/olive`, replacing the existing one if there is one.
- Open your profile and try opening Miiverse. If you went the FTPiiU route, restart the
  console first.

## Opening the menu

Everything here lives in Aroma's plugin config menu. Press **L + D-Pad Down + − (Minus)**
on your controller or GamePad, then pick **JustGetMiiOnline** from the list.

Back out of the menu when you're done. If you changed any of the three settings, or ran the
WaraWara Plaza reset, the console relaunches at that point, which is what makes the change
take.

## The settings

| Setting | Choices |
|---|---|
| Account server, `Network Server` | Protarium or Pretendo |
| Miiverse, `Miiverse Service` | Protaverse, Juxt or Roseverse |
| SpotPass, `SpotPass Server` | the account server's own, or Roseverse's (only on Roseverse) |

**On the defaults it writes nothing at all.** Not Protarium's own values written back,
nothing: every path checks the setting before it touches memory. Installing this and
leaving it alone should be indistinguishable from not installing it.

Change any of the three, or run the WaraWara Plaza reset, and the console relaunches when you
back out of the menu, which is what keeps a change whole. Back out having changed nothing and
nothing happens. Every boot shows a toast naming the account server and Miiverse, like
`Protarium + Protaverse Enabled`.

### Account server

Decides which Miiverse a console with nothing stored starts on. **Picking Pretendo moves
matchmaking with it.** Your friend list stays put, same NNID either way.

Here's what you'd be walking away from. Protarium serves all of these:

| Title | Title |
|---|---|
| Call of Duty: Black Ops II | Sonic & All-Stars Racing Transformed |
| FAST Racing NEO | Splatoon: Splatfests |
| Hyrule Warriors | Splatoon Global Testfire |
| Lost Reavers | Super Mario Maker: 100 Mario Challenge |
| Mario & Sonic at the Rio 2016 Olympic Games | Super Smash Bros. for Wii U: Conquest |
| Mario & Sonic at the Sochi 2014 Olympic Winter Games | Terraria |
| Mario Tennis: Ultra Smash | Trine 2 |
| Monster Hunter 3 Ultimate | Wii Sports Club |
| Puyo Puyo Tetris | Wii U Chat |

That list keeps growing, so check with Protarium for anything added since.

Protarium passes through to Pretendo-only titles anyway, so switching gives up the list
above and buys back nothing you didn't already have. The only real reason to go is a title
both networks serve, and even then only if your friends aren't on Protarium for it yet, or
you specifically want Pretendo's matchmaking on it, say for an event they're running.

### Miiverse

All three are selectable under either account server, and switching account servers leaves
this where it is, **including Juxt on Protarium and Protaverse on Pretendo.** Each of those
takes its token from its own account server, so on those two pairings I route that one
sign-in request to the Miiverse's own account server and leave everything else alone.

**Roseverse is Project Rose's Miiverse revival**, and it isn't paired to an account server
at all. RosePatcher can sit alongside this if you want their implementation of TVii.

### SpotPass

A selector only on Roseverse, the one Miiverse with a SpotPass of its own, and under
Protarium only once **Advanced Overrides** is on, which sits off by default near the foot of
the Debug page. One server per console, so having either is not having the other:
Protarium's carries Splatfests, Conquest and 100 Mario, Rose's carries the WaraWara Plaza of
the Miiverse in use.

## Building

devkitPPC and wut, plus five libraries devkitPro doesn't package: the plugin system,
libmocha, libkernel, libfunctionpatcher and libnotifications. The toolchain and the
`ppc-zlib` portlib come from devkitPro's package manager; build those five from source and
`make install` each one. Build libkernel with a plain `make`: its generated linker script
races under parallel make and the build fails on a missing file.

```bash
make
```

The Dockerfile pins the same versions. Its images are linux/amd64 only, so on Apple Silicon
they run emulated and a native toolchain is a lot faster.

## Disclaimer

This is provided as is, with no warranty of any kind. It changes which servers your console
signs in against, which is not something the Wii U was ever built to have changed underneath
it. It works on my hardware. I can't promise anything about yours, and what you do with it
is your own risk.

## Built with AI assistance

The code, the docs and the reverse engineering behind this were all done with AI help. That
doesn't mean I did nothing and let it vibecode the whole thing unchecked. I directed,
reviewed and tested everything in here. This wasn't thrown together without planning,
thought or care.

If you'd rather not run software built that way, that's fair. I'd sooner you knew before you
install anything.

## License

GPL-3.0. This builds on work from other projects in the scene, Inkay and RosePatcher among
them. The game constants in it are numbers I measured off software that's already out there.

Roseverse's addresses and URLs come from upstream Inkay's own table, where both Protarium's
fork and Project Rose's inherited them, and Rose's hostnames are read out of
Inkay-Roseverse's copy of that table. Project Rose's obfuscation constant is not recorded in
this repository.
