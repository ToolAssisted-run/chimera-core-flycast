/* Zipped rom sets, answered.
 *
 * Flycast's NAOMI cart loader opens rom sets through core/archive, which
 * upstream is libzip plus the 7z sdk - a hundred translation units this build
 * dropped to be rid of them. This is the same Archive contract over
 * waterbox/zipfile: the central directory, a member by name, a member by CRC,
 * stored and deflated - which is all a MAME rom set is.
 *
 * What is refused: 7z (rezip it), encrypted or otherwise-compressed members,
 * and anything that is not a zip at all. The loader's own message is only
 * "Cannot open X", so the last refusal is kept where the driver can read it
 * and say why.
 */
#include "archive/archive.h"
#include "zipfile.h"

#include <cstring>
#include <memory>
#include <string>

namespace {

std::string g_lastError;

class ZipFile : public ArchiveFile
{
public:
	explicit ZipFile(std::unique_ptr<zipfile::Stream> stream) : m_stream(std::move(stream)) {}
	u32 Read(void *buffer, u32 length) override { return (u32)m_stream->read(buffer, length); }
	size_t length() override { return (size_t)m_stream->size(); }
	const char *getName() override { return m_stream->name().c_str(); }

private:
	std::unique_ptr<zipfile::Stream> m_stream;
};

class Zip : public Archive
{
public:
	~Zip() override { delete m_file; }

	ArchiveFile *OpenFile(const char *name) override
	{
		const zipfile::Entry *e = m_reader.find(name);
		return e ? open(*e) : nullptr;
	}
	ArchiveFile *OpenFileByCrc(u32 crc) override
	{
		const zipfile::Entry *e = m_reader.findByCrc(crc);
		return e ? open(*e) : nullptr;
	}

	bool Open(hostfs::File *file) override
	{
		m_file = file;
		const s64 size = file->size();
		if (size < 0) { g_lastError = "could not size the file"; return false; }
		auto readAt = [file](uint64_t offset, void *buffer, size_t len) {
			return file->seek((s64)offset, SEEK_SET) == 0 && file->read(buffer, 1, len) == len;
		};
		return m_reader.open(readAt, (uint64_t)size, &g_lastError);
	}

private:
	ArchiveFile *open(const zipfile::Entry &e)
	{
		std::unique_ptr<zipfile::Stream> s = m_reader.open(e, &g_lastError);
		return s ? new ZipFile(std::move(s)) : nullptr;
	}

	hostfs::File *m_file = nullptr;
	zipfile::Reader m_reader;
};

hostfs::File *openAny(const std::string &path)
{
	/* the loader names a parent set bare ("vf4"), and upstream's OpenArchive
	 * tried the extensions for it; a 7z is opened only to be refused by name */
	static const char *const suffixes[] = { "", ".zip", ".ZIP" };
	for (const char *s : suffixes)
	{
		try {
			hostfs::File *f = hostfs::storage().openFile(path + s, "rb");
			if (f != nullptr) return f;
		} catch (const hostfs::StorageException &) {
		}
	}
	return nullptr;
}

} // namespace

/* the reason the last OpenArchive answered nothing; empty when it did not */
extern "C" const char *chimera_archive_last_error(void) { return g_lastError.c_str(); }

Archive *OpenArchive(const std::string &path)
{
	g_lastError.clear();
	const size_t dot = path.find_last_of('.');
	if (dot != std::string::npos)
	{
		const std::string ext = path.substr(dot + 1);
		if (ext == "7z" || ext == "7Z")
		{
			g_lastError = "7z archives are not read here: rezip the set as an ordinary zip";
			return nullptr;
		}
	}
	hostfs::File *file = openAny(path);
	if (file == nullptr)
	{
		g_lastError = "no such file";
		return nullptr;
	}
	std::unique_ptr<Zip> zip(new Zip());
	if (!zip->Open(file))
		return nullptr;
	return zip.release();
}
