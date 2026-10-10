#!/usr/bin/env python3
"""Writes the three declarations a package carries - waterbox.config,
file_slots.json and default_keybinds.json - from the one description below.
The panel here must be the panel wbx-entry.cpp binds (the gate checks the two
agree), and nothing is hand-edited in the generated files.

Usage: gen-config.py [OUTDIR]   (default: this script's folder)
"""
import json
import os
import sys

out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

BUTTONS = ["A", "B", "X", "Y", "Up", "Down", "Left", "Right", "L", "R", "Start", "Select",
           "ZL", "ZR", "Touch"]

# The letter each button writes into a movie's text and heads its input column
# with. The frontend keeps no table of these: a core says what its own controls
# are called. (An entry is read by position, so a letter may change and no
# movie made before it is harmed.)
MNEMONICS = {
    "A": "A", "B": "B", "X": "X", "Y": "Y", "Up": "U", "Down": "D", "Left": "L", "Right": "R",
    "L": "l", "R": "r", "Start": "S", "Select": "s", "ZL": "[", "ZR": "]", "Touch": "T",
}
assert sorted(MNEMONICS) == sorted(BUTTONS), "every button has a letter, and nothing else does"
assert len(set(MNEMONICS.values())) == len(MNEMONICS), "and no two share one"

# name, least, most, at rest, and the header of its input column
AXES = [
    ("Circle Pad X", -128, 127, 0, "CPX"),
    ("Circle Pad Y", -128, 127, 0, "CPY"),
    ("C-Stick X", -128, 127, 0, "CSX"),
    ("C-Stick Y", -128, 127, 0, "CSY"),
    ("Touch X", 0, 65535, 32768, "TX"),
    ("Touch Y", 0, 65535, 32768, "TY"),
    ("Accel X", -4000, 4000, 0, "AX"),
    ("Accel Y", -4000, 4000, 0, "AY"),
    ("Accel Z", -4000, 4000, -1000, "AZ"),
    ("Gyro X", -20000, 20000, 0, "GX"),
    ("Gyro Y", -20000, 20000, 0, "GY"),
    ("Gyro Z", -20000, 20000, 0, "GZ"),
]

INPUT_NAME = "Nintendo 3DS"

GAME_FORMATS = ["3ds", "cci", "cxi", "app", "3dsx", "elf"]

config = {
    "coreName": "Azahar",
    "systemId": "3DS",
    # what the system is called in front of a person: the core's word for it
    "systemNames": {"3DS": "Nintendo 3DS"},
    "author": "The Citra and Azahar teams; chimera port by Sergio Martin",
    "url": "https://github.com/ToolAssisted-run/chimera-core-azahar",
    "romFile": "game",
    "deterministic": True,
    "memoryLayoutMiB": [256, 16, 40, 256, 3072],
    "_memoryLayoutMiB_note": "sbrk, sealed, invisible, plain, mmap. The console's 256 MB of FCRAM (a New 3DS's; an old one uses the first 128), VRAM and the DSP's RAM are Azahar's own allocations, so they land on the mmap heap together with the JIT's code caches and the machine's own filesystem - the NAND, the SD card and every save, which live in guest memory so a savestate carries them. The game itself is never copied in: it is read from its mounted file.",
    "video": {
        "_comment": "By default the two screens stacked, the way the console is held: the 400x240 top screen above the 320x240 bottom one, centred, 400x480. A top screen in the 800-pixel wide mode some 2D games use is shown at 400, each pair of its pixels averaged. The Screen Layout settings put the screens elsewhere and Internal Resolution draws them larger, so width and height here are the most a side can be - the two screens side by side at 4x, or that turned upright - and the picture's own size comes with each frame. No virtual size is declared: a 3DS's pixels are square, so the picture is its own shape.",
        "width": 2880,
        "height": 2880,
        "vsyncNumerator": 268111856,
        "vsyncDenominator": 4481136,
        "gpuStatesSurviveTheContext": True,
        "_comment_gpuStates": "TRUE, and measured so (the gate's GPU legs). Under opengl-hw the renderer keeps the context id its GL objects came from; when a state lands on another context it throws the whole renderer away and makes it again, its caches filling back up from the console's memory - which holds everything the GPU drew, because every frame ends by flushing it there. A run loaded into a new host, or loaded around every frame, is byte-identical to one that never stopped. Above 1x the console's memory holds the pictures scaled down, so the core also copies every surface of the renderer's cache into its own memory before each state (the StateSaving export) and gives them back to the renderer it makes after a load.",
        "drawEveryFrame": True,
        "_comment_drawEveryFrame": "The core draws every frame whatever it is told: under opengl-hw the frame's end flushes the GPU's pictures into the console's memory, and a frame not drawn would leave that memory short.",
    },
    "audio": {
        "_comment": "The DSP's own output, 32728 Hz stereo, handed over as it makes it: about 547 pairs a frame, in 160-pair blocks, so a frame carries 480 or 640.",
        "rate": 32728,
        "samplesPerFrame": 2048,
        "channels": 2,
        "get": "GetAudio",
    },
    "lag": {"inputWasRead": "InputWasRead"},
    "extensions": {"." + f: "3DS" for f in GAME_FORMATS},
    "input": {
        "name": INPUT_NAME,
        "_comment": "The console's controls. The ZL and ZR buttons and the C-Stick are a New 3DS's and leave the input roll on an old one. The Circle Pad and C-Stick run -128..127 with up and right positive. The touch screen is the Touch button plus a point on the WHOLE stacked picture (Touch X/Y, 0..65535 across its 400 columns and down its 480 rows, as every absolute position in Chimera is): a point on the bottom screen touches it, anywhere else touches nothing. The accelerometer (thousandths of a g, resting at -1000 on Z) and gyroscope (tenths of a degree a second) take input only when the Motion setting is on.",
        "buttons": BUTTONS,
        "mnemonics": MNEMONICS,
        "axes": [{"name": n, "min": lo, "max": hi, "neutral": mid, "header": head}
                 for n, lo, hi, mid, head in AXES],
    },
    "settings": [
        {
            "name": "model",
            "display": "Model",
            "type": "enum",
            "options": ["new3ds", "old3ds"],
            "default": "new3ds",
            "description": "Which console this is: a New Nintendo 3DS (256 MB of "
                "memory, a faster processor mode, the ZL and ZR buttons and "
                "the C-Stick) or the original Nintendo 3DS. Azahar's default"
                " is the New 3DS, which runs every game. A game made for the"
                " original model may be timed differently on the new one. It"
                " is part of the machine, so a movie needs the same model.",
        },
        {
            "name": "region",
            "display": "Region",
            "type": "enum",
            "options": ["auto", "jpn", "usa", "eur", "aus", "chn", "kor", "twn"],
            "default": "auto",
            "description": "The console's region. 'auto' uses the game's own region. A "
                "game can read it, so it is part of the machine.",
        },
        {
            "name": "rtc_start",
            "display": "Clock at Power-On",
            "type": "int",
            "default": 946684800,
            "min": 946684800,
            "max": 2145916800,
            "description": "What the console's clock shows when the machine starts, in "
                "seconds since 1970-01-01 (UTC). The default is 2000-01-01 "
                "00:00:00, the earliest date a 3DS can be set to. After the "
                "start the clock runs with the emulated console and never "
                "with your computer. A game that takes its random numbers or"
                " its calendar from the clock plays differently with another"
                " value, so it is part of the machine.",
        },
        {
            "name": "cpu",
            "display": "CPU",
            "type": "enum",
            "options": ["jit", "interpreter"],
            "default": "jit",
            "description": "How the console's ARM11 processor is emulated: with "
                "dynarmic's recompiler, which is fast, or with Azahar's "
                "interpreter. Both are meant to give the same result. The "
                "interpreter is there to find out whether a problem comes "
                "from the recompiler or from the game.",
        },
        {
            "name": "cpu_clock",
            "display": "CPU Clock (%)",
            "type": "int",
            "default": 100,
            "min": 5,
            "max": 400,
            "description": "The speed of the ARM11 processor as a percentage of the "
                "real console's (Azahar's CPU clock setting). Above 100, a "
                "game that slows down in busy scenes slows down less. It "
                "changes how the game runs, so a movie needs the same value.",
        },
        {
            "name": "renderer",
            "display": "Renderer",
            "type": "enum",
            "options": ["software", "opengl-hw"],
            "default": "software",
            "description": "Which renderer draws the picture. 'software' is Azahar's "
                "own software renderer and the default. It runs completely "
                "inside the sandbox, so the picture, and anything the game "
                "reads back from it, is the same on every computer. 'opengl-"
                "hw' is Azahar's OpenGL renderer drawing on your REAL "
                "graphics card through the bridge. It is many times faster. "
                "The graphics card is outside the sandbox and differs "
                "between computers, and what a game reads back from its "
                "picture goes into the console's memory, so a movie recorded"
                " this way plays back only on the same graphics driver. On a"
                " computer that offers no OpenGL the core draws in software "
                "and says so.",
        },
        {
            "name": "internal_resolution",
            "display": "Internal Resolution",
            "type": "enum",
            "options": ["1x", "2x", "3x", "4x"],
            "default": "1x",
            "description": "How many times the console's own resolution the OpenGL "
                "renderer draws at (Azahar's Internal Resolution). At 2x the"
                " stacked picture is 800x960. The software renderer always "
                "draws at 1x. This is part of the machine and not only of "
                "the picture. What the graphics card draws is written back "
                "into the console's memory reduced to the console's size, "
                "and those bytes differ from the ones a 1x frame leaves. A "
                "movie therefore needs the resolution it was made with. "
                "Above 1x every savestate also holds the renderer's pictures"
                " at full size, so states are larger and saving one takes "
                "longer.",
        },
        {
            "name": "username",
            "display": "User Name",
            "type": "string",
            "default": "",
            "description": "The user name in the console's own settings, one to ten "
                "characters. Games read it, for example as the owner of a "
                "save file or in a greeting. It is part of the machine, so a"
                " movie made with one name needs that name. Left empty, it "
                "is Azahar's default, AZAHAR.",
        },
        {
            "name": "layout",
            "display": "Screen Layout",
            "type": "enum",
            "options": ["stacked", "single", "large", "side-by-side"],
            "default": "stacked",
            "description": "How the two screens are placed in one picture (Azahar's "
                "Screen Layout). 'stacked' puts the top screen above the "
                "bottom one (400x480). 'single' shows the top screen alone "
                "(400x240). 'large' shows one screen at full size with the "
                "other one small next to it, and Large Screen Proportion "
                "says how small. 'side-by-side' shows both at full size "
                "(720x240). Swap Screens exchanges the two screens in any "
                "layout. The layout changes the picture only, with one thing"
                " to know. The Touch axes are a position in the picture, so "
                "a touch lands where the layout puts the bottom screen. A "
                "movie that uses touch needs the layout it was made with, "
                "and with the top screen alone nothing can be touched.",
        },
        {
            "name": "swap_screens",
            "display": "Swap Screens",
            "type": "bool",
            "default": False,
            "description": "Exchanges the two screens in the layout. With 'single', the"
                " bottom screen is shown alone (320x240). With 'large', the "
                "bottom screen is the large one.",
        },
        {
            "name": "upright",
            "display": "Upright Screens",
            "type": "bool",
            "default": False,
            "description": "Shows the console turned on its side, for games that are "
                "held like a book. The picture is turned a quarter turn, and"
                " its width and height change places.",
        },
        {
            "name": "large_screen_proportion",
            "display": "Large Screen Proportion",
            "type": "int",
            "default": 4,
            "min": 1,
            "max": 16,
            "description": "With the 'large' layout, how many times larger the large "
                "screen is than the small one. Azahar's default is 4.",
        },
        {
            "name": "linear_filter",
            "display": "Linear Filtering",
            "type": "bool",
            "default": True,
            "description": "Whether the OpenGL renderer smooths a screen when it scales"
                " it into the picture (Azahar's Enable Linear Filtering). "
                "The difference shows where a screen is not at a whole "
                "multiple of its own size, such as the small screen of the "
                "'large' layout. It changes the picture only.",
        },
        {
            "name": "motion",
            "display": "Motion Controls",
            "type": "bool",
            "default": False,
            "description": "Whether the motion sensors (accelerometer and gyroscope) "
                "take input through the six Accel and Gyro axes. With this "
                "off the console lies still and flat, and those axes are "
                "removed from the input columns.",
        },
        {
            "name": "aes_keys",
            "display": "AES Keys",
            "type": "bool",
            "default": False,
            "description": "Whether the project includes the console's AES keys "
                "(aes_keys.txt, dumped from your own console). A decrypted "
                "game needs none, and this package includes none. They are "
                "needed to install and run .cia content, for amiibo and for "
                "some online-account features.",
        },
        {
            "name": "seeddb",
            "display": "Seed Database",
            "type": "bool",
            "default": False,
            "description": "Whether the project includes seeddb.bin, the per-game seeds"
                " that some eShop games are encrypted with. It is only used "
                "together with AES Keys.",
        },
    ],
    "firmware": [
        {
            "id": "aes_keys.txt",
            "display": "AES keys (aes_keys.txt)",
            "description": "The console's AES keys in Azahar's text format, dumped from"
                " your own console. Azahar looks for them in its system data"
                " folder, and the core puts them there.",
            "name": "aes_keys.txt",
            "requiredWhen": {"setting": "aes_keys", "is": True},
        },
        {
            "id": "seeddb.bin",
            "display": "Seed database (seeddb.bin)",
            "description": "The seeds that some eShop games are encrypted with.",
            "name": "seeddb.bin",
            "requiredWhen": {"setting": "seeddb", "is": True},
        },
    ],
}

slots = {
    "_comment": "This file lists the kinds of file a 3DS project can hold. Chimera's"
        " New Project window is built from it. A project is one decrypted "
        "game, and optionally what the game already saved.",
    "slots": [
        {
            "id": "game",
            "title": "Game",
            "min": 1,
            "max": 1,
            "formats": GAME_FORMATS,
            "help": "A DECRYPTED dump. It can be a cartridge (.3ds or .cci), an "
                "executable (.cxi or .app) or homebrew (.3dsx or .elf). "
                "Azahar does not decrypt. An encrypted dump is refused with "
                "a message that says so. An old dump that was decrypted but "
                "whose header still says 'encrypted' is recognised and runs.",
        },
        {
            "id": "savedata",
            "title": "Save data",
            "min": 0,
            "max": 1,
            "formats": ["zip"],
            "help": "What the game already saved, as Emulator > Export Save "
                "Data... wrote it. It is a .zip of the SD card's 'Nintendo "
                "3DS' folder (the game's save data and extra data). It is "
                "put back onto the machine's SD card before the game starts.",
        },
    ],
}

pad = {
    "A": "X, J1 B2, X1 B",
    "B": "Z, J1 B1, X1 A",
    "X": "S, J1 B4, X1 Y",
    "Y": "A, J1 B3, X1 X",
    "Up": "Up, J1 POV1U, X1 DpadUp",
    "Down": "Down, J1 POV1D, X1 DpadDown",
    "Left": "Left, J1 POV1L, X1 DpadLeft",
    "Right": "Right, J1 POV1R, X1 DpadRight",
    "L": "Q, J1 B5, X1 LeftShoulder",
    "R": "W, J1 B6, X1 RightShoulder",
    "Start": "Enter, J1 B10, X1 Start",
    "Select": "Backspace, J1 B9, X1 Back",
    "ZL": "E, J1 B7, X1 LeftTrigger",
    "ZR": "R, J1 B8, X1 RightTrigger",
    "Touch": "WMouse L",
}
analog = {
    "Circle Pad X": {"Value": "X1 LeftThumbX Axis", "Mult": 1.0, "Deadzone": 0.1},
    "Circle Pad Y": {"Value": "X1 LeftThumbY Axis", "Mult": 1.0, "Deadzone": 0.1},
    "C-Stick X": {"Value": "X1 RightThumbX Axis", "Mult": 1.0, "Deadzone": 0.1},
    "C-Stick Y": {"Value": "X1 RightThumbY Axis", "Mult": 1.0, "Deadzone": 0.1},
    "Touch X": {"Value": "WMouse X", "Mult": 1.0, "Deadzone": 0.0},
    "Touch Y": {"Value": "WMouse Y", "Mult": 1.0, "Deadzone": 0.0},
}
keybinds = {
    "_comment": [
        "The console on the keyboard and the first pad: the face buttons where a 3DS has them",
        "(A right, B bottom), the D-pad on the arrows, the Circle Pad and C-Stick on the pad's",
        "sticks, and the touch screen under the mouse: its left button touches, where the",
        "pointer is on the picture. Motion is left unbound.",
    ],
    "AllTrollers": {INPUT_NAME: pad},
    "AllTrollersAutoFire": {INPUT_NAME: {}},
    "AllTrollersAnalog": {INPUT_NAME: analog},
}

for name, obj in (("waterbox.config", config), ("file_slots.json", slots),
                  ("default_keybinds.json", keybinds)):
    with open(os.path.join(out, name), "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
