# Experimental local OCR sources

The normal release uses Google Lens. Its Shell worker accepts `ocr_backend = lens`
and does not load neural weights or a game OCR plugin.

The earlier CPU-only ncnn adapter remains available for host experiments with
`-DVJO_WITH_NCNN=ON`. That build option does not enable ncnn in the current Shell
worker. The older installation and model notes are preserved on the
[Textractor experiment branch](https://github.com/bee-san/vita-jp-overlay/blob/experiment/textractor/docs/local-ocr.md).

Meiki has its own
[experiment branch and installation guide](https://github.com/bee-san/vita-jp-overlay/blob/experiment/meiki-ocr/docs/meiki-ocr.md).
Keep experimental game plugins disabled when installing the Lens release;
the FTP installer comments their taiHEN registrations. Switching back requires
the matching experimental Kernel, Shell, and game plugin package together.

The current Lens memory limits are documented in
[the local dictionary guide](local-dictionaries.md), and connection measurements
are in [Lens performance](lens-performance.md).
