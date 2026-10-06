# Inter 4.1

Unmodified static TrueType fonts from the official rsms/inter release, vendored
for the Avalonia desktop editor. The opt-in native game UI also reuses the regular
face. Source Sans remains the native ImGui font.

- Official release: https://github.com/rsms/inter/releases/tag/v4.1
- Release published: 2024-11-16; verified/downloaded 2026-09-26.
- Resolved release commit: `e3a3d4c57d5ecc01453a575621882a384c1995a3`.
- Official archive: https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
- Archive SHA-256: `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`.
- License: SIL Open Font License 1.1; original release text in `LICENSE.txt`.
- Copyright: The Inter Project Authors.

| Vendored file | Original archive entry | SHA-256 |
| --- | --- | --- |
| `Inter-Regular.ttf` | `extras/ttf/Inter-Regular.ttf` | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| `Inter-SemiBold.ttf` | `extras/ttf/Inter-SemiBold.ttf` | `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` |
| `LICENSE.txt` | `LICENSE.txt` | `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a` |

`fc-scan` reports typographic family `Inter`, styles `Regular` and `SemiBold`,
and `variable=False` for both files. The semibold face also exposes legacy
family `Inter SemiBold`. These are the release's static font files, not local
instances generated from its variable font.

Desktop project resource links are `Assets/Inter-Regular.ttf` and
`Assets/Inter-SemiBold.ttf`. The family URI is
`avares://Poima.Editor/Assets#Inter`; the regular file URI is
`avares://Poima.Editor/Assets/Inter-Regular.ttf#Inter`.

No desktop publish or visual qualification is implied by this vendoring step.
