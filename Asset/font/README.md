# Local Native Fonts — Canonical Directory Structure

Every font family in `Asset/font/` must reside in its own dedicated directory:

```
Asset/font/
├── fonts.json
├── README.md
├── LICENSE.txt
└── <FamilyDir>/
    ├── <FamilyDir>-Regular.ttf
    ├── <FamilyDir>-Bold.ttf
    └── ...
```

## Schema Contract (`Asset/font/fonts.json`)

Each entry in `Asset/font/fonts.json` strictly defines:
- `"family"`: Display & CSS family name (e.g., `"Roboto Condensed"`, `"Antonio"`, `"Oswald"`).
- `"dir"`: Folder name under `Asset/font/` (e.g., `"RobotoCondensed"`, `"Antonio"`, `"Oswald"`).
- `"variants"`: Array of `{ "weight": <100..900>, "italic": <bool>, "file": "<filename>" }`.

The engine deterministically resolves every variant path as:
`Asset/font/<dir>/<file>`
