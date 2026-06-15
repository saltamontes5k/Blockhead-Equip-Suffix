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
#include <vector>
#include <cstdio>

// Hook address — TESBipedModelForm::GetBodyPartModel (Oblivion 1.2.0.416)
static const UInt32 kHookAddress = 0x00469230;

// Original engine function that hook replaced
static const UInt32 kEngineOriginal = 0x0047AA00;

// ActorBodyModelData — must match BlockheadInternals.h layout (offset-based)
struct BodyPartData
{
	TESForm*	modelSource;	// 00
	::TESModel*	model;			// 04
	NiNode*		model3D;		// 08
	UInt32		unk0C;			// 0C
};

struct ActorBodyModelData
{
	enum { kBodyPart__MAX = 16 };

	NiNode*			bip01Node;						// 000
	UInt8			pad[0x4C - 0x004];				// 004
	BodyPartData	bodyParts[kBodyPart__MAX];		// 0x4C
	UInt32			unk14C;							// 0x14C
	TESObjectREFR*	parentRef;						// 0x150
};
STATIC_ASSERT(sizeof(BodyPartData) == 0x10);
STATIC_ASSERT(offsetof(ActorBodyModelData, bodyParts) == 0x4C);
STATIC_ASSERT(offsetof(ActorBodyModelData, parentRef) == 0x150);

// ----------------------------------------------------------------
// Lightweight patch helpers (no SME dependency)

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

// ----------------------------------------------------------------
// Naked stub — replicates Blockhead's calling convention bridge.
// Forward-declared; implementation is after SuffixHandler.

static void __stdcall SuffixHandler(ActorBodyModelData*, TESForm*, TESModel*, int);
// The engine at 0x00469230 is in the middle of a thiscall member function.
// We push ECX to convert the this pointer to a stack argument, call our
// handler, and return to 0x00469235 (past the hijacked instruction).
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

// ----------------------------------------------------------------

static PluginHandle g_pluginHandle = 0;
static void* g_BlockheadHook = nullptr;

namespace EquipSuffix
{
	static std::unordered_map<std::string, int>					s_RaceGroups;
	static std::unordered_map<std::string, std::vector<UInt32>>	s_NPCGroups;
	static std::unordered_map<std::string, void*>				s_ModelCache;

	static void ParseINI()
	{
		s_RaceGroups.clear();
		s_NPCGroups.clear();

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
					std::string race = (comma == std::string::npos) ? races.substr(s) : races.substr(s, comma - s);
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
					std::string idStr = (comma == std::string::npos) ? ids.substr(s) : ids.substr(s, comma - s);
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

	static void Clear()
	{
		s_RaceGroups.clear();
		s_NPCGroups.clear();
		s_ModelCache.clear();
	}
}

// ----------------------------------------------------------------

static void __stdcall SuffixHandler(ActorBodyModelData* ModelData, TESForm* ModelSource,
	TESModel* Model, int BodyPart)
{
	if (ModelSource == nullptr)
		goto chain;

	{
		TESObjectREFR* Ref = ModelData->parentRef;
		if (!Ref || !Ref->baseForm || Ref->baseForm->typeID != kFormType_NPC)
			goto chain;

		TESNPC* NPC = OBLIVION_CAST(Ref->baseForm, TESForm, TESNPC);
		TESBipedModelForm* Biped = OBLIVION_CAST(ModelSource, TESForm, TESBipedModelForm);
		if (!Biped || !NPC)
			goto chain;

		bool Female = NPC->actorBaseData.IsFemale();
		TESModel* SrcModel = &Biped->bipedModel[Female ? 1 : 0];
		const char* srcPath = SrcModel->nifPath.m_data;
		if (!srcPath || !srcPath[0])
			goto chain;

		std::string path(srcPath);
		size_t dot = path.rfind('.');
		if (dot == std::string::npos)
			goto chain;

		std::string base = path.substr(0, dot);
		std::string ext = path.substr(dot);
		std::string overrideRel;

		const char* npcSuffix = EquipSuffix::GetNPCSuffix(NPC->refID);
		if (npcSuffix)
		{
			char buf[128];
			sprintf_s(buf, "_%s", npcSuffix);
			overrideRel = base + buf + ext;
		}
		else
		{
			TESRace* Race = NPC->race.race;
			const char* raceName = nullptr;
			if (Race)
				raceName = Race->GetEditorID();

			int group = EquipSuffix::GetRaceGroup(raceName);
			if (group >= 0)
			{
				char buf[16];
				sprintf_s(buf, "%d", group);
				overrideRel = base + buf + ext;
			}
		}

		if (!overrideRel.empty())
		{
			std::string fullPath = "Data\\Meshes\\" + overrideRel;
			if (GetFileAttributesA(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES)
			{
				// Call original engine to set up the default model first
				__asm
				{
					push	BodyPart
					push	Model
					push	ModelSource
					mov		ecx, ModelData
					mov		eax, kEngineOriginal
					call	eax
				}

				std::string key = overrideRel;
				for (auto& c : key) c = (char)std::tolower((unsigned char)c);

				TESModel* ovModel = nullptr;
				auto it = EquipSuffix::s_ModelCache.find(key);
				if (it != EquipSuffix::s_ModelCache.end())
					ovModel = (TESModel*)it->second;
				else
				{
					ovModel = (TESModel*)FormHeap_Allocate(sizeof(TESModel));
					memset(ovModel, 0, sizeof(TESModel));
					ovModel->nifPath.Set(overrideRel.c_str());
					EquipSuffix::s_ModelCache[key] = ovModel;
				}

				if (BodyPart >= 0 && BodyPart < ActorBodyModelData::kBodyPart__MAX)
					ModelData->bodyParts[BodyPart].model = ovModel;
				return;
			}
		}
	}

chain:
	if (g_BlockheadHook)
	{
		// Parse Blockhead's stub to find the real handler address.
		// Stub is: push ecx (0x51) / call <rel32> (E8 xx xx xx xx)
		// The call target = stub_base + 6 + rel_offset
		BYTE* stub = (BYTE*)g_BlockheadHook;
		if (stub[0] == 0x51 && stub[1] == 0xE8)
		{
			UInt32 rel = *(UInt32*)(stub + 2);
			void* realHandler = (void*)((UInt32)stub + 6 + rel);
			typedef void(__stdcall *BlockheadFn)(ActorBodyModelData*, TESForm*, TESModel*, int);
			((BlockheadFn)realHandler)(ModelData, ModelSource, Model, BodyPart);
		}
	}
}

// ----------------------------------------------------------------

extern "C"
{
	bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "Blockhead EquipSuffix";
		info->version = 1;

		g_pluginHandle = obse->GetPluginHandle();

		if (obse->isEditor) return false;
		if (obse->oblivionVersion != OBLIVION_VERSION_1_2_416) return false;
		if (obse->obseVersion < 21) return false;

		return true;
	}

	bool OBSEPlugin_Load(const OBSEInterface* obse)
	{
		g_BlockheadHook = ReadJumpTarget(kHookAddress);

		EquipSuffix::ParseINI();

		WriteJump(kHookAddress, HookStub);

		return true;
	}
}

BOOL WINAPI DllMain(HANDLE, DWORD, LPVOID)
{
	return TRUE;
}
