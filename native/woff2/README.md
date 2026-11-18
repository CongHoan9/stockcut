# WOFF2 decoder build

The Wasm font engine links the decoder-only targets from Google's WOFF2
reference implementation and Brotli's decoder/common libraries. The source
revisions are pinned in `CMakeLists.txt` so production builds do not track
upstream changes implicitly.

- WOFF2: Google, MIT license, tag `v1.0.2`
- Brotli: Google, MIT license, tag `v1.1.0`

The build downloads these sources into `native/.build/woff2`; they are not
copied into the application bundle.

The native smoke test also accepts TrueType, OpenType, WOFF, and WOFF2 fixture
paths. FreeType loads TTF/OTF/WOFF directly; WOFF2 is decompressed before the
font is handed to FreeType.

Run `make font-test FONT_TEST_FILES="/path/to/font.ttf /path/to/font.woff /path/to/font.woff2"`.
