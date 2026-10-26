# Icon, banner and .cia settings

- `../icon.png` (48x48): the HOME menu / Homebrew Launcher icon, used for both `dkr.3dsx` (SMDH) and `dkr.cia`.
- `banner.png` (256x128) and `banner.wav` (about 2 s, 16-bit stereo 44.1 kHz like other working homebrew banners; a 32 kHz mono 2.9 s file played as repeated beeps on hardware): the HOME menu banner and its sound, made into `dkr.bnr` by
  bannertool.
- `dkr.rsf`: makerom settings (title ID 000400000D4B5200, SD card access, New 3DS 804 MHz + 124 MB mode).

Build with `make -C port TARGET=3ds cia` (needs `makerom` and `bannertool` on the PATH, or `MAKEROM=` / `BANNERTOOL=`);
CI builds `dkr.cia` next to `dkr.3dsx`.

The art is original: red and yellow lettering set in the open-licence Lilita One font (SIL OFL) on a plain gradient,
and a calypso jingle synthesized from scratch, kept above ~450 Hz so the 3DS speakers carry it. No Nintendo or Rare artwork, logos or audio are used.
