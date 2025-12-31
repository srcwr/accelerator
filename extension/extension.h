/**
 * =============================================================================
 * Accelerator Extension
 * Copyright (C) 2009-2010 Asher Baker (asherkin).  All rights reserved.
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
 */

#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

#include <ISmmPlugin.h>

/**
 * @file extension.hpp
 * @brief Accelerator extension code header.
 */

/**
 * @brief Sample implementation of the SDK Extension.
 */
class Accelerator : public ISmmPlugin, public IMetamodListener
{
public:
	Accelerator() :
	  m_LevelInit(&IServerGameDLL::LevelInit, this, nullptr, &Accelerator::Hook_LevelInit_Post)
	, m_GameFrame(&IServerGameDLL::GameFrame, this, nullptr, &Accelerator::Hook_GameFrame_Post)
	{ }
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);

public:
	KHook::Return<bool> Hook_LevelInit_Post(
		IServerGameDLL*,
		const char *pMapName,
		char const *pMapEntities,
		char const *pOldLevel,
		char const *pLandmarkName,
		bool loadGame,
		bool background);
	KHook::Return<void> Hook_GameFrame_Post(IServerGameDLL*, bool simulating);
public:
	KHook::Virtual<IServerGameDLL, bool, char const *, char const *, char const *, char const *, bool, bool> m_LevelInit;
	KHook::Virtual<IServerGameDLL, void, bool> m_GameFrame;
public:
	const char *GetAuthor()
	{
		return "asherkin, kenzzer";
	}
	const char *GetName()
	{
		return "Accelerator";
	}
	const char *GetDescription()
	{
		return "SRCDS Crash Handler";
	}
	const char *GetURL()
	{
		return "https://github.com/srcwr/srcwr";
	}
	const char *GetLicense()
	{
		return "GPL-3.0-only";
	}
	const char *GetVersion()
	{
		return "2.6.1-srcwr";
	}
	const char *GetDate()
	{
		return __DATE__;
	}
	const char *GetLogTag()
	{
		return "CRASH";
	}
};

extern Accelerator g_accelerator;

PLUGIN_GLOBALVARS();

#endif // _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
