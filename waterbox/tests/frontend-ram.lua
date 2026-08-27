-- Frontend witness for the Flycast package: run the core inside
-- Chimera for a fixed number of frames with nothing pressed, then dump the
-- whole 68K RAM domain. The driver compares that dump byte-for-byte against
-- the native reference (run-native --dump-domain).
--
-- Job description comes from the file named by the MINIHAWK_JOB env var:
--   frames=<how many frames to advance>
--   out=<path to write the RAM dump (binary)>
--   meta=<path to write result metadata (text)>
--   shot=<optional path to write a screenshot>

-- the primary RAM domain is named per system (a Dreamcast calls it System RAM); take whichever this machine has
local DOMAIN = "System RAM"

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local meta = {}
local function finish(status, detail)
	local lines = {
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. (meta.frames or 0),
		"lag=" .. (meta.lag or 0),
		"ramsize=" .. (meta.ramsize or 0),
		"ramhash=" .. (meta.ramhash or ""),
		"flashhash=" .. (meta.flashhash or ""),
		-- the picture the machine draws: a PAL 2600 has more lines than an
		-- NTSC one, which is how a format setting shows up from out here
		"height=" .. (meta.height or 0),
	}
	if meta.metaPath then
		writeAll(meta.metaPath, table.concat(lines, "\n") .. "\n")
	end
	client.exit()
end

local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB env var not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end
meta.metaPath = job.meta

local sysid = emu.getsystemid()
if sysid ~= "DC" then
	finish("ERROR", "wrong system id: " .. tostring(sysid))
end
if emu.getcorename() ~= "Flycast" then
	finish("ERROR", "wrong core: " .. tostring(emu.getcorename()))
end

pcall(function() client.speedmode(6400) end)
pcall(function() client.invisibleemulation(true) end)

local frames = tonumber(job.frames) or 120
for _ = 1, frames do
	emu.frameadvance()
end

meta.frames = emu.framecount()
meta.lag = emu.lagcount()
meta.height = client.bufferheight()
pcall(function()
	memory.usememorydomain(DOMAIN)
	meta.ramsize = memory.getcurrentmemorydomainsize()
	meta.ramhash = memory.hash_region(0, meta.ramsize, DOMAIN)
	-- the flash is where a Dreamcast keeps what KIND of machine it is: region,
	-- language, date, console id. A setting that changes the machine without
	-- changing RAM changes this.
	local okFlash, flashSize = pcall(function() return memory.getmemorydomainsize("Flash") end)
	if okFlash and flashSize and flashSize > 0 then
		meta.flashhash = memory.hash_region(0, flashSize, "Flash")
	end
end)

if job.shot ~= nil and job.shot ~= "" then
	client.screenshot(job.shot)
end

local ram = memory.read_bytes_as_array(0, meta.ramsize, DOMAIN)
local chunks = {}
for i = 1, #ram do
	chunks[i] = string.char(ram[i])
end
writeAll(job.out, table.concat(chunks))

finish("OK", "")
