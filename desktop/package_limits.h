#ifndef DUNLIN_DESKTOP_PACKAGE_LIMITS_H
#define DUNLIN_DESKTOP_PACKAGE_LIMITS_H
/* Desktop-only optional npm/extension storage. Real upstream UI bundles contain
 * over 1,100 files and 75 MiB of content. These are admission bounds, not performance
 * claims; compact/embedded runtimes do not include this storage layer. */
#define DC_PACKAGE_MAX_FILES 2048u
#define DC_PACKAGE_MAX_FILE (16u * 1024u * 1024u)
#define DC_PACKAGE_MAX_TOTAL (96u * 1024u * 1024u)
#define DC_PACKAGE_MAX_ARCHIVE (80u * 1024u * 1024u)
#endif
