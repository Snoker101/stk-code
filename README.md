# SuperTuxKart
[![Linux build status](https://github.com/supertuxkart/stk-code/actions/workflows/linux.yml/badge.svg)](https://github.com/supertuxkart/stk-code/actions/workflows/linux.yml)
[![Apple build status](https://github.com/supertuxkart/stk-code/actions/workflows/apple.yml/badge.svg)](https://github.com/supertuxkart/stk-code/actions/workflows/apple.yml)
[![Windows build status](https://github.com/supertuxkart/stk-code/actions/workflows/windows.yml/badge.svg)](https://github.com/supertuxkart/stk-code/actions/workflows/windows.yml)
[![Switch build status](https://github.com/supertuxkart/stk-code/actions/workflows/switch.yml/badge.svg)](https://github.com/supertuxkart/stk-code/actions/workflows/switch.yml)
[![#supertuxkart on the libera IRC network](https://img.shields.io/badge/libera-%23supertuxkart-brightgreen.svg)](https://web.libera.chat/?channels=#supertuxkart)

SuperTuxKart is a free kart racing game. It focuses on fun and not on realistic kart physics. Instructions can be found on the in-game help page.

The SuperTuxKart homepage can be found at <https://supertuxkart.net/>. There is also our [FAQ](https://supertuxkart.net/FAQ) and information on how get in touch with the [community](https://supertuxkart.net/Community).

The latest release binaries can be found [here](https://github.com/supertuxkart/stk-code/releases/latest), and those of preview releases [here](https://github.com/supertuxkart/stk-code/releases/preview).

# Snoccer: improved offline bots
This branch, `improved_offline_bots`, is the Snoccer server version with much stronger soccer bots for offline play. Online bots are not changed yet.

It still includes the advanced ranking system for soccer (to the best of our knowledge the most advanced publicly available ranking system for STK soccer).

## The bots
- **Difficulty:** Novice uses the original simple bots. Intermediate, Expert and SuperTux use the new ones. On SuperTux they also jump at the ball and pick light karts. Bots get no extra speed, nitro or items: they win on skill alone.
- **Teamwork:** each bot has a job (striker, goalie, support or attacker), and the jobs change during play. At a kickoff the bot closest to the ball takes it. A goalie that clears the ball and is still on it keeps attacking, and a teammate takes the goal.
- **Playing the ball:** bots predict where the ball is going, drive around it to hit it toward the goal, and use nitro and drifting to shoot. They also fire bowling balls at the ball.
- **Goalkeeping:** the goalie comes out to meet shots and reacts to bowling balls aimed at the ball or already flying at it.
- **Human teammates:** when you have the ball, your bots leave it to you and stay out of your way.
- **Karts:** bots never pick Sara the Racer in soccer.

## Options for testing the bots
- `--soccer-log`: at each goal in offline soccer, writes the last 10 seconds of play (ball, karts, controls and what each bot was doing) to the `soccer_log` folder in the config directory.
- `--soccer-ai-stats`: plays a 4 vs 4 bot match without a player and logs the result.
- `--soccer-ai-baseline=red|blue|both`: with `--soccer-ai-stats`, that team uses the original simple bots, to compare old and new.
- `--soccer-ai-goals=N`: with `--soccer-ai-stats`, the match ends when a team has N goals.

# Important
This version uses SQLite3 DB to save ranking data, on your system you have to install the package libsqlite3-dev. For debian: "sudo apt install libsqlite3-dev"

## Hardware Requirements
Any hardware that supports OpenGL >= 3.3 or OpenGL ES >= 3.0 should be able to run SuperTuxKart. For Android, Android 5.0 or greater is required.

This includes dedicated GPUs from Nvidia or AMD released after 2010, integrated GPUs released after 2012 and Android phones released after 2014.

The game will run on some very weak devices, but a better GPU allows to run smoothly using higher quality graphics and a higher resolution, and a better CPU helps to keep a smooth playing experience in online multiplayer.

Please check the "What are the hardware requirements?" question in our [FAQ](https://supertuxkart.net/FAQ) for more information.

## License
The software is released under the GNU General Public License (GPL) which can be found in the file [`COPYING`](/COPYING) in the same directory as this file.

---

## Building from source

Building instructions can be found in [`INSTALL.md`](/INSTALL.md)

## Contributing code

**To contribute code to the official STK repository, please review the ['How to contribute code'](https://supertuxkart.net/How_to_contribute_code) guide.** It contains important guidelines to make the process smoother and maximize the chances that your contribution is accepted.

## 3D coordinates
A reminder for those who are looking at the code and 3D models:

SuperTuxKart: X right, Y up, Z forwards

Blender: X right, Y forwards, Z up

The export utilities perform the needed transformation, so in Blender you just work with the XY plane as ground, and things will appear fine in STK (using XZ as ground in the code, obviously).
