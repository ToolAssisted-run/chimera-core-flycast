/* A zip file, read the way a rom set is read: by the central directory.
 *
 * MAME rom sets are zip archives, and Flycast's cartridge loader wants three
 * things of one - the list of members with their CRCs, a member by name, and a
 * member by CRC - which it gets upstream from libzip. This is the same
 * contract on the one piece of the format those three need, over the zlib this
 * build already carries for CHDs. Stored and deflated members, zip64 sizes and
 * offsets, and nothing else: no encryption, no spanning, no writing.
 *
 * Nothing here knows about Flycast. It reads through a callback so the same
 * code runs against a hostfs::File in the guest and a plain FILE in the unit
 * test, and errors come back as text rather than as a code somebody would
 * have to look up.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zipfile {

struct Entry
{
	std::string name;        /* as stored, folders and all */
	uint64_t compressedSize; /* bytes in the file */
	uint64_t size;           /* bytes once inflated */
	uint32_t crc;            /* CRC-32 of the inflated bytes, from the directory */
	uint16_t method;         /* 0 stored, 8 deflate; anything else is refused at open */
	uint64_t localHeader;    /* offset of the member's local header */
};

/* read `len` bytes at `offset`; false when the source cannot give them all */
using ReadAt = std::function<bool(uint64_t offset, void *buffer, size_t len)>;

/* One open member, read front to back. */
class Stream
{
public:
	virtual ~Stream() = default;
	/* bytes actually produced; short only at the end or on a broken stream */
	virtual size_t read(void *buffer, size_t len) = 0;
	virtual uint64_t size() const = 0;
	virtual const std::string &name() const = 0;
};

class Reader
{
public:
	/* false with `error` set when this is not a zip this reader takes */
	bool open(ReadAt readAt, uint64_t fileSize, std::string *error);

	const std::vector<Entry> &entries() const { return m_entries; }

	/* the member spelled exactly so; failing that, one whose own name, folders
	 * dropped and case ignored, is `name` - a rom set is flat and lowercase,
	 * but what somebody rezipped need not be */
	const Entry *find(const std::string &name) const;
	/* the first member with this CRC; 0 finds nothing, as upstream */
	const Entry *findByCrc(uint32_t crc) const;

	std::unique_ptr<Stream> open(const Entry &entry, std::string *error) const;

private:
	ReadAt m_readAt;
	uint64_t m_fileSize = 0;
	std::vector<Entry> m_entries;
};

} // namespace zipfile
