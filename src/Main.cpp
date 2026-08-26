#include "obse/PluginAPI.h"
#include "obse/CommandTable.h"
#include "obse/GameAPI.h"
#include "obse/GameObjects.h"
#include "obse/GameData.h"
#include "obse/GameForms.h"
#include "obse/GameExtraData.h"
#include "obse/GameBSExtraData.h"
#include "obse/GameActorValues.h"
#include "obse/GameOSDepend.h"
#include "obse/GameRTTI.h"
#include "obse/Script.h"
#include "obse/Utilities.h"

#include <windows.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <utility>
#include <share.h>
#include <intrin.h>

// ============================================================
//  BBS - Blockhead Body/Head Suffix
//  Fork of BES-Keywords (Blockhead EquipSuffix v0.6.0)
//
//  Keyword/state driven suffix overrides for equipment AND the
//  actor's own body/head mesh + texture assets, a health-gated
//  mesh/texture line, and a break-equipment system.
// ============================================================

static constexpr UInt32 kEquipHookAddress  = 0x00469230;   // Blockhead equipment hook (TESBipedModelFormGetBodyPartModel)
static constexpr UInt32 kBodyTexHookAddr   = 0x0052D4C9;   // Blockhead body texture hook
static constexpr UInt32 kBodyModelAHook    = 0x0047ABFA;   // Blockhead body model hook A
static constexpr UInt32 kBodyModelBHook    = 0x00523934;   // Blockhead body model hook B
static constexpr UInt32 kBodyEGTHookAddr   = 0x0052D612;   // Blockhead body EGT hook
static constexpr UInt32 kBodyModelGetFn    = 0x0052BE80;   // TESRace_GetBodyModel (original getter)
static constexpr UInt32 kHeadFaceGenFn     = 0x0052CD50;   // TESRace_GetFaceGenHeadParameters

// ------------------------------------------------------------
//  Game data structures (mirrors Blockhead's layouts)
// ------------------------------------------------------------

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

// FaceGenHeadParameters (partial - only fields we touch). Offsets from Blockhead.
struct FaceGenHeadParameters
{
	enum
	{
		kFaceGenData_Head = 0,
		kFaceGenData_EarsMale,
		kFaceGenData_EarsFemale,
		kFaceGenData_Mouth,
		kFaceGenData_TeethLower,
		kFaceGenData_TeethUpper,
		kFaceGenData_Tongue,
		kFaceGenData_EyesLeft,
		kFaceGenData_EyesRight = 8,
		kFaceGenData__END = 9,
	};

	UInt8		pad00[0x74];			// 00
	TESModel**		models;				// 74  NiTArray<TESModel*>
	UInt16			modelsNum;			// 78
	UInt16			modelsCap;			// 7A
	UInt8		pad7C[0x84 - 0x7C];		// 7C
	TESTexture**	textures;			// 84  NiTArray<TESTexture*>
	UInt16			texturesNum;		// 88
	UInt16			texturesCap;		// 8A
	UInt8		pad8C[0xB8 - 0x8C];		// 8C
	TESModel*		eyeLeft;			// B8
	TESModel*		eyeRight;			// BC
	SInt32			unkC0;				// C0
};

static const char* kHeadComponentNames[] = { "Head","EarsMale","EarsFemale","Mouth","TeethLower","TeethUpper","Tongue","EyesLeft","EyesRight" };

// ------------------------------------------------------------
//  Code patching primitives
// ------------------------------------------------------------

static UInt32 GetModuleImageSize(HMODULE module)
{
	const UInt8* base = (const UInt8*)module;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
	return nt->OptionalHeader.SizeOfImage;
}

static bool IsAddressInModule(UInt32 address, HMODULE module)
{
	UInt32 base = (UInt32)module;
	UInt32 size = GetModuleImageSize(module);
	if (!base || !size) return false;
	return address >= base && address < base + size;
}

static void WriteJump(UInt32 addr, void* target)
{
	UInt32 rel = (UInt32)target - addr - 5;
	BYTE code[5] = { 0xE9, (BYTE)rel, (BYTE)(rel >> 8), (BYTE)(rel >> 16), (BYTE)(rel >> 24) };
	DWORD old;
	VirtualProtect((void*)addr, 5, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)addr, code, 5);
	VirtualProtect((void*)addr, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)addr, 5);
}

static void* ReadJumpTarget(UInt32 addr)
{
	__try
	{
		BYTE* code = (BYTE*)addr;
		if (code[0] != 0xE9) return nullptr;
		UInt32 rel = *(UInt32*)(code + 1);
		return (void*)(addr + 5 + rel);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

// Scan a stub for the first relative call (E8) and return its target.
static void* FindCallTargetInStub(const void* stub, int maxBytes)
{
	__try
	{
		const BYTE* p = (const BYTE*)stub;
		for (int i = 0; i < maxBytes - 5; i++)
		{
			if (p[i] == 0xE8)
			{
				UInt32 rel = *(const UInt32*)(p + i + 1);
				return (void*)((UInt32)stub + i + 5 + rel);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
	return nullptr;
}

// Build a trampoline: copy `copyLen` bytes from `addr` into executable memory,
// then jump back to addr+copyLen. Returns the trampoline pointer.
static void* BuildTrampoline(UInt32 addr, UInt32 copyLen)
{
	void* tramp = VirtualAlloc(nullptr, copyLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) return nullptr;
	memcpy(tramp, (const void*)addr, copyLen);
	BYTE* code = (BYTE*)tramp + copyLen;
	UInt32 rel = ((UInt32)addr + copyLen) - ((UInt32)tramp + copyLen) - 5;
	code[0] = 0xE9;
	*(UInt32*)(code + 1) = rel;
	FlushInstructionCache(GetCurrentProcess(), tramp, copyLen + 5);
	return tramp;
}

// Patch `addr` with a jump to `target`, NOP-padding up to `copyLen` bytes.
static void PatchJumpN(UInt32 addr, void* target, UInt32 copyLen)
{
	UInt32 rel = (UInt32)target - addr - 5;
	BYTE code[16] = { 0 };
	code[0] = 0xE9;
	*(UInt32*)(code + 1) = rel;
	for (UInt32 i = 5; i < copyLen; i++)
		code[i] = 0x90;
	DWORD old;
	VirtualProtect((void*)addr, copyLen, PAGE_EXECUTE_READWRITE, &old);
	memcpy((void*)addr, code, copyLen);
	VirtualProtect((void*)addr, copyLen, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)addr, copyLen);
}

// ------------------------------------------------------------
//  Forward declarations
// ------------------------------------------------------------

static void __cdecl  BBSBodyTextureHandler(TESRace* Race, UInt8 BodyPart, TESNPC* NPC, BSStringT* OutTexPath, const char* Format, const char* OrgTexPath);
static TESModel* __stdcall BBSBodyModelHandler(TESNPC* NPC, TESRace* Race, UInt32 Gender, UInt32 BodyPart);
static TESModel* __stdcall BBSBodyEGTHandler(TESNPC* NPC, UInt32 BodyPart, TESModel* Original);
static void __stdcall DoHeadFaceGen(TESRace* Race, TESNPC* NPC, FaceGenHeadParameters* Params);

__declspec(naked) void BodyTextureHook()
{
	__asm
	{
		push	[ebp - 0x20]
		push	[ebp - 0x1C]
		push	edi
		call	BBSBodyTextureHandler
		add		esp, 0xC
		mov		edx, 0x0052D4CE
		jmp		edx
	}
}

__declspec(naked) void BodyModelAHook()
{
	__asm
	{
		mov		eax, [esp + 0x14]
		push	ecx
		push	eax
		call	BBSBodyModelHandler
		mov		edx, 0x0047ABFF
		jmp		edx
	}
}

__declspec(naked) void BodyModelBHook()
{
	__asm
	{
		push	ecx
		push	edi
		call	BBSBodyModelHandler
		mov		edx, 0x00523939
		jmp		edx
	}
}

__declspec(naked) void BodyEGTHook()
{
	__asm
	{
		add		esp, 0xC
		mov		eax, [esp + 0x8]
		push	esi
		push	ebp
		push	eax
		call	BBSBodyEGTHandler
		mov		esi, eax
		cmp		esi, 0
		mov		edx, 0x0052D617
		jmp		edx
	}
}

__declspec(naked) void HeadFaceGenHook()
{
	__asm
	{
		push	ebp
		mov		ebp, esp
		pushad
		push	[ebp + 0xC]
		push	[ebp + 0x8]
		push	ecx
		call	DoHeadFaceGen
		popad
		pop		ebp
		retn	8
	}
}

static PluginHandle g_pluginHandle = 0;

// Blockhead's equipment handler (chained).
static void* g_BlockheadEquipInner = nullptr;

// Blockhead's body/head asset handlers (chained).
static void* g_BlockheadTexInner  = nullptr;
static void* g_BlockheadModelInner = nullptr;
static void* g_BlockheadEGTInner  = nullptr;

// Entry-detour trampolines.
static void* g_HeadFaceGenTrampoline = nullptr;

// ============================================================
//  EquipSuffix namespace - rule engine + overrides
// ============================================================

namespace EquipSuffix
{
		static std::unordered_map<std::string, int>					s_RaceGroups;
		static std::unordered_map<std::string, std::vector<UInt32>>	s_NPCGroups;
		static std::unordered_map<UInt32, std::vector<UInt32>>		s_InvisibleNPCs;
		static std::unordered_map<std::string, TESModel*>			s_ModelCache;

		// ---- rule engine ----

		struct RuleClause
		{
			enum Type { kType_NPC, kType_Race, kType_Faction, kType_Class,
						kType_InvCount, kType_Worldspace, kType_Cell, kType_QuestStage, kType_Weather,
						kType_GMST, kType_QuestVar };
			enum Op { kOp_EQ, kOp_NE, kOp_LT, kOp_LE, kOp_GT, kOp_GE };

			Type		type;
			UInt32		formID;
			std::string	strParam;
			Op			op;
			SInt32		num;
			UInt32		varIndex;
			bool		byName;

			RuleClause() : type(kType_NPC), formID(0), op(kOp_EQ), num(0), varIndex(0), byName(false) {}
		};

		struct EquipRule
		{
			std::string					suffix;
			std::vector<RuleClause>		clauses;
		};

		struct FormIdGroupEntry
		{
			std::string				suffix;
			std::vector<UInt32>		formIDs;
		};

		static std::vector<EquipRule>			s_Rules;
		static std::vector<FormIdGroupEntry>	s_FactionGroups;
		static std::vector<FormIdGroupEntry>	s_ClassGroups;

		// ---- head override (facegen) ----
		// MANDATORY: the facegen head detour applies per-NPC head model/texture
		// overrides. Kept active by default; the reads are bounds-hardened and
		// wrapped in SEH so a bad FaceGenParams entry degrades gracefully.

		static bool s_HeadOverride = true;

		static FILE*	s_LogFile = nullptr;
		static bool		s_LogEnabled = true;
		static int		s_LogIndent = 0;
		static int		s_LogFlushCounter = 0;
		static char		s_IndentBuf[64] = {};

		static void Log(const char* fmt, ...)
		{
			if (!s_LogEnabled || !s_LogFile) return;

			if (s_IndentBuf[0])
				fputs(s_IndentBuf, s_LogFile);

			va_list args;
			va_start(args, fmt);
			vfprintf(s_LogFile, fmt, args);
			va_end(args);

			fputc('\n', s_LogFile);

			if (++s_LogFlushCounter >= 16)
			{
				fflush(s_LogFile);
				s_LogFlushCounter = 0;
			}
		}

		static void LogIndent(void)
		{
			s_LogIndent++;
			if (s_LogIndent * 2 < (int)sizeof(s_IndentBuf) - 1)
			{
				s_IndentBuf[s_LogIndent * 2] = ' ';
				s_IndentBuf[s_LogIndent * 2 - 1] = ' ';
			}
		}

		static void LogOutdent(void)
		{
			if (s_LogIndent > 0)
			{
				s_LogIndent--;
				s_IndentBuf[s_LogIndent * 2] = 0;
			}
		}

		static void Trim(std::string& s)
		{
			while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
		}

		static void Lowercase(std::string& s)
		{
			for (auto& c : s) c = (char)std::tolower((unsigned char)c);
		}

		static bool FileExists(const char* path)
		{
			if (!path || !path[0]) return false;
			if (g_FileFinder && *g_FileFinder)
				return (*g_FileFinder)->FindFile(path, 0,
					::FileFinder::FindFile_LooseFileOnly, -1)
					!= ::FileFinder::kFileStatus_NotFound;
			return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
		}

		static void FreeCachedModels()
		{
			for (auto& [key, model] : s_ModelCache)
			{
				if (model)
					FormHeap_Free(model);
			}
			s_ModelCache.clear();
		}

		// ---- clause parsing ----

		static bool IsHexToken(const std::string& s)
		{
			if (s.empty()) return false;
			for (char c : s)
				if (!isxdigit((unsigned char)c)) return false;
			return true;
		}

		static UInt32 ParseHex(const std::string& s)
		{
			return (UInt32)strtoul(s.c_str(), nullptr, 16);
		}

		static int ParseOp(const std::string& s)
		{
			if (s == "==") return RuleClause::kOp_EQ;
			if (s == "!=") return RuleClause::kOp_NE;
			if (s == "<=") return RuleClause::kOp_LE;
			if (s == ">=") return RuleClause::kOp_GE;
			if (s == "<")  return RuleClause::kOp_LT;
			if (s == ">")  return RuleClause::kOp_GT;
			return RuleClause::kOp_EQ;
		}

		static bool ParseClause(const std::string& raw, RuleClause& out)
		{
			std::vector<std::string> toks;
			size_t s = 0;
			while (s <= raw.size())
			{
				size_t c = raw.find(':', s);
				toks.push_back((c == std::string::npos) ? raw.substr(s) : raw.substr(s, c - s));
				if (c == std::string::npos) break;
				s = c + 1;
			}
			if (toks.empty() || toks[0].empty()) return false;

			std::string type = toks[0];
			Trim(type);
			Lowercase(type);

			auto param = [&]() -> std::string {
				return (toks.size() > 1) ? toks[1] : std::string();
			};

			if (type == "npc")
			{
				std::string p = param();
				if (!IsHexToken(p)) return false;
				out.type = RuleClause::kType_NPC; out.formID = ParseHex(p); return true;
			}
			if (type == "fac")
			{
				std::string p = param();
				if (!IsHexToken(p)) return false;
				out.type = RuleClause::kType_Faction; out.formID = ParseHex(p); return true;
			}
			if (type == "class")
			{
				std::string p = param();
				if (!IsHexToken(p)) return false;
				out.type = RuleClause::kType_Class; out.formID = ParseHex(p); return true;
			}
			if (type == "ws")
			{
				std::string p = param();
				if (!IsHexToken(p)) return false;
				out.type = RuleClause::kType_Worldspace; out.formID = ParseHex(p); return true;
			}
			if (type == "cell")
			{
				std::string p = param();
				if (!IsHexToken(p)) return false;
				out.type = RuleClause::kType_Cell; out.formID = ParseHex(p); return true;
			}
			if (type == "race")
			{
				std::string p = param();
				if (IsHexToken(p))
				{
					out.type = RuleClause::kType_Race; out.formID = ParseHex(p); out.strParam.clear();
					return true;
				}
				out.type = RuleClause::kType_Race; out.formID = 0;
				out.strParam = p;
				Trim(out.strParam);
				Lowercase(out.strParam);
				return !out.strParam.empty();
			}
			if (type == "inv" || type == "qst")
			{
				if (toks.size() < 4) return false;
				if (!IsHexToken(toks[1])) return false;
				out.type = (type == "inv") ? RuleClause::kType_InvCount : RuleClause::kType_QuestStage;
				out.formID = ParseHex(toks[1]);
				out.op = (RuleClause::Op)ParseOp(toks[2]);
				out.num = (SInt32)strtol(toks[3].c_str(), nullptr, 10);
				return true;
			}
			if (type == "gst")
			{
				// gst:<editorID>:<op>:<num>
				if (toks.size() < 4) return false;
				out.type = RuleClause::kType_GMST;
				out.formID = 0;
				out.strParam = toks[1];
				Trim(out.strParam);
				out.op = (RuleClause::Op)ParseOp(toks[2]);
				out.num = (SInt32)strtol(toks[3].c_str(), nullptr, 10);
				return !out.strParam.empty();
			}
			if (type == "qvar")
			{
				// qvar:<questFormID>:<name|idx>:<op>:<num>
				if (toks.size() < 5) return false;
				if (!IsHexToken(toks[1])) return false;
				out.type = RuleClause::kType_QuestVar;
				out.formID = ParseHex(toks[1]);
				out.strParam = toks[2];
				Trim(out.strParam);
				if (IsHexToken(out.strParam))
				{
					out.varIndex = ParseHex(out.strParam);
					out.byName = false;
					out.strParam.clear();
				}
				else
				{
					out.byName = true;
					Lowercase(out.strParam);
				}
				out.op = (RuleClause::Op)ParseOp(toks[3]);
				out.num = (SInt32)strtol(toks[4].c_str(), nullptr, 10);
				return true;
			}
			if (type == "wtr")
			{
				std::string p = param();
				if (IsHexToken(p))
				{
					out.type = RuleClause::kType_Weather; out.formID = ParseHex(p); out.strParam.clear();
					return true;
				}
				out.type = RuleClause::kType_Weather; out.formID = 0;
				out.strParam = p;
				Trim(out.strParam);
				Lowercase(out.strParam);
				return !out.strParam.empty();
			}
			return false;
		}

		// ---- condition evaluators ----

		static bool CompareNum(SInt32 value, RuleClause::Op op, SInt32 num)
		{
			switch (op)
			{
			case RuleClause::kOp_EQ: return value == num;
			case RuleClause::kOp_NE: return value != num;
			case RuleClause::kOp_LT: return value < num;
			case RuleClause::kOp_LE: return value <= num;
			case RuleClause::kOp_GT: return value > num;
			case RuleClause::kOp_GE: return value >= num;
			}
			return false;
		}

		static const char* GetRaceFullName(TESRace* race)
		{
			if (!race) return nullptr;
			TESFullName* fn = race->GetFullName();
			return (fn && fn->name.m_data) ? fn->name.m_data : nullptr;
		}

		static bool RaceNameEquals(const char* raceName, const char* target)
		{
			if (!raceName || !target) return false;
			std::string a(raceName), b(target);
			Lowercase(a);
			Lowercase(b);
			return a == b;
		}

		static bool NpcInFaction(TESNPC* npc, UInt32 factionFormID)
		{
			if (!npc) return false;
			TESActorBaseData::FactionListEntry* e = &npc->actorBaseData.factionList;
			while (e)
			{
				if (e->data && e->data->faction &&
					(e->data->faction->refID & 0xFFFFFF) == (factionFormID & 0xFFFFFF))
					return true;
				e = e->next;
			}
			return false;
		}

		static SInt32 CountInventoryItem(TESObjectREFR* ref, UInt32 formID)
		{
			if (!ref) return 0;
			ExtraContainerChanges* changes = ExtraContainerChanges::GetForRef(ref);
			if (!changes || !changes->data || !changes->data->objList) return 0;

			SInt32 total = 0;
			for (tList<ExtraContainerChanges::EntryData>::Iterator it = changes->data->objList->Begin();
				!it.End(); ++it)
			{
				if (!it->type || (it->type->refID & 0xFFFFFF) != (formID & 0xFFFFFF)) continue;

				SInt32 count = it->countDelta;
				if (it->extendData)
				{
					for (tList<ExtraDataList>::Iterator xIt = it->extendData->Begin();
						!xIt.End(); ++xIt)
					{
						ExtraCount* xCount = (ExtraCount*)xIt->GetByType(kExtraData_Count);
						if (xCount) { count = xCount->count; break; }
					}
				}
				total += count;
			}
			return total;
		}

		static SInt32 GetQuestCurrentStage(UInt32 questFormID)
		{
			TESForm* form = LookupFormByID(questFormID);
			if (!form || form->typeID != kFormType_Quest) return -1;
			return ((TESQuest*)form)->stageIndex;
		}

		static SInt32 GetQuestVariableValue(UInt32 questFormID, const std::string& varName, UInt32 varIndex, bool byName)
		{
			TESForm* form = LookupFormByID(questFormID);
			if (!form || form->typeID != kFormType_Quest) return 0;
			TESQuest* quest = (TESQuest*)form;
			if (!quest->scriptable.script) return 0;

			UInt32 idx = varIndex;
			if (byName)
			{
				Script::VariableInfo* vi = quest->scriptable.script->GetVariableByName(varName.c_str());
				if (!vi) return 0;
				idx = vi->idx;
			}

			ScriptEventList* el = EventListFromForm(quest);
			if (!el) return 0;
			ScriptEventList::Var* v = el->GetVariable(idx);
			if (!v) return 0;
			return (SInt32)v->data;
		}

		static TESWeather* GetCurrentWeather()
		{
			Sky* sky = Sky::GetSingleton();
			return (sky) ? sky->firstWeather : nullptr;
		}

		static bool WeatherMatches(const RuleClause& c)
		{
			TESWeather* w = GetCurrentWeather();
			if (!w) return false;

			if (c.formID)
				return (w->refID & 0xFFFFFF) == (c.formID & 0xFFFFFF);

			int target = -1;
			if      (c.strParam == "none")     target = TESWeather::kType_None;
			else if (c.strParam == "pleasant" ||
					 c.strParam == "clear")    target = TESWeather::kType_Pleasant;
			else if (c.strParam == "cloudy")   target = TESWeather::kType_Cloudy;
			else if (c.strParam == "rain" ||
					 c.strParam == "rainy")    target = TESWeather::kType_Rainy;
			else if (c.strParam == "snow" ||
					 c.strParam == "snowy")    target = TESWeather::kType_Snow;
			if (target < 0) return false;
			return w->precipType == target;
		}

		static bool EvaluateClause(const RuleClause& c, TESObjectREFR* Ref, TESNPC* NPC)
		{
			switch (c.type)
			{
			case RuleClause::kType_NPC:
				return NPC && (NPC->refID & 0xFFFFFF) == (c.formID & 0xFFFFFF);

			case RuleClause::kType_Race:
				if (!NPC || !NPC->race.race) return false;
				if (c.formID)
					return (NPC->race.race->refID & 0xFFFFFF) == (c.formID & 0xFFFFFF);
				return RaceNameEquals(GetRaceFullName(NPC->race.race), c.strParam.c_str());

			case RuleClause::kType_Faction:
				return NpcInFaction(NPC, c.formID);

			case RuleClause::kType_Class:
				return NPC && NPC->npcClass && (NPC->npcClass->refID & 0xFFFFFF) == (c.formID & 0xFFFFFF);

			case RuleClause::kType_InvCount:
				return CompareNum(CountInventoryItem(Ref, c.formID), c.op, c.num);

			case RuleClause::kType_Worldspace:
				return Ref && Ref->parentCell && Ref->parentCell->worldSpace &&
					(Ref->parentCell->worldSpace->refID & 0xFFFFFF) == (c.formID & 0xFFFFFF);

			case RuleClause::kType_Cell:
				return Ref && Ref->parentCell && (Ref->parentCell->refID & 0xFFFFFF) == (c.formID & 0xFFFFFF);

			case RuleClause::kType_QuestStage:
				return CompareNum(GetQuestCurrentStage(c.formID), c.op, c.num);

			case RuleClause::kType_Weather:
				return WeatherMatches(c);

			case RuleClause::kType_GMST:
			{
				SettingInfo* info = nullptr;
				if (!GetGameSetting(c.strParam.c_str(), &info) || !info) return false;
				switch (info->Type())
				{
				case SettingInfo::kSetting_Float:     return CompareNum((SInt32)info->f, c.op, c.num);
				case SettingInfo::kSetting_Integer:  return CompareNum(info->i, c.op, c.num);
				case SettingInfo::kSetting_Unsigned: return CompareNum((SInt32)info->u, c.op, c.num);
				case SettingInfo::kSetting_Bool:     return CompareNum(info->b ? 1 : 0, c.op, c.num);
				default: return false;
				}
			}

			case RuleClause::kType_QuestVar:
				return CompareNum(GetQuestVariableValue(c.formID, c.strParam, c.varIndex, c.byName), c.op, c.num);
			}
			return false;
		}

		static bool EvaluateRule(const EquipRule& rule, TESObjectREFR* Ref, TESNPC* NPC)
		{
			if (rule.clauses.empty()) return false;
			for (auto& c : rule.clauses)
				if (!EvaluateClause(c, Ref, NPC)) return false;
			return true;
		}

		static const EquipRule* FindMatchingRule(TESObjectREFR* Ref, TESNPC* NPC)
		{
			for (auto& r : s_Rules)
				if (EvaluateRule(r, Ref, NPC)) return &r;
			return nullptr;
		}

		static const char* MatchLegacyFactionGroup(TESNPC* NPC)
		{
			for (auto& g : s_FactionGroups)
				for (auto id : g.formIDs)
					if (NpcInFaction(NPC, id)) return g.suffix.c_str();
			return nullptr;
		}

		static const char* MatchLegacyClassGroup(TESNPC* NPC)
		{
			if (!NPC || !NPC->npcClass) return nullptr;
			UInt32 cls = NPC->npcClass->refID & 0xFFFFFF;
			for (auto& g : s_ClassGroups)
				for (auto id : g.formIDs)
					if ((id & 0xFFFFFF) == cls) return g.suffix.c_str();
			return nullptr;
		}

		static int GetRaceGroup(const char* raceName)
		{
			if (!raceName) return -1;
			std::string lower(raceName);
			Lowercase(lower);
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
			Lowercase(key);

			TESModel* ovModel = nullptr;
			auto it = s_ModelCache.find(key);
			if (it != s_ModelCache.end())
			{
				ovModel = it->second;
				Log("Reusing cached TESModel @ %08X", (UInt32)ovModel);
			}
			else
			{
				ovModel = (TESModel*)FormHeap_Allocate(sizeof(TESModel));
				memcpy(ovModel, Model, sizeof(TESModel));
				ovModel->nifPath.m_data = nullptr;
				ovModel->nifPath.m_dataLen = 0;
				ovModel->nifPath.m_bufLen = 0;
				if (!overrideRel.empty())
					ovModel->nifPath.Set(overrideRel.c_str());
				s_ModelCache[key] = ovModel;
				Log("Created TESModel @ %08X  path=%s", (UInt32)ovModel, overrideRel.c_str());
			}

			// chain to Blockhead so its scripted/override layers still apply on top
			if (g_BlockheadEquipInner)
			{
				typedef void (__stdcall *EquipFn)(ActorBodyModelData*, TESForm*, TESModel*, int);
				((EquipFn)g_BlockheadEquipInner)(ModelData, ModelSource, ovModel, BodyPart);
			}

			return true;
		}

		// ============================================================
		//  Category suffix resolution (shared by equipment + assets)
		// ============================================================

		static std::string ResolveCategorySuffix(TESObjectREFR* Ref, TESNPC* NPC)
		{
			const char* npcSuffix = NPC ? GetNPCSuffix(NPC->refID) : nullptr;
			if (npcSuffix) return npcSuffix;

			const EquipRule* rule = FindMatchingRule(Ref, NPC);
			if (rule) return rule->suffix;

			const char* fac = MatchLegacyFactionGroup(NPC);
			if (fac) return fac;

			if (NPC && NPC->race.race)
			{
				const char* rn = GetRaceFullName(NPC->race.race);
				int g = GetRaceGroup(rn);
				if (g >= 0) return std::to_string(g);
			}

			const char* cls = MatchLegacyClassGroup(NPC);
			if (cls) return cls;

			return std::string();
		}

		// ============================================================
		//  Asset suffix resolution
		// ============================================================

		enum AssetKind { kAsset_Texture, kAsset_Model, kAsset_EGT };

		static const char* kBodyComponentNames[16] = {
			"Head","Hair","UpperBody","LowerBody","Hand","Foot",
			"RightRing","LeftRing","Amulet","Weapon","BackWeapon","SideWeapon",
			"Quiver","Shield","Torch","Tail"
		};

		static const char* GetAssetRoot(AssetKind kind)
		{
			return (kind == kAsset_Texture) ? "Textures\\" : "Meshes\\";
		}

		static const char* GetOverrideFolderRoot(bool isHead)
		{
			return isHead ? "HeadAssetOverrides" : "BodyAssetOverrides";
		}

		// Normalize a path for file-existence checks: strip a leading root prefix if present.
		static std::string StripKnownRoot(const char* path, AssetKind kind)
		{
			std::string p(path);
			std::string root = GetAssetRoot(kind);
			if (p.size() >= root.size() && !_strnicmp(p.c_str(), root.c_str(), root.size()))
				p = p.substr(root.size());
			return p;
		}

		// Resolve the override path (relative, no root prefix) for an asset, or empty.
		// origRelPath - original asset path relative to its root (e.g. Characters\Imperial\HeadHuman.dds)
		// categorySuffix - rule suffix or null
		// gateStage - health gate stage or 0
		// NPC - the NPC base form
		// componentName - body part / head component name ("UpperBody", "Head", "Tongue", ...)
		// isHead - use HeadAssetOverrides folders (true) or BodyAssetOverrides (false)
		static std::string ResolveAssetOverride(const char* origRelPath, const char* categorySuffix,
			TESNPC* NPC, const char* componentName, bool isHead, AssetKind kind)
		{
			if (!origRelPath || !origRelPath[0] || !NPC) return std::string();

			std::string rel = StripKnownRoot(origRelPath, kind);
			size_t dot = rel.rfind('.');
			if (dot == std::string::npos) return std::string();
			std::string base = rel.substr(0, dot);
			std::string ext = rel.substr(dot);
			std::string root = GetAssetRoot(kind);

			const char* cat = (categorySuffix && categorySuffix[0]) ? categorySuffix : nullptr;

			// candidate override paths (relative to root), in priority order
			std::vector<std::string> candidates;

			if (cat)
			{
				candidates.push_back(base + "_" + cat + ext);					// direct category

				// per-race override folder: {Head|Body}AssetOverrides\PerRace\{M|F}\{Race}_{S}_{Component}.{ext}
				TESRace* race = NPC->race.race;
				const char* raceName = race ? GetRaceFullName(race) : nullptr;
				if (raceName && raceName[0])
				{
					const char* genderDir = NPC->actorBaseData.IsFemale() ? "F" : "M";
					std::string racePath = "Characters\\" + std::string(GetOverrideFolderRoot(isHead)) +
						"\\PerRace\\" + genderDir + "\\" + raceName + "_" + cat + "_" + componentName + ext;
					candidates.push_back(racePath);
				}

				// per-NPC override folder: ...\PerNPC\{Plugin}\{FormID:08X}_{S}_{Component}.{ext}
				if (g_dataHandler && *g_dataHandler)
				{
					UInt8 modIndex = NPC->GetModIndex();
					ModEntry::Data* plugin = (*g_dataHandler)->modsByID[modIndex];
					if (plugin && plugin->name[0])
					{
						char npcPath[MAX_PATH] = {0};
						sprintf_s(npcPath, "Characters\\%s\\PerNPC\\%s\\%08X_%s_%s%s",
							GetOverrideFolderRoot(isHead), plugin->name, NPC->refID & 0xFFFFFF,
							cat, componentName, ext.c_str());
						candidates.push_back(npcPath);
					}
				}
			}

			for (auto& cand : candidates)
			{
				std::string full = root + cand;
				if (FileExists(full.c_str()))
					return cand;
			}
			return std::string();
		}

		// ============================================================

		// ============================================================
		//  INI parsing
		// ============================================================

		static const char* kINIPath = "Data\\OBSE\\Plugins\\BlockheadEquipSuffix.ini";

		static void ParseINI(void)
		{
			s_RaceGroups.clear();
			s_NPCGroups.clear();
			s_InvisibleNPCs.clear();
			s_Rules.clear();
			s_FactionGroups.clear();
			s_ClassGroups.clear();
			s_HeadOverride = false;
			FreeCachedModels();

			char sbuf[256] = {};
			if (GetPrivateProfileStringA("Settings", "Logging", "1", sbuf, sizeof(sbuf), kINIPath))
			{
				char* end = nullptr;
				long val = strtol(sbuf, &end, 10);
				s_LogEnabled = (end != sbuf && val != 0);
			}

			// ---- [HeadOverride] ----
			if (GetPrivateProfileStringA("HeadOverride", "Enabled", "1", sbuf, sizeof(sbuf), kINIPath))
			{
				char* end = nullptr;
				long val = strtol(sbuf, &end, 10);
				s_HeadOverride = (end != sbuf && val != 0);
			}

			if (s_LogEnabled)
			{
				s_LogFile = _fsopen("Data\\OBSE\\Plugins\\BlockheadEquipSuffix.log", "w", _SH_DENYWR);
				if (!s_LogFile)
				{
					char alt[MAX_PATH];
					sprintf_s(alt, "Data\\OBSE\\Plugins\\BlockheadEquipSuffix_%lu.log", GetCurrentProcessId());
					s_LogFile = _fsopen(alt, "w", _SH_DENYWR);
				}
			}

			Log("========================================");
			Log(" BlockheadEquipSuffix 1.0 initialising");
			Log("========================================");

			char buffer[8192] = {};

			// [EquipRules]
			memset(buffer, 0, sizeof(buffer));
			if (GetPrivateProfileSectionA("EquipRules", buffer, sizeof(buffer), kINIPath))
			{
				char* p = buffer;
				while (*p)
				{
					std::string line(p); p += line.size() + 1;
					Trim(line);
					if (line.empty() || line[0] == ';') continue;

					size_t eq = line.find('=');
					if (eq == std::string::npos) continue;

					std::string suffix = line.substr(0, eq);
					Trim(suffix);
					if (suffix.empty()) continue;

					std::string clauseText = line.substr(eq + 1);
					EquipRule rule;
					rule.suffix = suffix;

					size_t s = 0;
					bool bad = false;
					while (s < clauseText.size())
					{
						size_t comma = clauseText.find(',', s);
						std::string cl = (comma == std::string::npos)
							? clauseText.substr(s) : clauseText.substr(s, comma - s);
						s = (comma == std::string::npos) ? clauseText.size() : comma + 1;

						Trim(cl);
						if (cl.empty()) continue;

						RuleClause rc;
						if (!ParseClause(cl, rc)) { bad = true; break; }
						rule.clauses.push_back(rc);
					}

					if (!bad && !rule.clauses.empty())
					{
						s_Rules.push_back(rule);
						Log("[INI] EquipRules: '%s' (%zu clauses)", suffix.c_str(), rule.clauses.size());
					}
					else
						Log("[INI] EquipRules: skipping invalid rule '%s'", suffix.c_str());
				}
			}

			// [EquipFactionGroups]
			memset(buffer, 0, sizeof(buffer));
			if (GetPrivateProfileSectionA("EquipFactionGroups", buffer, sizeof(buffer), kINIPath))
			{
				char* p = buffer;
				while (*p)
				{
					std::string line(p); p += line.size() + 1;
					Trim(line);
					if (line.empty() || line[0] == ';') continue;

					size_t eq = line.find('=');
					if (eq == std::string::npos) continue;

					std::string suffix = line.substr(0, eq);
					std::string ids = line.substr(eq + 1);
					Trim(suffix);
					if (suffix.empty()) continue;

					FormIdGroupEntry g;
					g.suffix = suffix;

					size_t s = 0;
					while (s < ids.size())
					{
						size_t comma = ids.find(',', s);
						std::string idStr = (comma == std::string::npos)
							? ids.substr(s) : ids.substr(s, comma - s);
						s = (comma == std::string::npos) ? ids.size() : comma + 1;

						Trim(idStr);
						if (idStr.empty() || !IsHexToken(idStr)) continue;
						g.formIDs.push_back(ParseHex(idStr));
					}

					if (!g.formIDs.empty())
						s_FactionGroups.push_back(g);
				}
			}

			// [EquipClassGroups]
			memset(buffer, 0, sizeof(buffer));
			if (GetPrivateProfileSectionA("EquipClassGroups", buffer, sizeof(buffer), kINIPath))
			{
				char* p = buffer;
				while (*p)
				{
					std::string line(p); p += line.size() + 1;
					Trim(line);
					if (line.empty() || line[0] == ';') continue;

					size_t eq = line.find('=');
					if (eq == std::string::npos) continue;

					std::string suffix = line.substr(0, eq);
					std::string ids = line.substr(eq + 1);
					Trim(suffix);
					if (suffix.empty()) continue;

					FormIdGroupEntry g;
					g.suffix = suffix;

					size_t s = 0;
					while (s < ids.size())
					{
						size_t comma = ids.find(',', s);
						std::string idStr = (comma == std::string::npos)
							? ids.substr(s) : ids.substr(s, comma - s);
						s = (comma == std::string::npos) ? ids.size() : comma + 1;

						Trim(idStr);
						if (idStr.empty() || !IsHexToken(idStr)) continue;
						g.formIDs.push_back(ParseHex(idStr));
					}

					if (!g.formIDs.empty())
						s_ClassGroups.push_back(g);
				}
			}

			// [EquipRaceGroups]
			memset(buffer, 0, sizeof(buffer));
			if (GetPrivateProfileSectionA("EquipRaceGroups", buffer, sizeof(buffer), kINIPath))
			{
				char* p = buffer;
				while (*p)
				{
					std::string line(p); p += line.size() + 1;
					size_t eq = line.find('=');
					if (eq == std::string::npos) continue;

					std::string groupStr = line.substr(0, eq);
					std::string races = line.substr(eq + 1);

					Trim(groupStr);
					char* end = nullptr;
					long groupNum = strtol(groupStr.c_str(), &end, 10);
					if (end == groupStr.c_str() || groupNum < 0) continue;

					size_t s = 0;
					while (s < races.size())
					{
						size_t comma = races.find(',', s);
						std::string race = (comma == std::string::npos)
							? races.substr(s) : races.substr(s, comma - s);
						s = (comma == std::string::npos) ? races.size() : comma + 1;

						Trim(race);
						if (race.empty()) continue;

						Lowercase(race);
						s_RaceGroups[race] = (int)groupNum;
					}
				}
			}

			// [EquipNPCGroups]
			memset(buffer, 0, sizeof(buffer));
			if (GetPrivateProfileSectionA("EquipNPCGroups", buffer, sizeof(buffer), kINIPath))
			{
				char* p = buffer;
				while (*p)
				{
					std::string line(p); p += line.size() + 1;
					size_t eq = line.find('=');
					if (eq == std::string::npos) continue;

					std::string alias = line.substr(0, eq);
					std::string ids = line.substr(eq + 1);

					Trim(alias);
					if (alias.empty()) continue;

					size_t s = 0;
					while (s < ids.size())
					{
						size_t comma = ids.find(',', s);
						std::string idStr = (comma == std::string::npos)
							? ids.substr(s) : ids.substr(s, comma - s);
						s = (comma == std::string::npos) ? ids.size() : comma + 1;

						Trim(idStr);
						if (idStr.empty()) continue;

						UInt32 fid = 0;
						if (sscanf_s(idStr.c_str(), "%x", &fid) == 1 && fid)
							s_NPCGroups[alias].push_back(fid);
					}
				}
			}

			// [EquipInvisible]
			memset(buffer, 0, sizeof(buffer));
			if (GetPrivateProfileSectionA("EquipInvisible", buffer, sizeof(buffer), kINIPath))
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
					Trim(line);
					if (line.empty()) continue;

					size_t eq = line.find('=');
					std::string idPart = (eq == std::string::npos) ? line : line.substr(0, eq);
					Trim(idPart);

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

							Trim(slotName);
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

			Log("[INI] %zu rules, %zu faction-groups, %zu class-groups, "
				"%zu race-groups, %zu NPC-groups, %zu invisible, logging=%s",
				s_Rules.size(), s_FactionGroups.size(), s_ClassGroups.size(),
				s_RaceGroups.size(), s_NPCGroups.size(),
				s_InvisibleNPCs.size(), s_LogEnabled ? "ON" : "OFF");
			Log("[INI] HeadOverride=%s", s_HeadOverride ? "ON" : "OFF");
			if (s_LogFile) fflush(s_LogFile);
		}

		static void Clear(void)
		{
			s_RaceGroups.clear();
			s_NPCGroups.clear();
			s_InvisibleNPCs.clear();
			s_Rules.clear();
			s_FactionGroups.clear();
			s_ClassGroups.clear();
			FreeCachedModels();

			if (s_LogFile)
			{
				Log("BlockheadEquipSuffix 1.0 shutting down.");
				fclose(s_LogFile);
				s_LogFile = nullptr;
			}
		}
}

using namespace EquipSuffix;
		//  Cached TESModel helper (body model / EGT overrides)
		// ============================================================

		static TESModel* GetCachedModel(const std::string& overrideRel, TESModel* src)
		{
			std::string key = overrideRel;
			Lowercase(key);
			auto it = s_ModelCache.find(key);
			if (it != s_ModelCache.end())
				return it->second;

			TESModel* ov = (TESModel*)FormHeap_Allocate(sizeof(TESModel));
			memcpy(ov, src, sizeof(TESModel));
			ov->nifPath.m_data = nullptr;
			ov->nifPath.m_dataLen = 0;
			ov->nifPath.m_bufLen = 0;
			if (!overrideRel.empty())
				ov->nifPath.Set(overrideRel.c_str());
			s_ModelCache[key] = ov;
			return ov;
		}

		// Fresh clones for head facegen overrides - NOT cached (Blockhead's dtors free them).
		static TESModel* CloneModelWithPath(TESModel* src, const char* path)
		{
			TESModel* m = (TESModel*)FormHeap_Allocate(sizeof(TESModel));
			memcpy(m, src, sizeof(TESModel));
			m->nifPath.m_data = nullptr;
			m->nifPath.m_dataLen = 0;
			m->nifPath.m_bufLen = 0;
			if (path && path[0])
				m->nifPath.Set(path);
			return m;
		}

		static TESTexture* CloneTextureWithPath(TESTexture* src, const char* path)
		{
			TESTexture* t = (TESTexture*)FormHeap_Allocate(sizeof(TESTexture));
			memcpy(t, src, sizeof(TESTexture));
			t->ddsPath.m_data = nullptr;
			t->ddsPath.m_dataLen = 0;
			t->ddsPath.m_bufLen = 0;
			if (path && path[0])
				t->ddsPath.Set(path);
			return t;
		}

		// ============================================================
		//  Head facegen overrides
		// ============================================================

		static void ApplyHeadOverrides(TESRace* Race, TESNPC* NPC, FaceGenHeadParameters* Params)
		{
			std::string cat = ResolveCategorySuffix(nullptr, NPC);
			const char* catPtr = cat.empty() ? nullptr : cat.c_str();

			if (!catPtr) return;

			// bounds-harden the facegen component arrays: only touch components
			// the game actually filled, and never beyond kFaceGenData__END
			UInt32 modelCount = Params->modelsNum;
			UInt32 textureCount = Params->texturesNum;
			if (modelCount > 9) modelCount = 9;
			if (textureCount > 9) textureCount = 9;

			for (UInt32 i = 0; i < modelCount; i++)
			{
				const char* comp = kHeadComponentNames[i];

				if (Params->models[i])
				{
					TESModel* m = Params->models[i];
					const char* p = m->nifPath.m_data;
					if (p && p[0])
					{
						std::string ovr = ResolveAssetOverride(p, catPtr, NPC, comp, true, kAsset_Model);
						if (!ovr.empty())
						{
							TESModel* nm = CloneModelWithPath(m, ovr.c_str());
							if (nm)
							{
								Params->models[i] = nm;
								if (i == FaceGenHeadParameters::kFaceGenData_EyesLeft)
									Params->eyeLeft = nm;
								else if (i == FaceGenHeadParameters::kFaceGenData_EyesRight)
									Params->eyeRight = nm;
								Log("[Head] model[%u] %s -> %s", i, comp, ovr.c_str());
							}
						}
					}
				}
			}

			// eyes have no head textures
			for (UInt32 i = 0; i < textureCount; i++)
			{
				const char* comp = kHeadComponentNames[i];
				if (i == FaceGenHeadParameters::kFaceGenData_EyesLeft ||
					i == FaceGenHeadParameters::kFaceGenData_EyesRight)
					continue;

				if (Params->textures[i])
				{
					TESTexture* t = Params->textures[i];
					const char* p = t->ddsPath.m_data;
					if (p && p[0])
					{
						std::string ovr = ResolveAssetOverride(p, catPtr, NPC, comp, true, kAsset_Texture);
						if (!ovr.empty())
						{
							TESTexture* nt = CloneTextureWithPath(t, ovr.c_str());
							if (nt)
							{
								Params->textures[i] = nt;
								Log("[Head] texture[%u] %s -> %s", i, comp, ovr.c_str());
							}
						}
					}
				}
			}
		}

		// ============================================================
		//  Chain helpers (call Blockhead's inner handlers)
		// ============================================================

		typedef void (__cdecl *BodyTextureFn)(TESRace*, UInt8, TESNPC*, BSStringT*, const char*, const char*);
		typedef TESModel* (__stdcall *BodyModelFn)(TESNPC*, TESRace*, UInt32, UInt32);
		typedef TESModel* (__stdcall *BodyEGTFn)(TESNPC*, UInt32, TESModel*);

		static void ChainBodyTexture(TESRace* Race, UInt8 BodyPart, TESNPC* NPC, BSStringT* OutTexPath, const char* Format, const char* OrgTexPath)
		{
			if (g_BlockheadTexInner)
				((BodyTextureFn)g_BlockheadTexInner)(Race, BodyPart, NPC, OutTexPath, Format, OrgTexPath);
		}

		static TESModel* ChainBodyModel(TESNPC* NPC, TESRace* Race, UInt32 Gender, UInt32 BodyPart)
		{
			if (g_BlockheadModelInner)
				return ((BodyModelFn)g_BlockheadModelInner)(NPC, Race, Gender, BodyPart);
			return ThisStdCallRetn<TESModel*, TESRace, UInt32, UInt32>(kBodyModelGetFn, Race, Gender, BodyPart);
		}

		static TESModel* ChainBodyEGT(TESNPC* NPC, UInt32 BodyPart, TESModel* Original)
		{
			if (g_BlockheadEGTInner)
				return ((BodyEGTFn)g_BlockheadEGTInner)(NPC, BodyPart, Original);
			return Original;
		}

		// ============================================================
		//  Body asset handlers
		// ============================================================

		static void __cdecl BBSBodyTextureHandler(TESRace* Race, UInt8 BodyPart, TESNPC* NPC, BSStringT* OutTexPath, const char* Format, const char* OrgTexPath)
		{
			const char* src = (OrgTexPath && OrgTexPath[0]) ? OrgTexPath : (OutTexPath ? OutTexPath->m_data : nullptr);
			if (!src || !src[0] || !NPC || !OutTexPath)
			{
				ChainBodyTexture(Race, BodyPart, NPC, OutTexPath, Format, OrgTexPath);
				return;
			}

			{
				std::string cat = ResolveCategorySuffix(nullptr, NPC);
				if (cat.empty())
				{
					ChainBodyTexture(Race, BodyPart, NPC, OutTexPath, Format, OrgTexPath);
					return;
				}
				const char* comp = (BodyPart < 16) ? kBodyComponentNames[BodyPart] : "Unknown";
				std::string ovr = ResolveAssetOverride(src, cat.c_str(),
					NPC, comp, false, kAsset_Texture);
				if (!ovr.empty())
				{
					std::string full = "Textures\\" + ovr;
					OutTexPath->Set(full.c_str());
					Log("[BodyTex] part %d %s -> %s", BodyPart, comp, full.c_str());
					return;
				}
			}

			ChainBodyTexture(Race, BodyPart, NPC, OutTexPath, Format, OrgTexPath);
		}

		static TESModel* __stdcall BBSBodyModelHandler(TESNPC* NPC, TESRace* Race, UInt32 Gender, UInt32 BodyPart)
		{
			if (!NPC || !Race)
				return ChainBodyModel(NPC, Race, Gender, BodyPart);

			{
				TESModel* original = ThisStdCallRetn<TESModel*, TESRace, UInt32, UInt32>(kBodyModelGetFn, Race, Gender, BodyPart);
				if (original && original->nifPath.m_data && original->nifPath.m_data[0])
				{
					std::string cat = ResolveCategorySuffix(nullptr, NPC);
					if (!cat.empty())
					{
						const char* comp = (BodyPart < 16) ? kBodyComponentNames[BodyPart] : "Unknown";
						std::string ovr = ResolveAssetOverride(original->nifPath.m_data,
							cat.c_str(), NPC, comp, false, kAsset_Model);
						if (!ovr.empty())
						{
							TESModel* m = GetCachedModel(ovr, original);
							if (m)
							{
								Log("[BodyMesh] part %d %s -> %s", BodyPart, comp, ovr.c_str());
								return m;
							}
						}
					}
				}
			}

			return ChainBodyModel(NPC, Race, Gender, BodyPart);
		}

		static TESModel* __stdcall BBSBodyEGTHandler(TESNPC* NPC, UInt32 BodyPart, TESModel* Original)
		{
			if (!NPC || !Original || !Original->nifPath.m_data || !Original->nifPath.m_data[0])
				return ChainBodyEGT(NPC, BodyPart, Original);

			{
				std::string cat = ResolveCategorySuffix(nullptr, NPC);
				if (!cat.empty())
				{
					const char* comp = (BodyPart < 16) ? kBodyComponentNames[BodyPart] : "Unknown";
					std::string ovr = ResolveAssetOverride(Original->nifPath.m_data,
						cat.c_str(), NPC, comp, false, kAsset_EGT);
					if (!ovr.empty())
					{
						TESModel* m = GetCachedModel(ovr, Original);
						if (m)
						{
							Log("[BodyEGT] part %d %s -> %s", BodyPart, comp, ovr.c_str());
							return m;
						}
					}
				}
			}

			return ChainBodyEGT(NPC, BodyPart, Original);
		}

		// ============================================================
		//  Entry-detour handlers
		// ============================================================

		static void __stdcall DoHeadFaceGen(TESRace* Race, TESNPC* NPC, FaceGenHeadParameters* Params)
		{
			if (g_HeadFaceGenTrampoline)
			{
				typedef void (__fastcall *OrigFn)(TESRace*, void*, TESNPC*, FaceGenHeadParameters*);
				((OrigFn)g_HeadFaceGenTrampoline)(Race, nullptr, NPC, Params);
			}

			if (!Race || !NPC || !Params) return;

			// Head overrides are opt-in. When disabled the detour is a pure
			// passthrough - the trampoline already ran the original facegen.
			if (!s_HeadOverride) return;

			__try
			{
				ApplyHeadOverrides(Race, NPC, Params);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				Log("[Head] exception during facegen override");
			}
		}

// ============================================================
//  Equipment suffix hook (retained from BES-Keywords)
// ============================================================

static void ChainEquip(ActorBodyModelData* ModelData, TESForm* ModelSource, TESModel* Model, int BodyPart)
{
	if (g_BlockheadEquipInner)
	{
		typedef void (__stdcall *EquipFn)(ActorBodyModelData*, TESForm*, TESModel*, int);
		((EquipFn)g_BlockheadEquipInner)(ModelData, ModelSource, Model, BodyPart);
	}
}

static bool TryApplyEquipSuffix(const std::string& base, const std::string& ext,
	const char* suffix, const char* suffixDesc, TESModel* Model,
	ActorBodyModelData* ModelData, TESForm* ModelSource, int BodyPart,
	std::string& outAppliedPath)
{
	if (!suffix) return false;

	std::string suffixedPath = base + '_' + suffix + ext;
	Log("  %s suffix \"%s\"  ->  %s", suffixDesc, suffix, suffixedPath.c_str());

	std::string fullPath = "Data\\Meshes\\" + suffixedPath;
	if (FileExists(fullPath.c_str()))
	{
		Log("  File check: %s  ->  FOUND", fullPath.c_str());
		ApplyTESModel(suffixedPath, Model, ModelData, ModelSource, BodyPart);
		outAppliedPath = suffixedPath;
		return true;
	}
	Log("  File check: %s  ->  not found", fullPath.c_str());
	return false;
}

static void __stdcall SuffixHandler(ActorBodyModelData* ModelData, TESForm* ModelSource,
									 TESModel* Model, int BodyPart)
{
	if (!ModelSource || !ModelData)
	{
		ChainEquip(ModelData, ModelSource, Model, BodyPart);
		return;
	}

	TESObjectREFR* Ref = ModelData->parentRef;
	if (!Ref || !Ref->baseForm || Ref->baseForm->typeID != kFormType_NPC)
	{
		ChainEquip(ModelData, ModelSource, Model, BodyPart);
		return;
	}

	TESNPC* NPC = OBLIVION_CAST(Ref->baseForm, TESForm, TESNPC);
	TESBipedModelForm* Biped = OBLIVION_CAST(ModelSource, TESForm, TESBipedModelForm);
	if (!NPC || !Biped)
	{
		ChainEquip(ModelData, ModelSource, Model, BodyPart);
		return;
	}

	Log("[Suffix] NPC=%08X  BodyPart=%d  Item=%s (%08X)",
		NPC->refID, BodyPart,
		ModelSource->GetEditorID() ? ModelSource->GetEditorID() : "?",
		ModelSource->refID);
	LogIndent();

	if (BodyPart == -1)
	{
		unsigned long index;
		if (_BitScanForward(&index, Biped->partMask))
			BodyPart = (int)index;
	}

	const char* srcPath = Model->nifPath.m_data;
	if (!srcPath || !srcPath[0])
	{
		Log("Source model path empty, chaining");
		LogOutdent();
		ChainEquip(ModelData, ModelSource, Model, BodyPart);
		return;
	}

	Log("Source path: %s", srcPath);

	std::string_view srcView(srcPath);
	size_t dot = srcView.rfind('.');
	if (dot == std::string_view::npos)
	{
		Log("No extension in source path, chaining");
		LogOutdent();
		ChainEquip(ModelData, ModelSource, Model, BodyPart);
		return;
	}

	std::string base(srcView.substr(0, dot));
	std::string ext(srcView.substr(dot));
	bool applied = false;
	std::string appliedPath;
	std::string appliedSuffix;

	// 1. invis -- slot hiding
	const std::vector<UInt32>* visibleSlots = GetInvisibleVisibleSlots(NPC->refID);
	if (visibleSlots && BodyPart >= 0)
	{
		bool slotVisible = false;
		for (auto v : *visibleSlots)
			if ((UInt32)BodyPart == v) { slotVisible = true; break; }

		if (!slotVisible)
		{
			Log("NPC %08X slot %d hidden (visible slots:", NPC->refID, BodyPart);
			for (auto v : *visibleSlots)
				Log(" %d", v);
			Log(")");

			ApplyTESModel("_invisible.nif", Model, ModelData, ModelSource, BodyPart);
			LogOutdent();
			return;
		}
		else
			Log("NPC %08X slot %d visible -- proceeding normally", NPC->refID, BodyPart);
	}

	// 2. NPC specific
	const char* npcSuffix = GetNPCSuffix(NPC->refID);
	if (npcSuffix)
	{
		if (TryApplyEquipSuffix(base, ext, npcSuffix, "NPC", Model, ModelData, ModelSource, BodyPart, appliedPath))
		{
			applied = true;
			appliedSuffix = npcSuffix;
		}
	}

	// 3. [EquipRules]
	if (!applied)
	{
		const EquipRule* rule = FindMatchingRule(Ref, NPC);
		if (rule)
		{
			Log("  Rule '%s' matched (%zu clauses)", rule->suffix.c_str(), rule->clauses.size());
			if (TryApplyEquipSuffix(base, ext, rule->suffix.c_str(), "Rule", Model, ModelData, ModelSource, BodyPart, appliedPath))
			{
				applied = true;
				appliedSuffix = rule->suffix;
			}
		}
	}

	// 5. legacy faction groups
	if (!applied)
	{
		const char* facSuffix = MatchLegacyFactionGroup(NPC);
		if (facSuffix)
		{
			Log("  Legacy faction group '%s' matched", facSuffix);
			if (TryApplyEquipSuffix(base, ext, facSuffix, "FactionGroup", Model, ModelData, ModelSource, BodyPart, appliedPath))
			{
				applied = true;
				appliedSuffix = facSuffix;
			}
		}
	}

	// 6. legacy race groups
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
			Log("Race: %s (%08X)  Female=%d", raceName, Race->refID, NPC->actorBaseData.IsFemale());

			int group = GetRaceGroup(raceName);
			if (group >= 0)
			{
				std::string raceSuffix = std::to_string(group);
				if (TryApplyEquipSuffix(base, ext, raceSuffix.c_str(), "RaceGroup", Model, ModelData, ModelSource, BodyPart, appliedPath))
				{
					applied = true;
					appliedSuffix = raceSuffix;
				}
			}
			else
				Log("Race \"%s\" not in any group", raceName);
		}
	}

	// 7. legacy class groups
	if (!applied)
	{
		const char* clsSuffix = MatchLegacyClassGroup(NPC);
		if (clsSuffix)
		{
			Log("  Legacy class group '%s' matched", clsSuffix);
			if (TryApplyEquipSuffix(base, ext, clsSuffix, "ClassGroup", Model, ModelData, ModelSource, BodyPart, appliedPath))
			{
				applied = true;
				appliedSuffix = clsSuffix;
			}
		}
	}

	if (applied)
	{
		Log("OVERRIDE APPLIED  BodyPart=%d  ->  %s", BodyPart, appliedPath.c_str());
		LogOutdent();
		return;
	}

	LogOutdent();
	Log("[Suffix] Chaining to downstream handler...");
	ChainEquip(ModelData, ModelSource, Model, BodyPart);
}

// ============================================================
//  Equipment hook stub (mirrors Blockhead's)
// ============================================================

__declspec(naked) void EquipHookStub()
{
	__asm
	{
		push	ecx
		call	SuffixHandler
		mov		eax, 0x00469235
		jmp		eax
	}
}

// ============================================================
//  OBSE keyword / messaging integration
// ============================================================

static const OBSEInterface* g_OBSE = nullptr;
static OBSEMessagingInterface* g_messaging = nullptr;
static bool g_postLoadChecked = false;

static void InstallHooks(void)
{
	Log("[Hook] installing hooks");

	// Blockhead must have installed its equipment hook and body/head hooks.
	g_BlockheadEquipInner = FindCallTargetInStub(ReadJumpTarget(kEquipHookAddress), 24);
	if (g_BlockheadEquipInner)
	{
		Log("[Hook] Blockhead equipment handler @ %08X", (UInt32)g_BlockheadEquipInner);
		WriteJump(kEquipHookAddress, (void*)EquipHookStub);
		Log("[Hook] BlockheadEquipSuffix equipment hook installed at %08X", kEquipHookAddress);
	}
	else
		Log("[Hook] ERROR: no Blockhead equipment hook at %08X", kEquipHookAddress);

	// Body asset hooks
	void* texStub = ReadJumpTarget(kBodyTexHookAddr);
	if (texStub)
	{
		g_BlockheadTexInner = FindCallTargetInStub(texStub, 24);
		WriteJump(kBodyTexHookAddr, (void*)BodyTextureHook);
		Log("[Hook] BlockheadEquipSuffix body texture hook installed at %08X (blockhead inner @ %08X)",
			kBodyTexHookAddr, (UInt32)g_BlockheadTexInner);
	}
	else
		Log("[Hook] ERROR: no Blockhead body texture hook at %08X", kBodyTexHookAddr);

	void* modelAStub = ReadJumpTarget(kBodyModelAHook);
	if (modelAStub)
	{
		g_BlockheadModelInner = FindCallTargetInStub(modelAStub, 24);
		WriteJump(kBodyModelAHook, (void*)BodyModelAHook);
		Log("[Hook] BlockheadEquipSuffix body model A hook installed at %08X", kBodyModelAHook);
	}
	else
		Log("[Hook] ERROR: no Blockhead body model hook A at %08X", kBodyModelAHook);

	void* modelBStub = ReadJumpTarget(kBodyModelBHook);
	if (modelBStub)
	{
		if (!g_BlockheadModelInner)
			g_BlockheadModelInner = FindCallTargetInStub(modelBStub, 24);
		WriteJump(kBodyModelBHook, (void*)BodyModelBHook);
		Log("[Hook] BlockheadEquipSuffix body model B hook installed at %08X", kBodyModelBHook);
	}
	else
		Log("[Hook] ERROR: no Blockhead body model hook B at %08X", kBodyModelBHook);

	void* egtStub = ReadJumpTarget(kBodyEGTHookAddr);
	if (egtStub)
	{
		g_BlockheadEGTInner = FindCallTargetInStub(egtStub, 24);
		WriteJump(kBodyEGTHookAddr, (void*)BodyEGTHook);
		Log("[Hook] BlockheadEquipSuffix body EGT hook installed at %08X (blockhead inner @ %08X)",
			kBodyEGTHookAddr, (UInt32)g_BlockheadEGTInner);
	}
	else
		Log("[Hook] ERROR: no Blockhead body EGT hook at %08X", kBodyEGTHookAddr);

	// Head facegen entry detour.
	// First instructions at 0x52CD50: "6A FF" (push 0xFF, 2 bytes) +
	// "68 08 6B 9D 00" (push offset, 5 bytes) = 7 bytes total, ending
	// right before the SEH registration (mov eax, fs:0). Copy exactly 7.
	g_HeadFaceGenTrampoline = BuildTrampoline(kHeadFaceGenFn, 7);
	if (g_HeadFaceGenTrampoline)
	{
		PatchJumpN(kHeadFaceGenFn, (void*)HeadFaceGenHook, 7);
		Log("[Hook] BlockheadEquipSuffix head facegen detour installed at %08X (trampoline @ %08X)",
			kHeadFaceGenFn, (UInt32)g_HeadFaceGenTrampoline);
	}
	else
		Log("[Hook] ERROR: could not build head facegen trampoline at %08X", kHeadFaceGenFn);

	if (EquipSuffix::s_LogFile) fflush(EquipSuffix::s_LogFile);
}

static void UnifiedMessageHandler(OBSEMessagingInterface::Message* msg)
{
	if (!msg) return;

	if (msg->type == OBSEMessagingInterface::kMessage_PostLoad)
	{
		if (g_postLoadChecked) return;
		g_postLoadChecked = true;

		if (!g_OBSE)
		{
			EquipSuffix::Log("[MSG] ERROR: no OBSE interface stored");
			return;
		}

		InstallHooks();
	}
}

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
		EquipSuffix::ParseINI();

		g_OBSE = obse;
		g_pluginHandle = obse->GetPluginHandle();

		g_messaging = (OBSEMessagingInterface*)obse->QueryInterface(kInterface_Messaging);
		if (!g_messaging)
		{
			EquipSuffix::Log("[MSG] ERROR: messaging interface unavailable");
			return false;
		}

		// Register with sender "OBSE" to receive OBSE-core messages
		// (kMessage_PostLoad), so InstallHooks runs once all plugins load.
		// (RegisterListener's 2nd arg is the SENDER name, not an event mask.)
		g_messaging->RegisterListener(g_pluginHandle, "OBSE", UnifiedMessageHandler);

		EquipSuffix::Log("BlockheadEquipSuffix 1.0 loaded; hook install deferred to PostLoad.");
		if (EquipSuffix::s_LogFile) fflush(EquipSuffix::s_LogFile);
		return true;
	}
}

BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved)
{
	if (dwReason == DLL_PROCESS_DETACH)
		EquipSuffix::Clear();
	return TRUE;
}
