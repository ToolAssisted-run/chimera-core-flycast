/* waterbox/zipfile against a zip tests/zip/make-testzip.py wrote: every member
 * found by name and by CRC, every byte back as the generator made it, the
 * folder member found bare, a bad name refused, and a file that is not a zip
 * refused with a reason. Exit status is the verdict; one line per check. */
#include "zipfile.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failed;
static void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) g_failed++;
}

/* the generator's member bytes, reproduced */
static std::vector<uint8_t> body(const char *name, size_t size)
{
	const uint32_t seed = crc32(0, (const Bytef *)name, (uInt)strlen(name));
	uint32_t x = seed;
	std::vector<uint8_t> out(size);
	for (size_t i = 0; i < size; i++)
	{
		x = (x * 1103515245u + 12345u) & 0x7fffffffu;
		out[i] = (size < 100000 || i % 64 == 0) ? (uint8_t)(x >> 16) : (uint8_t)(i / 64 + seed);
	}
	return out;
}

static std::vector<uint8_t> slurp(const zipfile::Reader &r, const zipfile::Entry &e, size_t chunk)
{
	std::string err;
	auto s = r.open(e, &err);
	std::vector<uint8_t> out;
	if (!s) { fprintf(stderr, "open %s: %s\n", e.name.c_str(), err.c_str()); return out; }
	std::vector<uint8_t> buf(chunk);
	for (;;)
	{
		size_t n = s->read(buf.data(), chunk);
		if (n == 0) break;
		out.insert(out.end(), buf.begin(), buf.begin() + n);
	}
	return out;
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: %s test.zip zip64.zip notazip\n", argv[0]); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END);
	uint64_t size = (uint64_t)ftell(f);
	auto readAt = [f](uint64_t off, void *buf, size_t len) {
		return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
	};

	zipfile::Reader r;
	std::string err;
	check(r.open(readAt, size, &err), "the test zip opens");
	if (!err.empty()) fprintf(stderr, "  %s\n", err.c_str());
	check(r.entries().size() == 5, "five members listed");

	struct { const char *name; size_t size; } members[] = {
		{ "epr-21001.ic22", 4096 }, { "mpr-21002.ic1", 200000 },
		{ "sub/dir/mpr-21003.ic2", 777 }, { "empty.bin", 0 }, { "big64.bin", 70000 },
	};
	for (const auto &m : members)
	{
		const zipfile::Entry *e = r.find(m.name);
		std::string label = std::string("by name: ") + m.name;
		check(e != nullptr && e->size == m.size, label.c_str());
		if (!e) continue;
		std::vector<uint8_t> want = body(m.name, m.size);
		/* read in awkward chunks, so a stream is proven across calls */
		std::vector<uint8_t> got = slurp(r, *e, 4093);
		label = std::string("bytes: ") + m.name;
		check(got == want, label.c_str());
		uint32_t crc = crc32(0, want.data(), (uInt)want.size());
		label = std::string("directory crc: ") + m.name;
		check(e->crc == crc, label.c_str());
		if (m.size > 0)
		{
			label = std::string("by crc: ") + m.name;
			check(r.findByCrc(crc) == e, label.c_str());
		}
	}
	/* the folder member, asked for the way the loader asks: bare, and however
	 * a rezipper cased it */
	const zipfile::Entry *bareHit = r.find("MPR-21003.IC2");
	check(bareHit != nullptr && bareHit->name == "sub/dir/mpr-21003.ic2", "a folder member is found by bare name, case ignored");
	check(r.find("mpr-99999.ic9") == nullptr, "an absent name finds nothing");
	check(r.findByCrc(0) == nullptr, "crc 0 finds nothing");
	/* a one-shot read of the whole member, the loader's own shape */
	{
		const zipfile::Entry *e = r.find("mpr-21002.ic1");
		std::vector<uint8_t> got = slurp(r, *e, 200000);
		check(got == body("mpr-21002.ic1", 200000), "a member read in one call");
	}
	fclose(f);

	/* the hand-written zip64: saturated directory fields, the truth beside them */
	{
		FILE *h = fopen(argv[2], "rb");
		if (!h) { perror(argv[2]); return 2; }
		fseek(h, 0, SEEK_END);
		uint64_t hsize = (uint64_t)ftell(h);
		auto readAtH = [h](uint64_t off, void *buf, size_t len) {
			return fseek(h, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, h) == len;
		};
		zipfile::Reader z64;
		err.clear();
		check(z64.open(readAtH, hsize, &err), "a zip64 directory opens");
		if (!err.empty()) fprintf(stderr, "  %s\n", err.c_str());
		const zipfile::Entry *e = z64.find("mpr-64001.ic3");
		check(e != nullptr && e->size == 5000 && e->compressedSize == 5000 && e->localHeader == 0, "zip64 sizes and offset read from the extended field");
		if (e) check(slurp(z64, *e, 4096) == body("mpr-64001.ic3", 5000), "bytes: mpr-64001.ic3 (zip64)");
		fclose(h);
	}

	/* not a zip */
	FILE *g = fopen(argv[3], "rb");
	if (!g) { perror(argv[2]); return 2; }
	fseek(g, 0, SEEK_END);
	uint64_t gsize = (uint64_t)ftell(g);
	auto readAtG = [g](uint64_t off, void *buf, size_t len) {
		return fseek(g, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, g) == len;
	};
	zipfile::Reader bad;
	err.clear();
	check(!bad.open(readAtG, gsize, &err) && !err.empty(), "a file that is not a zip is refused with a reason");
	printf("  (%s)\n", err.c_str());
	fclose(g);

	printf("%s: %d failed\n", g_failed ? "FAIL" : "PASS", g_failed);
	return g_failed ? 1 : 0;
}
