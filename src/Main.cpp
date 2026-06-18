#include "obse/PluginAPI.h"
#include "obse/CommandTable.h"
#include "obse/GameAPI.h"
#include "obse/GameObjects.h"
#include "obse/GameData.h"
#include "obse/GameForms.h"
#include "obse/GameOSDepend.h"
#include "obse/GameRTTI.h"

#include <windows.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <share.h>

static const UInt32 kHookAddress = 0x00469230;

struct BodyPartData
{
	TESForm*	modelSource;
	TESModel*	model;
	NiNode*		model3D;
	UInt32		unk0C;
};

struct ActorBodyModelData
{
	enum { kBodyPart__MAX = 16 };

	NiNode*			bip01Node;
	UInt8			pad[0x4C - 0x004];
	BodyPartData	bodyParts[kBodyPart__MAX];
	UInt32			unk14C;
	TESObjectREFR*	parentRef;
};
STATIC_ASSERT(sizeof(BodyPartData) == 0x10);
STATIC_ASSERT(offsetof(ActorBodyModelData, bodyParts) == 0x4C);
STATIC_ASSERT(offsetof(ActorBodyModelData, parentRef) == 0x150);

static void WriteJump(UInt32 addr, void* target)
{
	UInt32 rel = (UInt32)target - addr - 5;
	BYTE code[5] = { 0xE9, (BYTE)rel, (BYTE)(rel >> 8), (BYTE)(rel >> 16), (BYTE)(rel >> 24) };
	DWORD old;
	VirtualProtect((void*)addr, 5, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)addr, code, 5);
	VirtualProtect((void*)addr, 5, old, &old);
}

static void* ReadJumpTarget(UInt32 addr)
{
	BYTE* code = (BYTE*)addr;
	if (code[0] != 0xE9) return nullptr;
	UInt32 rel = *(UInt32*)(code + 1);
	return (void*)(addr + 5 + rel);
}

static void __stdcall SuffixHandler(ActorBodyModelData*, TESForm*, TESModel*, int);

__declspec(naked) void HookStub()
{
	__asm
	{
		push	ecx
		call	SuffixHandler
		mov		eax, 0x00469235
		jmp		eax
	}
}

static PluginHandle g_pluginHandle = 0;
static void* g_BlockheadHook = nullptr;

static void CallBlockhead(ActorBodyModelData* ModelData, TESForm* ModelSource, TESModel* Model, int BodyPart)
{
	if (!g_BlockheadHook) return;
	BYTE* stub = (BYTE*)g_BlockheadHook;
	if (stub[0] == 0x51 && stub[1] == 0xE8)
	{
		UInt32 rel = *(UInt32*)(stub + 2);
		void* realHandler = (void*)((UInt32)stub + 6 + rel);
		typedef void(__stdcall *BlockheadFn)(ActorBodyModelData*, TESForm*, TESModel*, int);
		((BlockheadFn)realHandler)(ModelData, ModelSource, Model, BodyPart);
	}
}

namespace EquipSuffix
{
		static std::unordered_map<std::string, int>					s_RaceGroups;
		static std::unordered_map<std::string, std::vector<UInt32>>	s_NPCGroups;
		static std::unordered_map<UInt32, std::vector<UInt32>>		s_InvisibleNPCs;
		static std::unordered_map<std::string, void*>				s_ModelCache;

	static FILE*	s_LogFile = nullptr;
	static bool		s_LogEnabled = true;
	static int		s_LogIndent = 0;

	static void Log(const char* fmt, ...)
	{
		if (!s_LogEnabled || !s_LogFile) return;

		for (int i = 0; i < s_LogIndent; i++)
			fputs("  ", s_LogFile);

		va_list args;
		va_start(args, fmt);
		vfprintf(s_LogFile, fmt, args);
		va_end(args);

		fputc('\n', s_LogFile);
		fflush(s_LogFile);
	}

	static void LogIndent(void)  { s_LogIndent++; }
	static void LogOutdent(void) { if (s_LogIndent > 0) s_LogIndent--; }

	static bool FileExists(const char* path)
	{
		if (g_FileFinder && *g_FileFinder)
			return (*g_FileFinder)->FindFile(path, 0,
				::FileFinder::FindFile_LooseFileOnly, -1)
				!= ::FileFinder::kFileStatus_NotFound;
		return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
	}

	static void ParseINI(void)
	{
		s_RaceGroups.clear();
		s_NPCGroups.clear();

		char sbuf[256] = {};
		if (GetPrivateProfileStringA("Settings", "Logging", "1", sbuf, sizeof(sbuf),
			"Data\\OBSE\\Plugins\\BlockheadEquipSuffix.ini"))
		{
			s_LogEnabled = (atoi(sbuf) != 0);
		}

		if (s_LogEnabled)
		{
			s_LogFile = _fsopen("Data\\OBSE\\Plugins\\BlockheadEquipSuffix.log", "w", _SH_DENYWR);
			if (!s_LogFile)
			{
				char alt[MAX_PATH];
				for (int id = 1; id < 100; id++)
				{
					sprintf_s(alt, "Data\\OBSE\\Plugins\\BlockheadEquipSuffix_%d.log", id);
					s_LogFile = _fsopen(alt, "w", _SH_DENYWR);
					if (s_LogFile) break;
				}
			}
		}

		Log("========================================");
		Log(" BlockheadEquipSuffix  0.5d  initialising");
		Log("========================================");

		char buffer[8192] = {};
		if (GetPrivateProfileSectionA("EquipRaceGroups", buffer, sizeof(buffer),
			"Data\\OBSE\\Plugins\\BlockheadEquipSuffix.ini"))
		{
			char* p = buffer;
			while (*p)
			{
				std::string line(p); p += line.size() + 1;
				size_t eq = line.find('=');
				if (eq == std::string::npos) continue;

				std::string groupStr = line.substr(0, eq);
				std::string races = line.substr(eq + 1);

				while (!groupStr.empty() && groupStr.back() == ' ') groupStr.pop_back();
				while (!groupStr.empty() && groupStr.front() == ' ') groupStr.erase(0, 1);
				int groupNum = atoi(groupStr.c_str());

				size_t s = 0;
				while (s < races.size())
				{
					size_t comma = races.find(',', s);
					std::string race = (comma == std::string::npos)
						? races.substr(s) : races.substr(s, comma - s);
					s = (comma == std::string::npos) ? races.size() : comma + 1;

					while (!race.empty() && race.back() == ' ') race.pop_back();
					while (!race.empty() && race.front() == ' ') race.erase(0, 1);
					if (race.empty()) continue;

					std::string lower = race;
					for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
					s_RaceGroups[lower] = groupNum;
				}
			}
		}

		memset(buffer, 0, sizeof(buffer));
		if (GetPrivateProfileSectionA("EquipNPCGroups", buffer, sizeof(buffer),
			"Data\\OBSE\\Plugins\\BlockheadEquipSuffix.ini"))
		{
			char* p = buffer;
			while (*p)
			{
				std::string line(p); p += line.size() + 1;
				size_t eq = line.find('=');
				if (eq == std::string::npos) continue;

				std::string alias = line.substr(0, eq);
				std::string ids = line.substr(eq + 1);

				while (!alias.empty() && alias.back() == ' ') alias.pop_back();
				while (!alias.empty() && alias.front() == ' ') alias.erase(0, 1);
				if (alias.empty()) continue;

				size_t s = 0;
				while (s < ids.size())
				{
					size_t comma = ids.find(',', s);
					std::string idStr = (comma == std::string::npos)
						? ids.substr(s) : ids.substr(s, comma - s);
					s = (comma == std::string::npos) ? ids.size() : comma + 1;

					while (!idStr.empty() && idStr.back() == ' ') idStr.pop_back();
					while (!idStr.empty() && idStr.front() == ' ') idStr.erase(0, 1);
					if (idStr.empty()) continue;

					UInt32 fid = 0;
					if (sscanf_s(idStr.c_str(), "%x", &fid) == 1 && fid)
						s_NPCGroups[alias].push_back(fid);
				}
			}
		}

		memset(buffer, 0, sizeof(buffer));
		if (GetPrivateProfileSectionA("EquipInvisible", buffer, sizeof(buffer),
			"Data\\OBSE\\Plugins\\BlockheadEquipSuffix.ini"))
		{
			static const struct { const char* name; UInt32 id; } kSlotNames[] = {
				{ "Head",0 },{ "Hair",1 },{ "UpperBody",2 },{ "LowerBody",3 },
				{ "Hand",4 },{ "Foot",5 },{ "RightRing",6 },{ "LeftRing",7 },
				{ "Amulet",8 },{ "Weapon",9 },{ "BackWeapon",10 },{ "SideWeapon",11 },
				{ "Quiver",12 },{ "Shield",13 },{ "Torch",14 },{ "Tail",15 },
			};
			constexpr size_t kNumSlots = sizeof(kSlotNames) / sizeof(kSlotNames[0]);

			char* p = buffer;
			while (*p)
			{
				std::string line(p); p += line.size() + 1;
				while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
				if (line.empty()) continue;

				size_t eq = line.find('=');
				std::string idPart = (eq == std::string::npos) ? line : line.substr(0, eq);
				while (!idPart.empty() && idPart.back() == ' ') idPart.pop_back();

				UInt32 fid = 0;
				if (sscanf_s(idPart.c_str(), "%x", &fid) != 1 || !fid) continue;
				fid &= 0xFFFFFF;

				if (eq == std::string::npos)
				{
					s_InvisibleNPCs[fid] = {};
				}
				else
				{
					std::string slots = line.substr(eq + 1);
					std::vector<UInt32> visible;
					size_t s = 0;
					while (s < slots.size())
					{
						size_t comma = slots.find(',', s);
						std::string slotName = (comma == std::string::npos)
							? slots.substr(s) : slots.substr(s, comma - s);
						s = (comma == std::string::npos) ? slots.size() : comma + 1;

						while (!slotName.empty() && slotName.back() == ' ') slotName.pop_back();
						while (!slotName.empty() && slotName.front() == ' ') slotName.erase(0, 1);
						if (slotName.empty()) continue;

						for (size_t i = 0; i < kNumSlots; i++)
						{
							if (!_stricmp(slotName.c_str(), kSlotNames[i].name))
							{
								visible.push_back(kSlotNames[i].id);
								break;
							}
						}
					}
					s_InvisibleNPCs[fid] = visible;
				}
			}
		}

		Log("[INI] Loaded %zu race groups, %zu NPC groups, %zu invisible NPCs, logging=%s",
			s_RaceGroups.size(), s_NPCGroups.size(), s_InvisibleNPCs.size(), s_LogEnabled ? "ON" : "OFF");
	}

	static int GetRaceGroup(const char* raceName)
	{
		if (!raceName) return -1;
		std::string lower(raceName);
		for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
		auto it = s_RaceGroups.find(lower);
		return (it != s_RaceGroups.end()) ? it->second : -1;
	}

	static const char* GetNPCSuffix(UInt32 formID)
	{
		for (auto& [alias, targets] : s_NPCGroups)
			for (auto tid : targets)
				if ((tid & 0xFFFFFF) == (formID & 0xFFFFFF))
					return alias.c_str();
		return nullptr;
	}

	static const std::vector<UInt32>* GetInvisibleVisibleSlots(UInt32 formID)
	{
		auto it = s_InvisibleNPCs.find(formID & 0xFFFFFF);
		return (it != s_InvisibleNPCs.end()) ? &it->second : nullptr;
	}

	static bool ApplyTESModel(const std::string& overrideRel, TESModel* Model,
		ActorBodyModelData* ModelData, TESForm* ModelSource, int BodyPart)
	{
		std::string key = overrideRel;
		for (auto& c : key) c = (char)std::tolower((unsigned char)c);

		TESModel* ovModel = nullptr;
		auto it = s_ModelCache.find(key);
		if (it != s_ModelCache.end())
		{
			ovModel = (TESModel*)it->second;
			Log("Reusing cached TESModel @ %08X", (UInt32)ovModel);
		}
		else
		{
			ovModel = (TESModel*)FormHeap_Allocate(sizeof(TESModel));
			memcpy(ovModel, Model, sizeof(TESModel));
			memset(&ovModel->nifPath, 0, sizeof(BSStringT));
			if (!overrideRel.empty())
			ovModel->nifPath.Set(overrideRel.c_str());
			s_ModelCache[key] = ovModel;
			if (overrideRel.empty())
				Log("Created TESModel @ %08X  path=(empty — slot hidden)", (UInt32)ovModel);
			else
				Log("Created TESModel @ %08X  path=%s", (UInt32)ovModel, overrideRel.c_str());
		}

		CallBlockhead(ModelData, ModelSource, ovModel, BodyPart);

		return true;
	}

	static void Clear(void)
	{
		s_RaceGroups.clear();
		s_NPCGroups.clear();
		s_InvisibleNPCs.clear();
		s_ModelCache.clear();

		if (s_LogFile)
		{
			Log("BlockheadEquipSuffix 0.5d shutting down.");
			fclose(s_LogFile);
			s_LogFile = nullptr;
		}
	}
}

static void __stdcall SuffixHandler(ActorBodyModelData* ModelData, TESForm* ModelSource,
									 TESModel* Model, int BodyPart)
{
	using namespace EquipSuffix;

	if (ModelSource == nullptr)
		goto chain;

	{
		TESObjectREFR* Ref = ModelData->parentRef;
		if (!Ref || !Ref->baseForm || Ref->baseForm->typeID != kFormType_NPC)
		{
			Log("[Suffix] Ref not NPC (Ref=%08X), chaining", Ref ? Ref->refID : 0);
			goto chain;
		}

		TESNPC* NPC = OBLIVION_CAST(Ref->baseForm, TESForm, TESNPC);
		TESBipedModelForm* Biped = OBLIVION_CAST(ModelSource, TESForm, TESBipedModelForm);
		if (!Biped || !NPC)
		{
			Log("[Suffix] null Biped/NPC (NPC=%08X), chaining",
				NPC ? NPC->refID : 0);
			goto chain;
		}

		Log("[Suffix] NPC=%08X  BodyPart=%d  Item=%s (%08X)",
			NPC->refID, BodyPart,
			ModelSource->GetEditorID() ? ModelSource->GetEditorID() : "?",
			ModelSource->refID);
		LogIndent();

		bool Female = NPC->actorBaseData.IsFemale();

		if (BodyPart == -1)
		{
			UInt32 partID = 0;
			while (partID < ActorBodyModelData::kBodyPart__MAX)
			{
				if (Biped->partMask & (1 << partID))
					break;
				partID++;
			}
			if (partID < ActorBodyModelData::kBodyPart__MAX)
				BodyPart = partID;
		}

		const char* srcPath = Model->nifPath.m_data;
		if (!srcPath || !srcPath[0])
		{
			Log("Source model path empty, chaining");
			LogOutdent();
			goto chain;
		}

		Log("Source path: %s", srcPath);

		std::string path(srcPath);
		size_t dot = path.rfind('.');
		if (dot == std::string::npos)
		{
			Log("No extension in source path, chaining");
			LogOutdent();
			goto chain;
		}

		std::string base = path.substr(0, dot);
		std::string ext  = path.substr(dot);
		bool applied = false;
		std::string appliedPath;

		const std::vector<UInt32>* visibleSlots = EquipSuffix::GetInvisibleVisibleSlots(NPC->refID);
		if (visibleSlots && BodyPart >= 0)
		{
			bool slotVisible = false;
			for (auto v : *visibleSlots)
				if ((UInt32)BodyPart == v) { slotVisible = true; break; }

			if (!slotVisible)
			{
				Log("NPC %08X slot %d hidden (visible slots:", NPC->refID, BodyPart);
				for (auto v : *visibleSlots)
					EquipSuffix::Log(" %d", v);
				EquipSuffix::Log(")");

				EquipSuffix::ApplyTESModel("_invisible.nif", Model, ModelData, ModelSource, BodyPart);
				LogOutdent();
				return;
			}
			else
				Log("NPC %08X slot %d visible — proceeding normally", NPC->refID, BodyPart);
		}

		const char* npcSuffix = EquipSuffix::GetNPCSuffix(NPC->refID);
		if (npcSuffix)
		{
			char buf[128];
			sprintf_s(buf, "_%s", npcSuffix);
			std::string npcPath = base + buf + ext;
			Log("NPC suffix match: alias=\"%s\"  ->  %s", npcSuffix, npcPath.c_str());

			std::string npcFullPath = "Data\\Meshes\\" + npcPath;
			if (EquipSuffix::FileExists(npcFullPath.c_str()))
			{
				Log("File check: %s  ->  FOUND", npcFullPath.c_str());
				applied = EquipSuffix::ApplyTESModel(npcPath, Model, ModelData, ModelSource, BodyPart);
				if (applied) appliedPath = npcPath;
			}
			else
				Log("File check: %s  ->  not found (falling through to race group)", npcFullPath.c_str());
		}

		if (!applied)
		{
			TESRace* Race = NPC->race.race;
			const char* raceName = nullptr;

			if (Race)
			{
				TESFullName* fn = Race->GetFullName();
				if (fn && fn->name.m_data)
					raceName = fn->name.m_data;
			}

			if (Race && raceName)
			{
				Log("Race: %s (%08X)  Female=%d", raceName, Race->refID, Female);

				int group = EquipSuffix::GetRaceGroup(raceName);
				if (group >= 0)
				{
					char buf[16];
					sprintf_s(buf, "_%d", group);
					std::string racePath = base + buf + ext;
					Log("Race group %d match  ->  %s", group, racePath.c_str());

					std::string raceFullPath = "Data\\Meshes\\" + racePath;
					bool raceFound = EquipSuffix::FileExists(raceFullPath.c_str());
					Log("File check: %s  ->  %s", raceFullPath.c_str(), raceFound ? "FOUND" : "not found");

					if (raceFound)
					{
						applied = EquipSuffix::ApplyTESModel(racePath, Model, ModelData, ModelSource, BodyPart);
						if (applied) appliedPath = racePath;
					}
				}
				else
					Log("Race \"%s\" not in any group", raceName);
			}
			else
			{
				if (Race)
					Log("Race %08X has no full name", Race->refID);
				else
					Log("Race is NULL for NPC %08X", NPC->refID);
			}
		}

		if (applied)
		{
			Log("OVERRIDE APPLIED  BodyPart=%d  ->  %s", BodyPart, appliedPath.c_str());
			LogOutdent();
			return;
		}

		LogOutdent();
	}

chain:
	Log("[Suffix] Chaining to downstream handler...");
	CallBlockhead(ModelData, ModelSource, Model, BodyPart);
}

extern "C"
{
	bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "Blockhead EquipSuffix";
		info->version = 5;

		g_pluginHandle = obse->GetPluginHandle();

		if (obse->isEditor) return false;
		if (obse->oblivionVersion != OBLIVION_VERSION_1_2_416) return false;
		if (obse->obseVersion < 21) return false;

		return true;
	}

	bool OBSEPlugin_Load(const OBSEInterface* obse)
	{
		EquipSuffix::ParseINI();

		g_BlockheadHook = ReadJumpTarget(kHookAddress);
		if (!g_BlockheadHook)
		{
			EquipSuffix::Log("[Hook] ERROR: Blockhead not detected at %08X — plugin requires Blockhead", kHookAddress);
			return false;
		}

		EquipSuffix::Log("[Hook] Blockhead hook detected @ %08X", (UInt32)g_BlockheadHook);

		WriteJump(kHookAddress, HookStub);
		EquipSuffix::Log("[Hook] EquipSuffix installed at %08X", kHookAddress);
		EquipSuffix::Log("BlockheadEquipSuffix 0.5d loaded successfully.");

		return true;
	}
}

BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved)
{
	if (dwReason == DLL_PROCESS_DETACH)
		EquipSuffix::Clear();
	return TRUE;
}
