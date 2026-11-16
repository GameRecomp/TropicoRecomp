<!-- SPDX-License-Identifier: MIT -->
# Tropico: text fields and the mouse

A text field (the name of a saved game, among others) is edited in a loop
of its own, in the function at 0x503260, until Enter or Esc. Clicking a
dialog's OK while a name was being edited did nothing; only Enter worked.
This is how the game itself behaves; `games/tropico1/patch_textedit.c`
changes it.

## The input queue

The window procedure (0x5278f0 for the mouse, 0x527b80 for keys) turns the
messages into events of nine dwords in a queue at 0x61b230 (30 at most,
the count at 0x61b668), oldest first:

| type | event |
|---|---|
| 2, 3 | left button down, up |
| 4, 5 | right button down, up |
| 6 | the mouse moved |
| 7, 8 | key down, up: +4 the virtual key, +8 the character (ToAscii) |
| 9 | the wheel |
| 10, 11 | middle button down, up |

The screen's mouse coordinates follow, and every event carries the state
of the buttons and of Shift, Ctrl and Alt (0x61b66c). 0x527790 takes the
first event (mouse moves run together); 0x4e7800 does too, through the
filter of hot keys.

## The field's loop

Clicking a field hides the cursor (0x4f8d40, called at 0x5038dd), then:

- 0x5038e2: the loop. It lets the system run (0x516ff0), reads an event
  with 0x4e7800 into its frame (esp+0x24), and at 0x503a11 compares the
  type with 7. Anything but a key down goes to 0x503f8b, which only looks
  whether the loop is to go on: mouse events are dropped.
- Keys are taken by the character at +8 (esp+0x2c), with +0xc (esp+0x30)
  beside it (Tab is made Enter in both when the field asks for it). Enter
  ends the loop and keeps the text (0x503d18); Esc puts the old text back
  and ends it (0x503b65).
- After the loop the cursor is shown again (0x503fb9).

## The patch

A patch point at 0x503a11: when the event read is a left button down, the
event is made an Enter (type 7, character 13 at +8 and +0xc), and the click
is put back at the front of the queue. The editing ends as with Enter, and
the dialog's own loop gets the click next: OK saves under the new name,
Cancel cancels, as they do when no name is being edited.
