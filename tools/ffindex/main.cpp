// aegisub-ffindex: pre-build ffms2 indexes into aegisub's cache folder
// built in the aegisub tree so it links the exact same ffms2/FFmpeg as aegisub.exe
// (ffms2 rejects indexes written by any other version)

#include <ffms.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint32_t crc32(std::string const& s) {
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		init = true;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (unsigned char ch : s)
		crc = table[(crc ^ ch) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

static std::string utf8(fs::path const& p) {
	auto s = p.u8string();
	return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

// same name aegisub's GetCacheFilename produces: <crc of path>_<size>_<mtime>.ffindex
static fs::path cache_name(fs::path const& cache_dir, fs::path const& video) {
	auto size = fs::file_size(video);
	auto mtime = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(video).time_since_epoch()).count();
	return cache_dir / (std::to_string(crc32(utf8(video))) + "_" + std::to_string(size) + "_" + std::to_string(mtime) + ".ffindex");
}

static FFMS_IndexErrorHandling parse_eh(std::string const& s) {
	if (s == "clear") return FFMS_IEH_CLEAR_TRACK;
	if (s == "stop") return FFMS_IEH_STOP_TRACK;
	if (s == "abort") return FFMS_IEH_ABORT;
	return FFMS_IEH_IGNORE;
}

// is there already an index aegisub would accept?
static bool index_ok(fs::path const& idx, std::string const& video, FFMS_IndexErrorHandling eh) {
	char buf[1024];
	FFMS_ErrorInfo err{FFMS_ERROR_SUCCESS, FFMS_ERROR_SUCCESS, sizeof(buf), buf};
	FFMS_Index *index = FFMS_ReadIndex(utf8(idx).c_str(), &err);
	if (!index) return false;
	bool ok = FFMS_IndexBelongsToFile(index, video.c_str(), &err) == 0
		&& FFMS_GetErrorHandling(index) == eh;
	FFMS_DestroyIndex(index);
	return ok;
}

static int progress(int64_t cur, int64_t total, void *last) {
	// only print when the percentage actually changes
	int pct = total > 0 ? static_cast<int>(cur * 100 / total) : 0;
	int &prev = *static_cast<int *>(last);
	if (pct != prev) {
		prev = pct;
		std::printf("\r  %3d%%", pct);
		std::fflush(stdout);
	}
	return 0;
}

static int run(std::vector<fs::path> const& args) {
	fs::path cache_dir;
	std::string eh_name = "ignore";
	bool force = false, dry = false, check = false;
	std::vector<fs::path> files;

	for (size_t i = 0; i < args.size(); ++i) {
		auto a = utf8(args[i]);
		if (a == "--cache-dir" && i + 1 < args.size()) cache_dir = args[++i];
		else if (a == "--error-handling" && i + 1 < args.size()) eh_name = utf8(args[++i]);
		else if (a == "--force") force = true;
		else if (a == "--print-name") dry = true;
		else if (a == "--check") check = true;
		else files.push_back(args[i]);
	}
	if (cache_dir.empty() || files.empty()) {
		std::fprintf(stderr, "usage: aegisub-ffindex --cache-dir DIR [--error-handling ignore|clear|stop|abort] [--force] [--print-name] [--check] VIDEO...\n");
		return 2;
	}

	auto eh = parse_eh(eh_name);
	FFMS_Init(0, 0);
	FFMS_SetLogLevel(FFMS_LOG_QUIET);
	fs::create_directories(cache_dir);

	int failures = 0;
	for (auto const& f : files) {
		std::error_code ec;
		// same normalization aegisub uses so the hash lines up
		auto video = fs::absolute(f, ec).lexically_normal();
		if (ec || !fs::is_regular_file(video)) {
			std::fprintf(stderr, "skip (not a file): %s\n", utf8(f).c_str());
			++failures;
			continue;
		}
		auto video_s = utf8(video);
		auto idx = cache_name(cache_dir, video);

		if (dry) {
			std::printf("%s\n", utf8(idx).c_str());
			continue;
		}
		// just report whether aegisub would accept the existing index, don't index
		if (check) {
			const char *state = !fs::exists(idx) ? "missing" : index_ok(idx, video_s, eh) ? "valid" : "stale";
			std::printf("%s\t%s\n", state, utf8(idx).c_str());
			continue;
		}
		if (!force && fs::exists(idx) && index_ok(idx, video_s, eh)) {
			std::printf("cached: %s\n", video_s.c_str());
			continue;
		}

		std::printf("indexing: %s\n", video_s.c_str());
		char buf[1024];
		FFMS_ErrorInfo err{FFMS_ERROR_SUCCESS, FFMS_ERROR_SUCCESS, sizeof(buf), buf};
		FFMS_Indexer *indexer = FFMS_CreateIndexer(video_s.c_str(), &err);
		if (!indexer) {
			std::fprintf(stderr, "\n  failed to open: %s\n", buf);
			++failures;
			continue;
		}
		// same as aegisub with "Index All Tracks" on: every video and audio track
		FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_AUDIO, 1, 0);
		FFMS_TrackTypeIndexSettings(indexer, FFMS_TYPE_VIDEO, 1, 0);
		int last_pct = -1;
		FFMS_SetProgressCallback(indexer, progress, &last_pct);
		FFMS_Index *index = FFMS_DoIndexing2(indexer, eh, &err);
		if (!index) {
			std::fprintf(stderr, "\n  failed to index: %s\n", buf);
			++failures;
			continue;
		}
		// write to a temp name first so a killed run never leaves a half-written index
		auto tmp = idx;
		tmp += ".tmp";
		if (FFMS_WriteIndex(utf8(tmp).c_str(), index, &err)) {
			std::fprintf(stderr, "\n  failed to write index: %s\n", buf);
			++failures;
		}
		else {
			fs::rename(tmp, idx, ec);
			if (ec) {
				std::fprintf(stderr, "\n  failed to rename index: %s\n", ec.message().c_str());
				++failures;
			}
			else
				std::printf("\r  done -> %s\n", utf8(idx.filename()).c_str());
		}
		FFMS_DestroyIndex(index);
	}
	return failures ? 1 : 0;
}

#ifdef _WIN32
// wmain so non-ascii paths (japanese filenames etc) survive
int wmain(int argc, wchar_t **argv) {
	std::vector<fs::path> args(argv + 1, argv + argc);
	return run(args);
}
#else
int main(int argc, char **argv) {
	std::vector<fs::path> args(argv + 1, argv + argc);
	return run(args);
}
#endif
