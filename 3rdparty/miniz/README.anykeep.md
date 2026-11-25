# miniz

Source files from miniz 3.1.0, commit
`174573d60290f447c13a2b1b3405de2b96e27d6c`:
https://github.com/richgel999/miniz/tree/174573d60290f447c13a2b1b3405de2b96e27d6c

The only local changes are LF line endings in LICENSE and removal of
trailing spaces in miniz.h to satisfy the repository whitespace check.

The source files and LICENSE are included so Debian source packages and other
offline builds do not need network access. AnyKeep builds only the four C
translation units as a private static library and generates miniz_export.h
with CMake. Update all upstream files together when upgrading miniz.
