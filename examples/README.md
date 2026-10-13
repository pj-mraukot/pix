# Example pictures

Each folder holds one picture in several formats:

- `.bmp`: uncompressed 24-bit RGB
- `.png`: lossless PNG, as saved (not re-optimized)
- `.jpg`: the lossy original, for some pictures
- `name.pix`: PIX, algorithm 1 (the header starts with `PIX` 0 `A1`)
- `name2.pix`: PIX, algorithm 2 (`PIX` 0 `A2`)

All PIX files decode to exactly the pixels of the `.bmp`. Open them with the viewer or decode them with `pix d name.pix name.bmp`.

## Sizes

Sizes are in bytes. The percentages compare each PIX file with the PNG.

| Folder | Size | BMP | PNG | JPG | PIX algorithm 1 | PIX algorithm 2 |
|---|---|---|---|---|---|---|
| `50purrs` | 768×512 | 1,179,702 | 921,426 | – | 844,781 (92%) | 656,975 (71%) |
| `alice` | 800×1104 | 2,649,654 | 1,018,661 | 102,933 | 909,909 (89%) | 699,492 (69%) |
| `alice2` | 1920×1040 | 5,990,454 | 372,857 | – | 315,781 (85%) | 99,996 (27%) |
| `CCCP` | 1168×784 | 2,747,190 | 1,608,234 | 472,557 | 1,444,322 (90%) | 1,123,265 (70%) |
| `flowers` | 1140×1140 | 3,898,854 | 864,354 | 209,001 | 544,120 (63%) | 365,318 (42%) |
| `IBM_VGA` | 1920×1040 | 5,990,454 | 114,673 | – | 76,425 (67%) | 45,487 (40%) |
| `paris` | 894×800 | 2,147,254 | 1,547,418 | – | 774,174 (50%) | 518,076 (33%) |
| `prince` | 640×432 | 829,494 | 47,151 | – | 31,335 (66%) | 22,923 (49%) |
| `sloth` | 1280×850 | 3,264,054 | 309,820 | – | 243,704 (79%) | 177,290 (57%) |
| `world` | 800×400 | 960,054 | 123,667 | – | 83,886 (68%) | 56,193 (45%) |
| `xkcd1` | 390×423 | 495,810 | 60,596 | – | 37,517 (62%) | 22,171 (37%) |
| `xkcd2` | 700×231 | 485,154 | 75,245 | – | 68,302 (91%) | 48,431 (64%) |
| `xkcd3` | 640×843 | 1,618,614 | 495,422 | 122,807 | 375,950 (76%) | 307,464 (62%) |
| `xkcd4` | 498×609 | 911,118 | 38,600 | – | 36,149 (94%) | 22,412 (58%) |
| `xkcd5` | 740×471 | 1,045,674 | 190,179 | – | 176,202 (93%) | 113,048 (59%) |
| `xkcd6` | 740×567 | 420,658 | 137,069 | – | 217,043 (158%) | 123,308 (90%) |

## Sources

These pictures are not part of the PIX source code: the BSD license in [`LICENSE`](../LICENSE) does not cover them.

### xkcd

The comics in `xkcd1` to `xkcd6` are by **Randall Munroe**, from [xkcd.com](https://xkcd.com). They are used under the [Creative Commons Attribution-NonCommercial 2.5 License](https://creativecommons.org/licenses/by-nc/2.5/), as described on [xkcd.com/license.html](https://xkcd.com/license.html). They are converted to the formats above but otherwise unchanged, and they may not be used commercially. Thank you, Randall, for making them free to share.

| Folder | Comic |
|---|---|
| `xkcd1` | [#3310 Rejected Space Mission Profiles](https://xkcd.com/3310/) |
| `xkcd2` | [#150 Grownups](https://xkcd.com/150/) |
| `xkcd3` | [#154 Beliefs](https://xkcd.com/154/) |
| `xkcd4` | [#155 Search History](https://xkcd.com/155/) |
| `xkcd5` | [#160 Penny Arcade Parody](https://xkcd.com/160/) |
| `xkcd6` | [#170 Turn Back](https://xkcd.com/170/) |

### Other pictures

- `50purrs`, `CCCP`: AI-generated pictures.
- `flowers`, `sloth`: from websites offering free images.
- `alice`: a cover of *Alice's Adventures in Wonderland* by Lewis Carroll.
- `alice2`: a screenshot of chapter I of *Alice's Adventures in Wonderland* on [Project Gutenberg](https://www.gutenberg.org).
- `IBM_VGA`: a screenshot of the IBM VGA 9×16 font page of [The Ultimate Oldschool PC Font Pack](https://int10h.org/oldschool-pc-fonts/).
- `paris`, `world`: maps.
- `prince`: a screenshot of the DOS game *Prince of Persia* running in DOSBox.
