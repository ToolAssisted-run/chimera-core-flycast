/* see zipfile.h */
#include "zipfile.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace zipfile {

namespace {

/* the format's little-endian fields */
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

constexpr uint32_t kEndOfDirectory = 0x06054b50;
constexpr uint32_t kZip64Locator = 0x07064b50;
constexpr uint32_t kZip64EndOfDirectory = 0x06064b50;
constexpr uint32_t kDirectoryEntry = 0x02014b50;
constexpr uint32_t kLocalHeader = 0x04034b50;

constexpr uint16_t kStored = 0;
constexpr uint16_t kDeflate = 8;

/* the end-of-directory record is at most 22 bytes plus a 65535-byte comment */
constexpr uint64_t kEndSearch = 22 + 65535;

bool fail(std::string *error, const char *what)
{
	if (error) *error = what;
	return false;
}

/* a lower-cased basename: what a lookup that ignores folders and case sees */
std::string bare(const std::string &name)
{
	size_t slash = name.find_last_of("/\\");
	std::string out = slash == std::string::npos ? name : name.substr(slash + 1);
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return out;
}

class StoredStream : public Stream
{
public:
	StoredStream(ReadAt readAt, const Entry &entry, uint64_t dataStart)
		: m_readAt(std::move(readAt)), m_entry(entry), m_pos(dataStart), m_left(entry.size) {}

	size_t read(void *buffer, size_t len) override
	{
		if (len > m_left) len = (size_t)m_left;
		if (len == 0 || !m_readAt(m_pos, buffer, len)) return 0;
		m_pos += len;
		m_left -= len;
		return len;
	}
	uint64_t size() const override { return m_entry.size; }
	const std::string &name() const override { return m_entry.name; }

private:
	ReadAt m_readAt;
	Entry m_entry;
	uint64_t m_pos;
	uint64_t m_left;
};

class DeflateStream : public Stream
{
public:
	DeflateStream(ReadAt readAt, const Entry &entry, uint64_t dataStart)
		: m_readAt(std::move(readAt)), m_entry(entry), m_pos(dataStart), m_inLeft(entry.compressedSize)
	{
		std::memset(&m_z, 0, sizeof(m_z));
		/* -MAX_WBITS: a raw deflate stream, no zlib header - the zip way */
		m_ok = inflateInit2(&m_z, -MAX_WBITS) == Z_OK;
	}
	~DeflateStream() override
	{
		if (m_ok) inflateEnd(&m_z);
	}

	size_t read(void *buffer, size_t len) override
	{
		if (!m_ok || m_done) return 0;
		m_z.next_out = (Bytef *)buffer;
		m_z.avail_out = (uInt)len;
		while (m_z.avail_out > 0)
		{
			if (m_z.avail_in == 0)
			{
				if (m_inLeft == 0) break;
				size_t chunk = (size_t)std::min<uint64_t>(m_inLeft, sizeof(m_in));
				if (!m_readAt(m_pos, m_in, chunk)) { m_ok = false; break; }
				m_pos += chunk;
				m_inLeft -= chunk;
				m_z.next_in = m_in;
				m_z.avail_in = (uInt)chunk;
			}
			int rc = inflate(&m_z, Z_NO_FLUSH);
			if (rc == Z_STREAM_END) { m_done = true; break; }
			if (rc != Z_OK) { m_ok = false; break; }
		}
		return len - m_z.avail_out;
	}
	uint64_t size() const override { return m_entry.size; }
	const std::string &name() const override { return m_entry.name; }

private:
	ReadAt m_readAt;
	Entry m_entry;
	uint64_t m_pos;
	uint64_t m_inLeft;
	z_stream m_z;
	uint8_t m_in[65536];
	bool m_ok = false;
	bool m_done = false;
};

} // namespace

bool Reader::open(ReadAt readAt, uint64_t fileSize, std::string *error)
{
	m_readAt = std::move(readAt);
	m_fileSize = fileSize;
	m_entries.clear();
	if (fileSize < 22) return fail(error, "too short to be a zip file");

	/* the end-of-central-directory record: scanned backwards for its
	 * signature, because a comment of any length may follow it */
	uint64_t tailLen = std::min<uint64_t>(fileSize, kEndSearch);
	std::vector<uint8_t> tail((size_t)tailLen);
	if (!m_readAt(fileSize - tailLen, tail.data(), (size_t)tailLen)) return fail(error, "could not read the end of the file");
	int64_t eocd = -1;
	for (int64_t i = (int64_t)tailLen - 22; i >= 0; i--)
		if (le32(&tail[(size_t)i]) == kEndOfDirectory) { eocd = i; break; }
	if (eocd < 0) return fail(error, "no end-of-central-directory record: not a zip file");
	const uint8_t *e = &tail[(size_t)eocd];
	uint64_t entryCount = le16(e + 10);
	uint64_t dirSize = le32(e + 12);
	uint64_t dirOffset = le32(e + 16);
	const uint64_t eocdOffset = fileSize - tailLen + (uint64_t)eocd;

	/* zip64: any of the three fields saturated means the real values live in
	 * the zip64 record the locator just before this one points at */
	if ((entryCount == 0xffff || dirSize == 0xffffffff || dirOffset == 0xffffffff) && eocdOffset >= 20)
	{
		uint8_t loc[20];
		if (m_readAt(eocdOffset - 20, loc, sizeof(loc)) && le32(loc) == kZip64Locator)
		{
			uint64_t z64 = le64(loc + 8);
			uint8_t rec[56];
			if (z64 + sizeof(rec) > fileSize || !m_readAt(z64, rec, sizeof(rec)) || le32(rec) != kZip64EndOfDirectory)
				return fail(error, "broken zip64 end-of-central-directory record");
			entryCount = le64(rec + 32);
			dirSize = le64(rec + 40);
			dirOffset = le64(rec + 48);
		}
	}
	if (dirOffset + dirSize > fileSize) return fail(error, "the central directory lies outside the file");

	std::vector<uint8_t> dir((size_t)dirSize);
	if (dirSize > 0 && !m_readAt(dirOffset, dir.data(), (size_t)dirSize)) return fail(error, "could not read the central directory");

	size_t p = 0;
	for (uint64_t n = 0; n < entryCount; n++)
	{
		if (p + 46 > dir.size() || le32(&dir[p]) != kDirectoryEntry) return fail(error, "a central directory entry is not where the count says");
		const uint8_t *h = &dir[p];
		Entry entry;
		const uint16_t flags = le16(h + 8);
		entry.method = le16(h + 10);
		entry.crc = le32(h + 16);
		entry.compressedSize = le32(h + 20);
		entry.size = le32(h + 24);
		const uint16_t nameLen = le16(h + 28);
		const uint16_t extraLen = le16(h + 30);
		const uint16_t commentLen = le16(h + 32);
		entry.localHeader = le32(h + 42);
		if (p + 46 + nameLen + extraLen + commentLen > dir.size()) return fail(error, "a central directory entry runs off the end");
		entry.name.assign((const char *)h + 46, nameLen);

		/* the zip64 extended-information field carries whichever of the three
		 * 32-bit fields saturated, in this order, and only those */
		const uint8_t *x = h + 46 + nameLen;
		const uint8_t *xEnd = x + extraLen;
		while (x + 4 <= xEnd)
		{
			const uint16_t id = le16(x), len = le16(x + 2);
			const uint8_t *f = x + 4;
			if (f + len > xEnd) break;
			if (id == 0x0001)
			{
				const uint8_t *q = f;
				if (entry.size == 0xffffffff && q + 8 <= f + len) { entry.size = le64(q); q += 8; }
				if (entry.compressedSize == 0xffffffff && q + 8 <= f + len) { entry.compressedSize = le64(q); q += 8; }
				if (entry.localHeader == 0xffffffff && q + 8 <= f + len) { entry.localHeader = le64(q); q += 8; }
			}
			x = f + len;
		}

		if (flags & 1) return fail(error, "an encrypted member: not a rom set this reader takes");
		if (entry.method != kStored && entry.method != kDeflate)
			return fail(error, "a member compressed with something other than deflate: rezip it as an ordinary zip");
		if (entry.localHeader >= fileSize) return fail(error, "a member's local header lies outside the file");

		m_entries.push_back(std::move(entry));
		p += 46 + nameLen + extraLen + commentLen;
	}
	return true;
}

const Entry *Reader::find(const std::string &name) const
{
	for (const Entry &e : m_entries)
		if (e.name == name) return &e;
	const std::string want = bare(name);
	for (const Entry &e : m_entries)
		if (bare(e.name) == want) return &e;
	return nullptr;
}

const Entry *Reader::findByCrc(uint32_t crc) const
{
	if (crc == 0) return nullptr;
	for (const Entry &e : m_entries)
		if (e.crc == crc && e.size > 0) return &e;
	return nullptr;
}

std::unique_ptr<Stream> Reader::open(const Entry &entry, std::string *error) const
{
	/* the local header repeats the name and extra field, and its lengths are
	 * its own - a writer may pad them differently from the directory's */
	uint8_t lh[30];
	if (entry.localHeader + sizeof(lh) > m_fileSize || !m_readAt(entry.localHeader, lh, sizeof(lh)) || le32(lh) != kLocalHeader)
	{
		fail(error, "a member's local header is not where the directory says");
		return nullptr;
	}
	const uint64_t dataStart = entry.localHeader + 30 + le16(lh + 26) + le16(lh + 28);
	if (dataStart + entry.compressedSize > m_fileSize)
	{
		fail(error, "a member's data runs off the end of the file");
		return nullptr;
	}
	if (entry.method == kStored)
		return std::unique_ptr<Stream>(new StoredStream(m_readAt, entry, dataStart));
	return std::unique_ptr<Stream>(new DeflateStream(m_readAt, entry, dataStart));
}

} // namespace zipfile
