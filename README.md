# PIX

PIX is a lossless image format for 24-bit RGB images. This repository contains:

- **`pix`**: a command-line encoder and decoder
- **`pixview`**: a Qt 6 image viewer that converts images to and from PIX
- **`pixview.exe`**: the same viewer for Windows, written with the Win32 API (one `.exe`, no DLLs to install)

## How the format works

A PIX file has a 20-byte header (magic, algorithm, width, height, data sizes) followed by the compressed data. The algorithm is stored as text, so a hex dump shows which one compressed the image: a file starts with `PIX` 0 `A1` or `PIX` 0 `A2`. Decoders handle both. The command-line tool encodes with algorithm 1 unless you ask for algorithm 2; the viewer saves with algorithm 2 by default.

### Algorithm 1: nibble codes and zlib

The image is processed in 16×16 blocks. Each pixel is written as a short code made of 4-bit *nibbles*. The encoder picks the shortest code that fits:

- repeat the last or next-to-last colour, or a run of the last colour
- copy the pixel above
- an index into a small per-block colour cache (80 colours), or a larger global cache (8192 colours). A colour pushed out of the small cache moves to the large one.
- a *residual*: the difference from the colour predicted from the pixels to the left and above (the MED predictor from JPEG-LS). An exact prediction costs 2 nibbles; small differences cost 3–6.
- a literal RGB colour (7 nibbles), which is added to one of the caches

The nibble stream is then compressed with zlib.

Images of any size are supported. When the width or height is not a multiple of 16, the encoder pads the image to whole blocks: the last pixel of each row is repeated to the right, and the last row is repeated downwards. The file stores the original size, so the decoder drops the padding and returns the image at its original size.

### Algorithm 2: prediction and arithmetic coding

Algorithm 2 is not nibble based. Every decision is a yes/no event coded with an adaptive binary arithmetic coder, whose probabilities depend on the surroundings, so a well-predicted pixel costs a fraction of a bit. Pixels are coded row by row, without blocks. For each pixel it codes one of:

- one of up to 9 *candidate* colours: the predicted colour, the colour that followed the same neighbourhood last time (this repeats text and other shapes), or the colour of a neighbour (left, above, above-right, above-left, two to the left, two above)
- an index into the list of recently used colours (up to 16384, most recently used first)
- residuals of G, R and B. Each channel is predicted by a blend of five predictors, weighted by how well each one did around the pixel. R and B also use the error of G.

The encoder estimates the cost of each option from the current probabilities and picks the cheapest one.

Algorithm 2 makes files about 27% smaller than algorithm 1 on the test images, but it is slower: about 1 µs per pixel to encode and to decode. A 1280×960 photo takes about 1.3 s to encode and 1.1 s to decode, against 0.35 s and 0.06 s for algorithm 1.

### Compression

These are total file sizes for a set of 30 test images (photos, renders, and UI screenshots and graphics), compared with PNG saved with `optimize=True` in Pillow:

| Category | Optimized PNG | PIX algorithm 1 | PIX algorithm 2 |
|---|---|---|---|
| Photos (9) | 3.77 MB | 3.40 MB (90%) | 2.43 MB (64%) |
| Renders (8) | 6.29 MB | 5.77 MB (92%) | 4.42 MB (70%) |
| UI and graphics (13) | 4.46 MB | 2.86 MB (64%) | 1.89 MB (42%) |

Results vary per image. With algorithm 1 one photo is 2% larger than PNG; with algorithm 2 every image is smaller than PNG. Two images are slightly larger with algorithm 2 than with algorithm 1: a small gradient and a text-mode screenshot. Random noise is larger than raw RGB with both (algorithm 1: 113%, algorithm 2: 101%).

## Building

The tools are developed on Linux Mint / Ubuntu. Install the dependencies:

```
sudo apt install build-essential cmake zlib1g-dev libpng-dev libjpeg-dev qt6-base-dev
```

Build the command-line tool:

```
make
```

The program `pix` goes into the top directory; object files go into `build/`.

Build the viewer, which uses CMake and goes into `viewer/build`:

```
make viewer
```

Build the Windows viewer, cross-compiled with MinGW-w64 into `winview/build/pixview.exe`:

```
sudo apt install g++-mingw-w64-x86-64 libz-mingw-w64-dev
make winview
```

`make clean` removes all three builds.

### Why two C++ standards

The command-line tool and the codec are compiled as **C++14**. The coding style defines `typedef unsigned char byte` and requires `using namespace std;`. In C++17 those two clash with `std::byte`. Qt 6 needs C++17, so the viewer is compiled as C++17 and uses the codec only through `src/pix_api.h`, which avoids the `byte` typedef.

## Usage

### Command line

```
./pix e input.png output.pix         # encode a PNG, JPG or BMP image with algorithm 1
./pix e -a 2 input.png output.pix    # encode with algorithm 2
./pix d input.pix output.bmp         # decode to a 24-bit BMP (either algorithm)
./pix t [-a 2] input.png             # encode and decode in memory, compare, print statistics
./pix r input.png output.pix         # encode with algorithm 1, and write a listing of every code to input.txt
```

`pix e`, `pix t` and `pix r` print the file size and how often each code was used. With algorithm 2 they print how many pixels were coded each way and roughly how many bytes went to each part.

`pix r` produces the same `.pix` file as `pix e` and works with algorithm 1 only. It also writes a report next to the input file, with the extension replaced by `.txt`. The report lists every code like a debugger: nibble address, nibbles, code name, pixel(s), colour, and an explanation. The explanation covers cache slots and frequency changes, the MED prediction and residual, and why a literal colour was needed. For example:

```
00001EA0  C 4A90D9   NEW    (3,53)                    #4A90D9  -> cache1 slot 0 (143 pixels of this colour left in block); no residual code fits (pred #F4F4F3 dG=-100 dR=-70 dB=+74)
00001EE3  5 0        C1     (30,63)                   #4A90D9  cache1 slot 0, freq 0 -> 1
00000003  4 4F       RUN    (1,0)-(2,6) x98           #FFFFFF  last colour 98 times; not in cache1, no freq update
```

Reports are large, roughly 90 bytes per pixel (a 1280×960 photo gives about 110 MB), so use small images or crops.

### Viewer

```
viewer/build/pixview [file]
```

- **File → Open** (Ctrl+O): opens a PIX file or any image Qt can read (PNG, JPG, BMP, GIF, …). You can also drag a file onto the window.
- **File → Save As** (Ctrl+Shift+S): saves as PIX, PNG, BMP or JPEG, chosen by the file extension. For PIX, choose the algorithm in the file type list: *PIX algorithm 1* or *PIX algorithm 2*. When you pick a file type, the extension of the file name changes to match.
- **Convert → Image to PIX** (Ctrl+E): pick an image, then save it as `.pix`, choosing the algorithm in the file type list. The status bar shows the size, the ratio to raw RGB, the algorithm and the encode time.
- **Convert → PIX to Image** (Ctrl+D): pick a `.pix` file, then save it as PNG, BMP or JPEG.
- **Convert → PIX Algorithm**: the algorithm the save dialogs start with (2 by default). Choosing a PIX type in a save dialog also changes it. Opening works with both algorithms.
- **View**: zoom in and out, actual size (Ctrl+0), and fit to window (Ctrl+F; shrinks large images, on by default).

When an image has transparency, it is placed on a white background before encoding, in both tools.

### Windows viewer

`winview/build/pixview.exe` is a single file: copy it to any 64-bit Windows PC and run it. It has the same menus, shortcuts and file dialogs as the Qt viewer. Some differences:

- Images other than PIX are read and written by the Windows Imaging Component, which is part of Windows. It reads PNG, JPEG, BMP, GIF, TIFF, ICO and any codecs installed on the PC (such as WebP or HEIF), and writes PNG, BMP and JPEG.
- PNG files keep their transparency; PIX, BMP and JPEG files get the image on white.
- Ctrl + mouse wheel zooms, the wheel scrolls (Shift + wheel sideways), and you can drag a large image with the left mouse button. Arrow keys, Page Up/Down, Home and End scroll too.

It also runs under Wine. A Linux path on the command line works there too:

```
wine winview/build/pixview.exe ~/Pictures/photo.png
```

## Repository layout

```
src/            codecs (pix_codec: algorithm 1 and the file format, pix2_codec: algorithm 2),
                image loading (libpng, libjpeg, own BMP reader), command-line tool
viewer/         Qt 6 viewer (CMake)
winview/        Windows viewer (Win32 API, MinGW-w64; built into winview/build)
build/          object files of the command-line tool (made by make)
```

## License

PIX is released under the BSD 2-clause license, see [`LICENSE`](LICENSE): you may use, change and redistribute it freely, also in closed-source products, as long as the copyright notice and the license text are kept with the source code and reproduced in the documentation of binary distributions.
