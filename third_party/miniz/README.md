Vendored miniz 3.1.2 (MIT) from richgel999/miniz commit `2948eebafc5b9f1c69bebed80333e403a09bd62d`.

Only its ZIP **reader/inflate** code is linked. Build defines `MINIZ_NO_DEFLATE_APIS`, `MINIZ_NO_ARCHIVE_WRITING_APIS`, and `MINIZ_NO_ZLIB_APIS` to remove unused compression/writing paths. `miniz_export.h` is generated in upstream CMake builds; ours is an equivalent static-library export declaration. See LICENSE.

This permits Win32 and Wine extraction without relying on `tar.exe`, external unzip programs, or a separately installed runtime.
