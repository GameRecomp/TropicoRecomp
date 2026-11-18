# Tropico 2: the full screen box, and text fields

What `games/tropico2/patch_fullscreen.c` and `patch_textedit.c` change and
why. Addresses are in `Tropico2.exe` (GOG 2.1.0.14).

## Full screen

`FullScreen` in `Tropico2.ini` is a preference whose value is at
`0x009b7c24`. The game's windowed mode does not work here: with
`FullScreen=0` the game stops at the start ("Failed trying to set screen
set to 640 x 480 x 16 bpp"). Its two switches:

- `0x008d1130` (thiscall, two arguments it does not read): the full screen
  (a display mode of the first kind, a popup window); sets the preference
  to 1.
- `0x008d11d0` (thiscall: width and height, 0 for the current): a window
  of that size (the second kind of mode); sets it to 0.

The Full Screen box, on the settings' graphics page in the game and on the
settings page of the main menu, calls the first when ticked and the second
when not (returning to `0x006d2fa3`/`0x006d2fb5` and
`0x006dc0ca`/`0x006dc0dc`), then reads the preference to tick itself
(`0x006d2fb7`, `0x006dc0de`); the pages read it when they are built
(`0x006d0601`, `0x006dbbee`). `0x00867540` toggles between the two.

So, as for Tropico ([tropico1-options.md](tropico1-options.md)), the game
always stays in its full screen, and the box is the host's full screen
(`rc_display.fullscreen`, and settings.ini): those calls from the box (and
the toggle) set the host's and do nothing else; any other call for a window
gets the game's full screen; the pages read the host's state; and the
start-up, which reads the preference first when it makes the game's window
(`0x008d0d0d`), finds 1, so that an ini file saved with `FullScreen=0`
starts too. On Android the box stays ticked.

## Text fields

A text field (a new pirate king's name, a saved game's) is a window with
an edit behaviour: class `BFilterEditText` (vtable `0x0093924c`), its
window at `+0x08`, made by the dialogs that ask for a name
(`0x006a0319`, `0x006bcb72`). Behaviours are attached to their window by
`0x008eb180` and detached by `0x008eb670` (methods of every behaviour). The
dialog's field is typed into at once, and the behaviour is detached when
the dialog closes. Keys come to it as messages (`'kydn'`, `'char'`,
`'kyup'`) through the focus of the dialog's window (`+0xd8` of a `TWindow`).

So the field is typed into while its behaviour is attached and its window
shown; `patch_textedit.c` tells the host its rectangle (where it is drawn,
`tropico2_drawn_by`), for an on-screen keyboard (`rc_plat_text_input`).

Unlike Tropico's ([tropico1-textedit.md](tropico1-textedit.md)), a click on
OK while a name is typed works: the dialog saves under that name.
