# Original image fixture

`solid.jpg` is an original 3×2 RGB image filled with (124, 76, 212), encoded as JPEG at quality 95 without chroma subsampling. It is distributed under this repository's Apache-2.0 license. Tests use the fixed bytes to qualify native JPEG decoding; no image library is required by the test runner.

`legacy-textured-v2.pmodel` is the original red/green emissive quad cooked by Poima 0.0.8 during `tests/texture_capture.py`. It retains the actual version-2 binary for compatibility tests rather than regenerating it with the new encoder. SHA-256: `247bd5ef2ffb749c595be93befb0c47058566766e19817f21a492bbe0e157ede`. Apache-2.0.
