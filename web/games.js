// SPDX-License-Identifier: MIT
// The games the converter makes builds of, as the page, the worker and the
// player name them. The id is the converter's (games/<id>/game.toml) and
// the directory the browser stores the game in.
export const GAMES = {
  tropico1: {
    id: 'tropico1',
    name: 'Tropico',
    stem: 'Tropico',
    installer: 'setup_tropico_2.1.0.14.exe',
    installerSize: 'about 820 MiB',
    size: 'about 1.3 GiB',
    web: 'tropico',
  },
  tropico2: {
    id: 'tropico2',
    name: 'Tropico 2',
    full: 'Tropico 2: Pirate Cove',
    stem: 'Tropico2',
    installer: 'setup_tropico2_2.1.0.14.exe',
    installerSize: 'about 1.1 GiB',
    size: 'about 1.9 GiB',
    web: 'tropico2',
  },
};

/* The game a page is for: ?game=<id> (or #<id>), else the default. */
export function gameOf(location) {
  const p = new URLSearchParams(location.search).get('game') || location.hash.replace(/^#/, '');
  return GAMES[p] || GAMES.tropico1;
}
