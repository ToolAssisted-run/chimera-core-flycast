/* Zipped rom sets, refused rather than half-supported.
 *
 * Flycast's NAOMI cart loader opens rom sets through core/archive, which is
 * libzip plus the 7z sdk - a hundred translation units to unpack a container a
 * Chimera project has no reason to hand it. A project names its files; the
 * frontend mounts them; the core reads them. Nothing in that path is a zip.
 *
 * So OpenArchive() says "not an archive" and every caller takes the path it
 * already has for a plain file. When NAOMI arrives as its own machine
 * (docs/PLAN.md, M6), rom sets get answered properly rather than by this.
 */
#include "archive/archive.h"

Archive *OpenArchive(const std::string& path)
{
	(void)path;
	return nullptr;
}
