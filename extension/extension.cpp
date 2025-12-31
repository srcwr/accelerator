/*
 * =============================================================================
 * Accelerator Extension
 * Copyright (C) 2011 Asher Baker (asherkin).  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "extension.h"
#include "threadtools.h"

#ifndef PLATFORM_ARCH_FOLDER
#define PLATFORM_ARCH_FOLDER ""
#endif

#if defined _LINUX
#include "client/linux/handler/exception_handler.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"
#include "common/linux/dump_symbols.h"
#include "common/path_helper.h"

#include <tier0/platform.h>

#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <paths.h>
#include <sys/stat.h>

#include <stdlib.h>

#include "../../../simplemem/src/simplemem.hpp"

#include "curlapi.h"
using namespace SourceMod;
#include "MemoryDownloader.h"

#define URL_MINIDUMP "https://crash.limetech.org/submit"
#define URL_SYMBOLS "https://crash.limetech.org/symbols/submit"
#define URL_BINARY "https://crash.limetech.org/binary/submit"
// 0 = Disabled
// 1 = System Only
// 2 = System + Game
// 3 = System + Game + Addons
#define MINIDUMP_OPTION_SYMBOLS "3"
#define MINIDUMP_OPTION_BINARY "3"

IServerGameDLL *server = NULL;

SMM_API METAMOD_PLUGIN *CreateInterface_MMS(const MetamodVersionInfo *mvi, const MetamodLoaderInfo *mli)
{
	return &g_accelerator;
}

class StderrInhibitor
{
	FILE *saved_stderr = nullptr;

public:
	StderrInhibitor() {
		saved_stderr = fdopen(dup(fileno(stderr)), "w");
		if (freopen(_PATH_DEVNULL, "w", stderr)) {
			// If it fails, not a lot we can (or should) do.
			// Add this brace section to silence gcc warnings.
		}
	}

	~StderrInhibitor() {
		fflush(stderr);
		dup2(fileno(saved_stderr), fileno(stderr));
		fclose(saved_stderr);
	}
};

// Taken from https://hg.mozilla.org/mozilla-central/file/3eb7623b5e63b37823d5e9c562d56e586604c823/build/unix/stdc%2B%2Bcompat/stdc%2B%2Bcompat.cpp
extern "C" void __attribute__((weak)) __cxa_throw_bad_array_new_length() {
	abort();
}

namespace std {
	/* We shouldn't be throwing exceptions at all, but it sadly turns out
	   we call STL (inline) functions that do. */
	void __attribute__((weak)) __throw_out_of_range_fmt(char const* fmt, ...) {
		va_list ap;
		char buf[1024];  // That should be big enough.

		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		buf[sizeof(buf) - 1] = 0;
		va_end(ap);

		__throw_range_error(buf);
	}
} // namespace std

// Updated versions of the SM ones for C++14
void operator delete(void *ptr, size_t sz) {
	free(ptr);
}

void operator delete[](void *ptr, size_t sz) {
	free(ptr);
}

#elif defined _WINDOWS
#define _STDINT // ~.~
#include "client/windows/handler/exception_handler.h"

#else
#error Bad platform.
#endif

#include <google_breakpad/processor/minidump.h>
#include <google_breakpad/processor/minidump_processor.h>
#include <google_breakpad/processor/process_state.h>
#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/stack_frame.h>
#include <processor/pathname_stripper.h>

#include <sstream>
#include <streambuf>

Accelerator g_accelerator;
PLUGIN_EXPOSE(Accelerator, g_accelerator);

typedef void (*GetSpew_t)(char *buffer, size_t length);
GetSpew_t GetSpew;
#if defined _WINDOWS
typedef void(__fastcall *GetSpewFastcall_t)(char *buffer, size_t length);
GetSpewFastcall_t GetSpewFastcall;
#endif

char spewBuffer[65536]; // Hi.

char crashMap[256];
char crashGamePath[512];
char crashCommandLine[1024];
char crashSourceModPath[512];
char crashGameDirectory[256];
char crashSourceModVersion[32];
char steamInf[1024];

char dumpStoragePath[512];
char logPath[512];

static char minidumpSteamID64[24]{};

google_breakpad::ExceptionHandler *handler = NULL;

void terminateHandler()
{
	const char *msg = "missing exception";
	std::exception_ptr pEx = std::current_exception();
	if (pEx) {
		try {
			std::rethrow_exception(pEx);
		} catch(const std::exception &e) {
			msg = strdup(e.what());
		} catch(...) {
			msg = "unknown exception";
		}
	}

	size_t msgLength = strlen(msg) + 2;
	volatile char * volatile msgForCrashDumps = (char *)alloca(msgLength);
	strcpy((char *)msgForCrashDumps + 1, msg);

	abort();
}

void (*SignalHandler)(int, siginfo_t *, void *);

const int kExceptionSignals[] = {
	SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS
};

const int kNumHandledSignals = sizeof(kExceptionSignals) / sizeof(kExceptionSignals[0]);

static bool dumpCallback(const google_breakpad::MinidumpDescriptor& descriptor, void* context, bool succeeded)
{
	//printf("Wrote minidump to: %s\n", descriptor.path());

	if (succeeded) {
		sys_write(STDOUT_FILENO, "Wrote minidump to: ", 19);
	} else {
		sys_write(STDOUT_FILENO, "Failed to write minidump to: ", 29);
	}

	sys_write(STDOUT_FILENO, descriptor.path(), my_strlen(descriptor.path()));
	sys_write(STDOUT_FILENO, "\n", 1);

	if (!succeeded) {
		return succeeded;
	}

	my_strlcpy(dumpStoragePath, descriptor.path(), sizeof(dumpStoragePath));
	my_strlcat(dumpStoragePath, ".txt", sizeof(dumpStoragePath));

	int extra = sys_open(dumpStoragePath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (extra == -1) {
		sys_write(STDOUT_FILENO, "Failed to open metadata file!\n", 30);
		return succeeded;
	}

	sys_write(extra, "-------- CONFIG BEGIN --------", 30);
	sys_write(extra, "\nMap=", 5);
	sys_write(extra, crashMap, my_strlen(crashMap));
	sys_write(extra, "\nGamePath=", 10);
	sys_write(extra, crashGamePath, my_strlen(crashGamePath));
	sys_write(extra, "\nCommandLine=", 13);
	sys_write(extra, crashCommandLine, my_strlen(crashCommandLine));
	sys_write(extra, "\nSourceModPath=", 15);
	sys_write(extra, crashSourceModPath, my_strlen(crashSourceModPath));
	sys_write(extra, "\nGameDirectory=", 15);
	sys_write(extra, crashGameDirectory, my_strlen(crashGameDirectory));
#if 0
	if (crashSourceModVersion[0]) {
		sys_write(extra, "\nSourceModVersion=", 18);
		sys_write(extra, crashSourceModVersion, my_strlen(crashSourceModVersion));
	}
#endif
	sys_write(extra, "\nExtensionVersion=", 18);
	sys_write(extra, "none", my_strlen("none"));
	sys_write(extra, "\nExtensionBuild=", 16);
	sys_write(extra, "none", my_strlen("none"));
	sys_write(extra, steamInf, my_strlen(steamInf));
	sys_write(extra, "\n-------- CONFIG END --------\n", 30);

	if (GetSpew) {
		GetSpew(spewBuffer, sizeof(spewBuffer));

		if (my_strlen(spewBuffer) > 0) {
			sys_write(extra, "-------- CONSOLE HISTORY BEGIN --------\n", 40);
			sys_write(extra, spewBuffer, my_strlen(spewBuffer));
			sys_write(extra, "-------- CONSOLE HISTORY END --------\n", 38);
		}
	}

	sys_close(extra);

	return succeeded;
}

KHook::Return<void> Accelerator::Hook_GameFrame_Post(IServerGameDLL*, bool simulating)
{
	std::set_terminate(terminateHandler);

	bool weHaveBeenFuckedOver = false;
	struct sigaction oact;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], NULL, &oact);

		if (oact.sa_sigaction != SignalHandler) {
			weHaveBeenFuckedOver = true;
			break;
		}
	}

	if (!weHaveBeenFuckedOver) {
		return { KHook::Action::Ignore };
	}

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaddset(&act.sa_mask, kExceptionSignals[i]);
	}

	act.sa_sigaction = SignalHandler;
	act.sa_flags = SA_ONSTACK | SA_SIGINFO;

	for (int i = 0; i < kNumHandledSignals; ++i) {
		sigaction(kExceptionSignals[i], &act, NULL);
	}

	return { KHook::Action::Ignore };
}

class ClogInhibitor
{
	std::streambuf *saved_clog = nullptr;

public:
	ClogInhibitor() {
		saved_clog = std::clog.rdbuf();
		std::clog.rdbuf(nullptr);
	}

	~ClogInhibitor() {
		std::clog.rdbuf(saved_clog);
	}
};

class UploadThread
{
public:
	FILE *log = nullptr;
	char serverId[38] = "";

	void RunThread() {
		META_CONPRINT("Accelerator upload thread started.\n");

		log = fopen(logPath, "a");
		if (!log) {
			META_LOG(g_PLAPI, "Failed to open Accelerator log file: %s", logPath);
		}

		char path[512];
		snprintf(path, sizeof(path), "%s/server-id.txt", dumpStoragePath);
		FILE *serverIdFile = fopen(path, "r");
		if (serverIdFile) {
			fread(serverId, 1, sizeof(serverId) - 1, serverIdFile);
			if (!feof(serverIdFile) || strlen(serverId) != 36) {
				serverId[0] = '\0';
			}
			fclose(serverIdFile);
		}
		if (!serverId[0]) {
			serverIdFile = fopen(path, "w");
			if (serverIdFile) {
				snprintf(serverId, sizeof(serverId), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
					rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, 0x40 | ((rand() % 255) & 0x0F), rand() % 255,
					0x80 | ((rand() % 255) & 0x3F), rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255, rand() % 255);
				fputs(serverId, serverIdFile);
				fclose(serverIdFile);
			}
		}

		DIR* dumps = opendir(dumpStoragePath);

		int skip = 0;
		int count = 0;
		int failed = 0;
		char metapath[512];
		char presubmitToken[512];
		char response[512];

		struct dirent* ent = nullptr;
		while (nullptr != (ent = readdir(dumps))) {
			if (ent->d_name[0] == '.'
				&& (    ent->d_name[1] == '\0'
					|| (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
				continue;
			}

			const char *name = ent->d_name;

			int namelen = strlen(name);
			if (namelen < 4 || strcmp(&name[namelen-4], ".dmp") != 0) {
				continue;
			}

			snprintf(path, sizeof(path), "%s/%s", dumpStoragePath, name);
			snprintf(metapath, sizeof(metapath), "%s.txt", path);

			if (0 != access(metapath, F_OK)) {
				metapath[0] = '\0';
			}

			presubmitToken[0] = '\0';
			PresubmitResponse presubmitResponse = kPRUploadCrashDumpAndMetadata;

			const char *presubmitOption = nullptr; //g_pSM->GetCoreConfigValue("MinidumpPresubmit");
			bool canPresubmit = !presubmitOption || (tolower(presubmitOption[0]) == 'y' || presubmitOption[0] == '1');

			if (canPresubmit) {
				presubmitResponse = PresubmitCrashDump(path, presubmitToken, sizeof(presubmitToken));
			}

			switch (presubmitResponse) {
				case kPRLocalError:
					failed++;
					META_LOG(g_PLAPI, "Accelerator failed to locally process crash dump");
					if (log) fprintf(log, "Failed to locally process crash dump");
					break;
				case kPRRemoteError:
				case kPRUploadCrashDumpAndMetadata:
				case kPRUploadMetadataOnly:
					if (UploadCrashDump((presubmitResponse == kPRUploadMetadataOnly) ? nullptr : path, metapath, presubmitToken, response, sizeof(response))) {
						count++;
						META_LOG(g_PLAPI, "Accelerator uploaded crash dump: %s", response);
						if (log) fprintf(log, "Uploaded crash dump: %s\n", response);
					} else {
						failed++;
						META_LOG(g_PLAPI, "Accelerator failed to upload crash dump: %s", response);
						if (log) fprintf(log, "Failed to upload crash dump: %s\n", response);
					}
					break;
				case kPRDontUpload:
					skip++;
					META_LOG(g_PLAPI, "Accelerator crash dump upload skipped by server");
					if (log) fprintf(log, "Skipped due to server request\n");
					break;
			}

			if (metapath[0]) {
				unlink(metapath);
			}

			unlink(path);

			if (log) fflush(log);
		}

		closedir(dumps);

		if (log) {
			fclose(log);
			log = nullptr;
		}

		META_CONPRINTF("Accelerator upload thread finished. (%d skipped, %d uploaded, %d failed)\n", skip, count, failed);
	}

#if 0
	void OnTerminate(IThreadHandle *pHandle, bool cancel) {
		META_CONPRINTF("Accelerator upload thread terminated. (canceled = %s)\n", (cancel ? "true" : "false"));
	}
#endif

#if defined _LINUX
	bool UploadSymbolFile(const google_breakpad::CodeModule *module, const char *presubmitToken) {
		if (log) fprintf(log, "UploadSymbolFile\n");
		if (log) fflush(log);

		auto debugFile = module->debug_file();
		std::string vdsoOutputPath = "";

		if (false && debugFile == "linux-gate.so") {
			FILE *auxvFile = fopen("/proc/self/auxv", "rb");
			if (auxvFile) {
				vdsoOutputPath = "cstrike/addons/srcwr/dumps/linux-gate.so";

				while (!feof(auxvFile)) {
					int auxvEntryId = 0;
					fread(&auxvEntryId, sizeof(auxvEntryId), 1, auxvFile);
					long auxvEntryValue = 0;
					fread(&auxvEntryValue, sizeof(auxvEntryValue), 1, auxvFile);

					if (auxvEntryId == 0) break;
					if (auxvEntryId != 33) continue; // AT_SYSINFO_EHDR

#ifdef PLATFORM_X64
					Elf64_Ehdr *vdsoHdr = (Elf64_Ehdr *)auxvEntryValue;
#else
					Elf32_Ehdr *vdsoHdr = (Elf32_Ehdr *)auxvEntryValue;
#endif
					auto vdsoSize = vdsoHdr->e_shoff + (vdsoHdr->e_shentsize * vdsoHdr->e_shnum);
					void *vdsoBuffer = malloc(vdsoSize);
					memcpy(vdsoBuffer, vdsoHdr, vdsoSize);

					FILE *vdsoFile = fopen(vdsoOutputPath.c_str(), "wb");
					if (vdsoFile) {
						fwrite(vdsoBuffer, 1, vdsoSize, vdsoFile);
						fclose(vdsoFile);
						debugFile = vdsoOutputPath;
					}

					free(vdsoBuffer);
					break;
				}

				fclose(auxvFile);
			}
		}

		if (debugFile[0] != '/') {
			return false;
		}

		if (log) fprintf(log, "Submitting symbols for %s\n", debugFile.c_str());
		if (log) fflush(log);

		auto debugFileDir = google_breakpad::DirName(debugFile);
		std::vector<std::string> debug_dirs{
			debugFileDir,
			debugFileDir + "/.debug",
			"/usr/lib/debug" + debugFileDir,
		};

		std::ostringstream outputStream;
		google_breakpad::DumpOptions options(ALL_SYMBOL_DATA, true, true, false);

		{
			StderrInhibitor stdrrInhibitor;

			if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", debug_dirs, options, outputStream)) {
				outputStream.str("");
				outputStream.clear();

				// Try again without debug dirs.
				if (!WriteSymbolFile(debugFile, debugFile, "Linux", "", {}, options, outputStream)) {
					if (log) fprintf(log, "Failed to process symbol file\n");
					if (log) fflush(log);
					return false;
				}
			}
		}

		auto output = outputStream.str();
		// output = output.substr(0, output.find("\n"));
		// printf(">>> %s\n", output.c_str());

		if (debugFile == vdsoOutputPath) {
			unlink(vdsoOutputPath.c_str());
		}

		WebForm *form = new WebForm;

		if (minidumpSteamID64[0] != '\0') form->AddString("UserID", minidumpSteamID64);

		form->AddString("ExtensionVersion", g_accelerator.GetVersion());
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		form->AddString("symbol_file", output.c_str());

		MemoryDownloader data;
		WebTransfer *xfer = WebTransfer::CreateWebSession();
		xfer->SetFailOnHTTPError(true);

		bool symbolUploaded = xfer->PostAndDownload(URL_SYMBOLS, form, &data, NULL);

		if (!symbolUploaded) {
			if (log) fprintf(log, "Symbol upload failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			if (log) fflush(log);
			return false;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		if (log) fprintf(log, "Symbol upload complete: %s\n", response);
		delete[] response;
		if (log) fflush(log);
		return true;
	}
#endif

	bool UploadModuleFile(const google_breakpad::CodeModule *module, const char *presubmitToken) {
		const auto &codeFile = module->code_file();

#ifndef WIN32
		if (codeFile[0] != '/') {
#else
		if (codeFile[1] != ':') {
#endif
			return false;
		}

		if (log) fprintf(log, "Submitting binary for %s\n", codeFile.c_str());
		if (log) fflush(log);

		WebForm *form = new WebForm;

		if (minidumpSteamID64[0] != '\0') form->AddString("UserID", minidumpSteamID64);

		form->AddString("ExtensionVersion", g_accelerator.GetVersion());
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		form->AddString("debug_identifier", module->debug_identifier().c_str());
		form->AddString("code_identifier", module->code_identifier().c_str());

		form->AddFile("code_file", codeFile.c_str());

		MemoryDownloader data;
		WebTransfer *xfer = WebTransfer::CreateWebSession();
		xfer->SetFailOnHTTPError(true);

		bool binaryUploaded = xfer->PostAndDownload(URL_BINARY, form, &data, NULL);

		if (!binaryUploaded) {
			if (log) fprintf(log, "Binary upload failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			if (log) fflush(log);
			return false;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		if (log) fprintf(log, "Binary upload complete: %s\n", response);
		if (log) fflush(log);
		delete[] response;

		return true;
	}

	enum ModuleType {
		kMTUnknown,
		kMTSystem,
		kMTGame,
		kMTAddon,
		kMTExtension,
	};

	const char *ModuleTypeCode[5] = {
		"Unknown",
		"System",
		"Game",
		"Addon",
		"Extension",
	};

#ifndef WIN32
#define PATH_SEP "/"
#else
#define PATH_SEP "\\"
#endif

	bool PathPrefixMatches(const std::string &prefix, const std::string &path) {
#ifndef WIN32
		return strncmp(prefix.c_str(), path.c_str(), prefix.length()) == 0;
#else
		return _strnicmp(prefix.c_str(), path.c_str(), prefix.length()) == 0;
#endif
	}

	struct PathComparator {
		struct compare {
			bool operator() (const unsigned char &a, const unsigned char &b) const {
#ifndef WIN32
				return a < b;
#else
				return tolower(a) < tolower(b);
#endif
			}
		};

		bool operator() (const std::string &a, const std::string &b) const {
			return !std::lexicographical_compare(
				a.begin(), a.end(),
				b.begin(), b.end(),
				compare());
		};
	};

	std::map<std::string, ModuleType, PathComparator> modulePathMap;
	bool InitModuleClassificationMap(const std::string &base) {
		if (!modulePathMap.empty()) {
			modulePathMap.clear();
		}

		modulePathMap[base] = kMTGame;
		modulePathMap[std::string(crashGamePath) + PATH_SEP "addons" PATH_SEP] = kMTAddon;
		modulePathMap[std::string(crashSourceModPath) + PATH_SEP "extensions" PATH_SEP] = kMTExtension;

		return true;
	}

	ModuleType ClassifyModule(const google_breakpad::CodeModule *module) {
		if (modulePathMap.empty()) {
			return kMTUnknown;
		}

		const auto &codeFile = module->code_file();

#ifndef WIN32
		if (codeFile == "linux-gate.so") {
			return kMTSystem;
		}

		if (codeFile[0] != '/') {
#else
		if (codeFile[1] != ':') {
#endif
			return kMTUnknown;
		}

		for (decltype(modulePathMap)::const_iterator i = modulePathMap.begin(); i != modulePathMap.end(); ++i) {
			if (PathPrefixMatches(i->first, codeFile)) {
				return i->second;
			}
		}

		return kMTSystem;
	}

	std::string PathnameStripper_Directory(const std::string &path) {
		std::string::size_type slash = path.rfind('/');
		std::string::size_type backslash = path.rfind('\\');

		std::string::size_type file_start = 0;
		if (slash != std::string::npos && (backslash == std::string::npos || slash > backslash)) {
			file_start = slash + 1;
		} else if (backslash != std::string::npos) {
			file_start = backslash + 1;
		}

		return path.substr(0, file_start);
	}

	enum PresubmitResponse {
		kPRLocalError,
		kPRRemoteError,
		kPRDontUpload,
		kPRUploadCrashDumpAndMetadata,
		kPRUploadMetadataOnly,
	};

	PresubmitResponse PresubmitCrashDump(const char *path, char *tokenBuffer, size_t tokenBufferLength) {
		google_breakpad::ProcessState processState;
		google_breakpad::ProcessResult processResult;
		google_breakpad::MinidumpProcessor minidumpProcessor(nullptr, nullptr);

		{
			ClogInhibitor clogInhibitor;
			processResult = minidumpProcessor.Process(path, &processState);
		}

		if (processResult != google_breakpad::PROCESS_OK) {
			return kPRLocalError;
		}

		std::string os_short = "";
		std::string cpu_arch = "";
		if (processState.system_info()) {
			os_short = processState.system_info()->os_short;
			if (os_short.empty()) {
				os_short = processState.system_info()->os;
			}
			cpu_arch = processState.system_info()->cpu;
		}

		int requestingThread = processState.requesting_thread();
		if (requestingThread == -1) {
			requestingThread = 0;
		}

		const google_breakpad::CallStack *stack = processState.threads()->at(requestingThread);
		if (!stack) {
			return kPRLocalError;
		}

		int frameCount = stack->frames()->size();
		if (frameCount > 1024) {
			frameCount = 1024;
		}

		std::ostringstream summaryStream;
		summaryStream << 2 << "|" << processState.time_date_stamp() << "|" << os_short << "|" << cpu_arch << "|" << processState.crashed() << "|" << processState.crash_reason() << "|" << std::hex << processState.crash_address() << std::dec << "|" << requestingThread;

		std::map<const google_breakpad::CodeModule *, unsigned int> moduleMap;

		unsigned int moduleCount = processState.modules() ? processState.modules()->module_count() : 0;
		for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
			auto module = processState.modules()->GetModuleAtIndex(moduleIndex);
			moduleMap[module] = moduleIndex;

			auto debugFile = google_breakpad::PathnameStripper::File(module->debug_file());
			auto debugIdentifier = module->debug_identifier();

			summaryStream << "|M|" << debugFile << "|" << debugIdentifier;
		}

		for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
			auto frame = stack->frames()->at(frameIndex);

			int moduleIndex = -1;
			auto moduleOffset = frame->ReturnAddress();
			if (frame->module) {
				moduleIndex = moduleMap[frame->module];
				moduleOffset -= frame->module->base_address();
			}

			summaryStream << "|F|" << moduleIndex << "|" << std::hex << moduleOffset << std::dec;
		}

		auto summaryLine = summaryStream.str();
		// printf("%s\n", summaryLine.c_str());

		WebForm *form = new WebForm;

		if (minidumpSteamID64[0] != '\0') form->AddString("UserID", minidumpSteamID64);

		form->AddString("ExtensionVersion", g_accelerator.GetVersion());
		form->AddString("ServerID", serverId);

		form->AddString("CrashSignature", summaryLine.c_str());

		MemoryDownloader data;
		WebTransfer *xfer = WebTransfer::CreateWebSession();
		xfer->SetFailOnHTTPError(true);

		bool uploaded = xfer->PostAndDownload(URL_MINIDUMP, form, &data, NULL);

		if (!uploaded) {
			if (log) fprintf(log, "Presubmit failed: %s (%d)\n", xfer->LastErrorMessage(), xfer->LastErrorCode());
			return kPRRemoteError;
		}

		int responseSize = data.GetSize();
		char *response = new char[responseSize + 1];
		strncpy(response, data.GetBuffer(), responseSize + 1);
		response[responseSize] = '\0';
		while (responseSize > 0 && response[responseSize - 1] == '\n') {
			response[--responseSize] = '\0';
		}
		//if (log) fprintf(log, "Presubmit complete: %s\n", response);

		if (responseSize < 2) {
			if (log) fprintf(log, "Presubmit response too short\n");
			delete[] response;
			return kPRRemoteError;
		}

		if (response[0] == 'E') {
			if (log) fprintf(log, "Presubmit error: %s\n", &response[2]);
			delete[] response;
			return kPRRemoteError;
		}

		PresubmitResponse presubmitResponse = kPRRemoteError;
		if (response[0] == 'Y') presubmitResponse = kPRUploadCrashDumpAndMetadata;
		else if (response[0] == 'N') presubmitResponse = kPRDontUpload;
		else if (response[0] == 'M') presubmitResponse = kPRUploadMetadataOnly;
		else return kPRRemoteError;

		if (response[1] != '|') {
			if (log) fprintf(log, "Response delimiter missing\n");
			delete[] response;
			return kPRRemoteError;
		}

		unsigned int responseCount = responseSize - 2;
		if (responseCount < moduleCount) {
			if (log) fprintf(log, "Response module list doesn't match sent list (%d < %d)\n", responseCount, moduleCount);
			delete[] response;
			return presubmitResponse;
		}

		// There was a presubmit token included.
		if (tokenBuffer && responseCount > moduleCount && response[2 + moduleCount] == '|') {
			int tokenStart = 2 + moduleCount + 1;
			int tokenEnd = tokenStart;
			while (tokenEnd < responseSize && response[tokenEnd] != '|') {
				tokenEnd++;
			}

			size_t tokenLength = tokenEnd - tokenStart;
			if (tokenLength < tokenBufferLength) {
				strncpy(tokenBuffer, &response[tokenStart], tokenLength);
				tokenBuffer[tokenLength] = '\0';
			}

			if (log) fprintf(log, "Got a presubmit token from server: %s\n", tokenBuffer);
		}

		if (moduleCount > 0) {
			auto mainModule = processState.modules()->GetMainModule();
			auto executableBaseDir = PathnameStripper_Directory(mainModule->code_file());
			InitModuleClassificationMap(executableBaseDir);

			// 0 = Disabled
			// 1 = System Only
			// 2 = System + Game
			// 3 = System + Game + Addons
			const char *symbolSubmitOptionStr = MINIDUMP_OPTION_SYMBOLS; //g_pSM->GetCoreConfigValue("MinidumpSymbolUpload");
			int symbolSubmitOption = symbolSubmitOptionStr ? atoi(symbolSubmitOptionStr) : 3;

			const char *binarySubmitOption = MINIDUMP_OPTION_BINARY; //g_pSM->GetCoreConfigValue("MinidumpBinaryUpload");
			bool canBinarySubmit = !binarySubmitOption || (tolower(binarySubmitOption[0]) == 'y' || binarySubmitOption[0] == '1');

			for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
				bool submitSymbols = false;
				bool submitBinary = (response[2 + moduleIndex] == 'U');

#if defined _LINUX
				submitSymbols = (response[2 + moduleIndex] == 'Y');
#endif

				if (!submitSymbols && !submitBinary) {
					continue;
				}
				if (log) fprintf(log, "Getting module at index %d\n", moduleIndex);
				if (log) fflush(log);

				auto module = processState.modules()->GetModuleAtIndex(moduleIndex);

				auto moduleType = ClassifyModule(module);
				if (log) fprintf(log, "Classified module %s as %s\n", module->code_file().c_str(), ModuleTypeCode[moduleType]);
				if (log) fflush(log);
				switch (moduleType) {
					case kMTUnknown:
						continue;
					case kMTSystem:
						if (symbolSubmitOption < 1) {
							continue;
						}
						break;
					case kMTGame:
						if (symbolSubmitOption < 2) {
							continue;
						}
						break;
					case kMTAddon:
					case kMTExtension:
						if (symbolSubmitOption < 3) {
							continue;
						}
						break;
				}

				if (canBinarySubmit && submitBinary) {
					UploadModuleFile(module, tokenBuffer);
				}

#if defined _LINUX
				if (submitSymbols) {
					UploadSymbolFile(module, tokenBuffer);
				}
#endif
			}
		}
		if (log) fprintf(log, "PresubmitCrashDump complete\n");
		if (log) fflush(log);

		delete[] response;
		return presubmitResponse;
	}

	bool UploadCrashDump(const char *path, const char *metapath, const char *presubmitToken, char *response, int maxlen) {
		WebForm *form = new WebForm;

		if (minidumpSteamID64[0] != '\0') form->AddString("UserID", minidumpSteamID64);

		form->AddString("GameDirectory", crashGameDirectory);
		form->AddString("ExtensionVersion", g_accelerator.GetVersion());
		form->AddString("ServerID", serverId);

		if (presubmitToken && presubmitToken[0]) {
			form->AddString("PresubmitToken", presubmitToken);
		}

		if (path && path[0]) {
			form->AddFile("upload_file_minidump", path);
		}

		if (metapath && metapath[0]) {
			form->AddFile("upload_file_metadata", metapath);
		}

		MemoryDownloader data;
		WebTransfer *xfer = WebTransfer::CreateWebSession();
		xfer->SetFailOnHTTPError(true);

		bool uploaded = xfer->PostAndDownload(URL_MINIDUMP, form, &data, NULL);

		if (response) {
			if (uploaded) {
				int responseSize = data.GetSize();
				if (responseSize >= maxlen) responseSize = maxlen - 1;
				strncpy(response, data.GetBuffer(), responseSize);
				response[responseSize] = '\0';
				while (responseSize > 0 && response[responseSize - 1] == '\n') {
					response[--responseSize] = '\0';
				}
			} else {
				snprintf(response, maxlen, "%s (%d)", xfer->LastErrorMessage(), xfer->LastErrorCode());
			}
		}

		return uploaded;
	}
} uploadThread;

uintp RunUploadThread(void* param)
{
	uploadThread.RunThread();
	return 0;
}

bool Accelerator::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	if (late) {
		strncpy(error, "CANNOT LATE LOAD ACCELERATOR!!! BECAUSE IM BAD!!!", maxlen);
		return false;
	}

	if (auto s = strstr(Plat_GetCommandLine(), "-accelerator_steamid64"); s) {
		s += 23;
		if (auto x = strtoul(s, NULL, 10); x) {
			snprintf(minidumpSteamID64, sizeof(minidumpSteamID64), "%lu", x);
		}
	}

	(void)SimpleMem::SimpleMemGetOffset("dummy"); // get things loaded (& erroring) early...

	PLUGIN_SAVEVARS();

	GET_V_IFACE_ANY(GetServerFactory, server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);

	m_LevelInit.Add(server);
	m_GameFrame.Add(server);

	strncpy(dumpStoragePath, "cstrike/addons/srcwr/dumps", sizeof(dumpStoragePath));
	mkdir(dumpStoragePath, 0770);

	mkdir("cstrike/addons/srcwr/logs", 0770);
	strncpy(logPath, "cstrike/addons/srcwr/logs/accelerator.log", sizeof(logPath));

	char gamepath[MAX_PATH];
	getcwd(gamepath, sizeof(gamepath));
	// Get these early so the upload thread can use them.
	strncpy(crashGamePath, gamepath, sizeof(crashGamePath) - 1);
	strncpy(crashSourceModPath, "none", sizeof(crashSourceModPath) - 1);
	strncpy(crashGameDirectory, "cstrike", sizeof(crashGameDirectory) - 1);

	CreateSimpleThread(RunUploadThread, nullptr, nullptr, 0);

	GetSpew = (GetSpew_t)SimpleMem::SimpleMemGetSymbol("GetSpew");

	google_breakpad::MinidumpDescriptor descriptor(dumpStoragePath);
	handler = new google_breakpad::ExceptionHandler(descriptor, NULL, dumpCallback, NULL, true, -1);

	struct sigaction oact;
	sigaction(SIGSEGV, NULL, &oact);
	SignalHandler = oact.sa_sigaction;

	strncpy(crashCommandLine, Plat_GetCommandLine(), sizeof(crashCommandLine) - 1);

	FILE *steamInfFile = fopen("cstrike/steam.inf", "rb");
	if (steamInfFile) {
		char steamInfTemp[1024] = {0};
		fread(steamInfTemp, sizeof(char), sizeof(steamInfTemp) - 1, steamInfFile);

		fclose(steamInfFile);

		unsigned commentChars = 0;
		unsigned valueChars = 0;
		unsigned source = 0;
		strcpy(steamInf, "\nSteam_");
		unsigned target = 7; // strlen("\nSteam_");
		while (true) {
			if (steamInfTemp[source] == '\0') {
				source++;
				break;
			}
			if (steamInfTemp[source] == '/') {
				source++;
				commentChars++;
				continue;
			}
			if (commentChars == 1) {
				commentChars = 0;
				steamInf[target++] = '/';
				valueChars++;
			}
			if (steamInfTemp[source] == '\r') {
				source++;
				continue;
			}
			if (steamInfTemp[source] == '\n') {
				commentChars = 0;
				source++;
				if (steamInfTemp[source] == '\0') {
					break;
				}
				if (valueChars > 0) {
					valueChars = 0;
					strcpy(&steamInf[target], "\nSteam_");
					target += 7;
				}
				continue;
			}
			if (commentChars >= 2) {
				source++;
				continue;
			}
			steamInf[target++] = steamInfTemp[source++];
			valueChars++;
		}
	}

#if 0
	if (late) {
		this->OnCoreMapStart(NULL, 0, 0);
	}
#endif

	return true;
}

bool Accelerator::Unload(char *error, size_t maxlen)
{
	m_LevelInit.Remove(server);
	m_GameFrame.Remove(server);
	return true;
}

KHook::Return<bool> Accelerator::Hook_LevelInit_Post(
	IServerGameDLL*,
	const char *pMapName,
	char const *pMapEntities,
	char const *pOldLevel,
	char const *pLandmarkName,
	bool loadGame,
	bool background)
{
	strncpy(crashMap, pMapName, sizeof(crashMap) - 1);
	return { KHook::Action::Ignore };
}
