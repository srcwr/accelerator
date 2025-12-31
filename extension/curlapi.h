/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
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
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#ifndef _INCLUDE_SOURCEMOD_CURLAPI_IMPL_H_
#define _INCLUDE_SOURCEMOD_CURLAPI_IMPL_H_

#include <curl/curl.h>

namespace SourceMod {


/**
 * @brief Status to return via the OnDownloadWrite function of ITransferHandler.
 */
enum DownloadWriteStatus
{
	DownloadWrite_Okay,		/**< Data transfer was successful. */
	DownloadWrite_Error,	/**< Halt the transfer and return an error. */
};

class WebTransfer;

class ITransferHandler
{
public:
	/**
	 * @brief Called when a downloader needs to write data it has received.
	 *
	 * @param downloader		Downloader object.
	 * @param userdata			User data passed to download function.
	 * @param ptr				Memory containing the received data.
	 * @param size				Size of each block in ptr.
	 * @param nmemb				Number of blocks in ptr.
	 * @return					Download status.
	 */
	virtual DownloadWriteStatus OnDownloadWrite(WebTransfer *session,
		void *userdata,
		void *ptr,
		size_t size,
		size_t nmemb)
	{
		return DownloadWrite_Error;
	}
};

class WebForm
{
public:
	WebForm();
	~WebForm();
public:
	bool AddString(const char *name, const char *data);
	bool AddFile(const char *name, const char *path);
public:
	curl_httppost *GetFormData();
private:
	curl_httppost *first;
	curl_httppost *last;
	CURLFORMcode lastError;
};

class WebTransfer
{
public:
	WebTransfer(CURL *curl);
	~WebTransfer();
	static WebTransfer *CreateWebSession();
public:
	const char *LastErrorMessage();
	int LastErrorCode();
	bool SetHeaderReturn(bool recv_hdr);
	bool Download(const char *url, ITransferHandler *handler, void *data);
	bool SetFailOnHTTPError(bool fail);
	bool PostAndDownload(const char *url,
		WebForm *form,
		ITransferHandler *handler,
		void *data);
private:
	CURL *curl;
	char errorBuffer[CURL_ERROR_SIZE];
	CURLcode lastError;
};


}


#endif /* _INCLUDE_SOURCEMOD_CURLAPI_IMPL_H_ */

