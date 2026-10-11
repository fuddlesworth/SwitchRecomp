SwitchRecomp - static recompiler for Nintendo Switch games (ARM64 -> C -> native)
================================================================================

Same idea as GBRecomp / PSXRecomp: the game CODE (exefs) is translated to C once
and compiled to a native Windows program. The game DATA (romfs: models, textures,
music) is not compiled: the game reads it as is from the GAME_data\romfs folder.

Use only your own copy of the game. No games, keys or firmware are included.

License: Apache 2.0 (LICENSE.txt). Free to use, modify and share, also
commercially, but you must always credit "SwitchRecomp by MZ Labs" and keep
NOTICE.txt in any copy, fork or derived work. Details in NOTICE.txt.

--------------------------------------------------------------------------------
1) RECOMPILE A GAME
--------------------------------------------------------------------------------
  Nothing to install: Python, TCC and GCC are bundled (python\, tcc\, gcc\).

  a) Dump the game from your own console and extract it with hactool and your
     own keys:
        hactool -k prod.keys --exefsdir=exefs --romfsdir=romfs  game.nca
     With an update, extract the combined romfs:
        hactool -k prod.keys --basenca=base.nca --exefsdir=exefs --romfsdir=romfs  update.nca
     You end up with a folder that has exefs\ and romfs\ inside.

  b) Open SwitchRecomp.exe
     - "Choose..." -> the game folder (the one with exefs\ and romfs\)
     - Compiler:  TCC    compiles in seconds, the game runs slower
                  GCC    takes longer to compile, the game runs faster
     - "Recompile", then "Play". The game is written to the games\ folder.

  Command line:
     python\python.exe switchrecomp.py C:\path\to\game -o MyGame
     python\python.exe switchrecomp.py homebrew.nro
     Options: --native (optimize for your CPU)  -O 3  --jobs N  --cc tcc

  A big game takes several minutes with GCC the first time. Only once: if the
  code did not change, the next run reuses what was already compiled.

  Output:
     MyGame.exe          the runtime (kernel, services, GPU -> OpenGL, video)
     MyGame_code.dll     the game code translated to x86-64
     MyGame_data\        exefs\ + romfs\ + save data
     MyGame.ini          options

  The exe and the dll must come from the same game version. If you update the
  game, recompile.

--------------------------------------------------------------------------------
2) PLAY
--------------------------------------------------------------------------------
  Requirements: Windows 10/11 64-bit, GPU with OpenGL 4.3 or newer (keep the
  driver up to date). Linux and the Steam Deck work through Proton.

  Controls
    Xbox / XInput controller: same as the Switch one, by position.
    Keyboard: Arrows = D-Pad, WASD = left stick, IJKL = right stick,
              X/Space = A, Z = B, C = X, V = Y, Q = L, E = R, 1 = ZL, 3 = ZR,
              Enter = +, Backspace = -
    F1 help   F3 screenshot   F4 FPS limit 60 / unlimited   F5 diagnostic trace
    F7 record .mp4 video   F8 full resolution PNG   F11 or Alt+Enter fullscreen
    Esc quit

  Options (MyGame.ini, rewritten on every start with all the available keys):
    docked=1               TV mode (1080p) instead of handheld (720p)
    resolution=1           internal resolution scale (0.25 .. 4)
    fps_limit=1            1 = 60 FPS like the console, 0 = unlimited
    shadows=1              0 skips shadow maps (faster on weak GPUs)
    lsfg=1                 frame generation with YOUR OWN copy of Lossless
                           Scaling (its Lossless.dll is read at run time, it is
                           never included or copied)
    log=2                  log everything (for bug reports)

  If something goes wrong: send MyGame.log (next to the exe). The F5 trace
  goes to the captures\ folder. performance.txt summarizes slow frames.

--------------------------------------------------------------------------------
Third-party components
--------------------------------------------------------------------------------
  OpenH264 (Cisco, BSD 2-clause)        game videos   runtime\oh264\LICENSE
  SPIRV-Cross (Khronos, Apache 2.0)     LSFG shaders  runtime\spvc\LICENSE-SPIRV-Cross.txt
  texture2ddecoder (MIT)                BCn textures  runtime\bcn\LICENSE_texture2ddecoder.txt
  TCC (LGPL)                            tcc\COPYING.txt
  Python (PSF license)                  python\LICENSE_PYTHON.txt
  GCC / MinGW-w64 (GPL with the GCC runtime library exception)

  The Apache 2.0 license in LICENSE.txt covers only the SwitchRecomp code.
  Each component above keeps its own license.
