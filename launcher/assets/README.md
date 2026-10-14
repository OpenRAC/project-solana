# Project Solana launcher art (Direction D, "Horizon")

Every asset ships as SVG (`svg/`, render at build time) and as a PNG (`png/`, 32-bit RGBA, sRGB) at 3x the base size you gave. All text is converted to outlines, so the SVGs don't need any fonts installed.

| Asset | File | PNG size |
|---|---|---|
| App icon | `app_icon` | 1024x1024 |
| App icon, 16/32 px version | `app_icon_small_16-32` | 1024x1024 |
| Wordmark (mark + PROJECT + SOLANA) | `wordmark` | 780x120 |
| Wordmark, no mark | `wordmark_text_only` | 780x120 |
| Default banner | `default_banner` | 3840x720 |
| Empty library art | `empty_library` | 960x600 |
| Button icons (10) | `icons/*` | 60x60 |
| Controller glyphs, white (22) | `controller_white/*` | 66x66 |
| Controller glyphs, colour (22) | `controller_colour/*` | 66x66 |
| Window background tile | `window_background_tile` | 512x512 (tileable, opaque) |

## Notes

- **App icon.** Use `app_icon` for the 48 px and larger sizes in the .ico, and `app_icon_small_16-32` for 16, 24 and 32 px. The small one drops the stripes, which turn to mush below 32 px, and has a thicker horizon.
- **Banner.** The left 40% is plain background (#0A0E1A). The sun sits at x≈2900, and its top is at y≈250, inside the centred 400 px band. The horizon crosses at y≈470 to 520. Stars only appear right of x≈1700.
- **Wordmark.** "PROJECT" is about 7 px tall at 100% scaling. If that's too small on your sidebar, use `wordmark_text_only` or just the mark.
- **Icons.** Pure white (#FFFFFF) on transparent, with a 1.6 px stroke at 1x, so they're ready for your tinting. Play is filled.
- **Controller glyph names:** `ps_cross`, `ps_circle`, `ps_square`, `ps_triangle`, `ps_l1`, `ps_r1`, `ps_l2`, `ps_r2`, `ps_start`, `ps_select`, `xbox_a`, `xbox_b`, `xbox_x`, `xbox_y`, `xbox_lb`, `xbox_rb`, `xbox_lt`, `xbox_rt`, `xbox_menu`, `xbox_view`.

## Palette (also in `palette.json`)

| Role | Hex |
|---|---|
| Background | `#0A0E1A` |
| Sidebar | `#0F1424` |
| Card | `#151C30` |
| Card hover | `#1C2540` |
| Border | `#26304D` |
| Primary text | `#F4F6FB` |
| Secondary text | `#9AA6C6` |
| Accent | `#FF7A3D` |
| Accent hover | `#FF9461` |
| Text on accent | `#0A0E1A` (white on this orange fails contrast) |
| Secondary accent (horizon cyan, optional) | `#7FD1FF` |
| Status green | `#3DD68C` |
| Status amber | `#FFB238` |
| Status red | `#FF5A5F` |

## Font, corners, spacing

- **Font.** Keep Segoe UI for the UI itself. The display faces (Russo One for SOLANA, Rajdhani for PROJECT) are already baked into the art as outlines, and neither reads as well as Segoe at 12 to 14 px. Both are under the SIL OFL if you ever want to bundle one for headings. If you do, get the TTFs from Google Fonts.
- **Corners.** 10 px on cards and 6 px on buttons and inputs. The tighter button radius suits the technical, hard-edged look better than 14 px.
- **Spacing.** A 4 px grid: 16 px card padding, 12 px between cards, 24 px for page margins.
