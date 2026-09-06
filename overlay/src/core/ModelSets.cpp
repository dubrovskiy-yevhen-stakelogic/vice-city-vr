#include "common.h"

#include "ModelSets.h"

#ifdef _WIN32
#include <windows.h>
#define MODELSET_STRICMP _stricmp
#define MODELSET_STRNICMP _strnicmp
#else
#include <strings.h>
#define MODELSET_STRICMP strcasecmp
#define MODELSET_STRNICMP strncasecmp
#endif

namespace ModelSets
{
namespace
{
eModelSet gActiveModelSet = MODEL_SET_CLASSIC;
eModelSet gRequestedModelSet = MODEL_SET_CLASSIC;
bool gInitialized;
bool gModernArchivePairAvailable;
bool gOptimizedVegetationArchivePairAvailable;
bool gXboxArchivePairAvailable;
eModelSet gActiveCategorySet[MODEL_CATEGORY_COUNT];
eModelSet gRequestedCategorySet[MODEL_CATEGORY_COUNT];
enum { MAX_VEGETATION_MODELS = 512, MAX_MANIFEST_MODEL_NAME = 24 };
char gVegetationModels[MAX_VEGETATION_MODELS][MAX_MANIFEST_MODEL_NAME] = {};
int gNumVegetationModels;
bool gVegetationManifestAvailable;
char gGameRoot[1024] = {};
char gSettingsPath[1024] = {};

const char *const gCategorySettingNames[MODEL_CATEGORY_COUNT] = {
	"ModelSetWorld",
	"ModelSetVegetation",
	"ModelSetVehicles",
	"ModelSetPeds",
	"ModelSetWeapons"
};

const char *const gCategoryNames[MODEL_CATEGORY_COUNT] = {
	"WORLD / BUILDINGS",
	"VEGETATION / PALMS",
	"VEHICLES",
	"PEDESTRIANS",
	"WEAPONS"
};

// The HD vegetation source is dramatically heavier than the Classic trees
// (especially the palms, which are repeated hundreds of times around the
// city).  Keep every other Modern category enabled for a fresh install, but
// make vegetation an explicit opt-in.  An existing INI value still wins.
const int gCategoryDefaults[MODEL_CATEGORY_COUNT] = {
	1, // world / buildings
	0, // vegetation / palms
	1, // vehicles
	1, // pedestrians
	1  // weapons
};

bool FileExists(const char *path)
{
#ifdef _WIN32
	const DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
	FILE *file = fopen(path, "rb");
	if(!file)
		return false;
	fclose(file);
	return true;
#endif
}

void NormalizeRoot(const char *root)
{
	gGameRoot[0] = '\0';
	if(root && root[0] != '\0')
		strncpy(gGameRoot, root, sizeof(gGameRoot)-1);
	gGameRoot[sizeof(gGameRoot)-1] = '\0';
	const size_t length = strlen(gGameRoot);
	if(length > 0 && gGameRoot[length-1] != '\\' &&
	   gGameRoot[length-1] != '/'){
		if(length+1 < sizeof(gGameRoot)){
			gGameRoot[length] = '\\';
			gGameRoot[length+1] = '\0';
		}
	}
}

void FindSettingsPath()
{
#ifdef _WIN32
	const DWORD length = GetModuleFileNameA(nil, gSettingsPath,
		(DWORD)ARRAY_SIZE(gSettingsPath));
	if(length > 0 && length < ARRAY_SIZE(gSettingsPath)){
		char *separator = strrchr(gSettingsPath, '\\');
		if(!separator)
			separator = strrchr(gSettingsPath, '/');
		if(separator){
			strcpy(separator+1, "vr_settings.ini");
			return;
		}
	}
#endif
	snprintf(gSettingsPath, sizeof(gSettingsPath), "%svr_settings.ini",
		gGameRoot);
}

bool IsSafeModelsPath(const char *path)
{
	if(!path || path[0] == '\0' || path[1] == ':')
		return false;
	if(path[0] == '\\' || path[0] == '/')
		return false;
	if(strstr(path, "..") != nil)
		return false;
	return MODELSET_STRNICMP(path, "models\\", 7) == 0 ||
		MODELSET_STRNICMP(path, "models/", 7) == 0 ||
		MODELSET_STRNICMP(path, "txd\\", 4) == 0 ||
		MODELSET_STRNICMP(path, "txd/", 4) == 0;
}

void BuildProfilePath(const char *profile, const char *relativePath,
	char *destination, size_t destinationSize)
{
	char normalized[512];
	strncpy(normalized, relativePath, sizeof(normalized)-1);
	normalized[sizeof(normalized)-1] = '\0';
	for(char *cursor = normalized; *cursor; cursor++)
		if(*cursor == '/')
			*cursor = '\\';
	snprintf(destination, destinationSize, "%smodelsets\\%s\\%s",
		gGameRoot, profile, normalized);
}

void BuildModernPath(const char *relativePath, char *destination,
	size_t destinationSize)
{
	BuildProfilePath("modern", relativePath, destination, destinationSize);
}

bool ArchivePairExists(const char *profile)
{
	char imagePath[1024], directoryPath[1024];
	BuildProfilePath(profile, "models\\gta3.img", imagePath,
		sizeof(imagePath));
	BuildProfilePath(profile, "models\\gta3.dir", directoryPath,
		sizeof(directoryPath));
	return FileExists(imagePath) && FileExists(directoryPath);
}

bool IsGta3ArchivePath(const char *relative)
{
	return MODELSET_STRICMP(relative, "models\\gta3.img") == 0 ||
		MODELSET_STRICMP(relative, "models/gta3.img") == 0 ||
		MODELSET_STRICMP(relative, "models\\gta3.dir") == 0 ||
		MODELSET_STRICMP(relative, "models/gta3.dir") == 0;
}

void LoadVegetationManifest()
{
	gNumVegetationModels = 0;
	gVegetationManifestAvailable = false;
	char path[1024];
	BuildProfilePath("optimized-vegetation", "vegetation_models.txt", path,
		sizeof(path));
	FILE *file = fopen(path, "rt");
	if(!file){
		BuildModernPath("vegetation_models.txt", path, sizeof(path));
		file = fopen(path, "rt");
	}
	if(!file){
		debug("Model set: no vegetation manifest at %s; vegetation cannot be separated from world assets\n", path);
		return;
	}

	char line[256];
	while(gNumVegetationModels < MAX_VEGETATION_MODELS &&
	      fgets(line, sizeof(line), file)){
		char *begin = line;
		while(*begin == ' ' || *begin == '\t')
			begin++;
		char *end = begin + strlen(begin);
		while(end > begin && (end[-1] == '\r' || end[-1] == '\n' ||
		      end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		if(begin[0] == '\0' || begin[0] == '#' || begin[0] == ';')
			continue;
		char *extension = strrchr(begin, '.');
		if(extension && MODELSET_STRICMP(extension, ".dff") == 0)
			*extension = '\0';
		strncpy(gVegetationModels[gNumVegetationModels], begin,
			MAX_MANIFEST_MODEL_NAME-1);
		gVegetationModels[gNumVegetationModels][MAX_MANIFEST_MODEL_NAME-1] = '\0';
		gNumVegetationModels++;
	}
	fclose(file);
	gVegetationManifestAvailable = gNumVegetationModels > 0;
	debug("Model set: loaded %d vegetation model names\n",
		gNumVegetationModels);
}
}

void InitializeStartup(const char *gameRoot)
{
	if(gInitialized)
		return;
	NormalizeRoot(gameRoot);
	FindSettingsPath();
#ifdef _WIN32
	int requested = (int)(int32)GetPrivateProfileIntA("VR", "ModelSet",
		MODEL_SET_CLASSIC, gSettingsPath);
#else
	int requested = MODEL_SET_CLASSIC;
#endif
	gModernArchivePairAvailable = ArchivePairExists("modern");
	gOptimizedVegetationArchivePairAvailable =
		ArchivePairExists("optimized-vegetation");
	gXboxArchivePairAvailable = ArchivePairExists("xbox");
	LoadVegetationManifest();
	// The global preset is deliberately binary. XBOX is meaningful only for
	// the vehicle category and is persisted by ModelSetVehicles.
	requested = Min(Max(requested, (int)MODEL_SET_CLASSIC),
		(int)MODEL_SET_MODERN);
	if(requested == MODEL_SET_MODERN && !gModernArchivePairAvailable){
		debug("Model set: requested Modern overlay is incomplete; using base install\n");
		requested = MODEL_SET_CLASSIC;
#ifdef _WIN32
		WritePrivateProfileStringA("VR", "ModelSet", "0", gSettingsPath);
#endif
	}
	gRequestedModelSet = (eModelSet)requested;
	gActiveModelSet = gRequestedModelSet;
	for(int category = 0; category < MODEL_CATEGORY_COUNT; category++){
#ifdef _WIN32
		int categorySet = (int)(int32)GetPrivateProfileIntA("VR",
			gCategorySettingNames[category], gCategoryDefaults[category],
			gSettingsPath);
#else
		int categorySet = gCategoryDefaults[category];
#endif
		categorySet = Min(Max(categorySet, (int)MODEL_SET_CLASSIC),
			(int)MODEL_SET_XBOX);
		if(categorySet == MODEL_SET_XBOX &&
		   (category != MODEL_CATEGORY_VEHICLES ||
		    !gXboxArchivePairAvailable))
			categorySet = MODEL_SET_CLASSIC;
		if(categorySet == MODEL_SET_MODERN &&
		   (!gModernArchivePairAvailable ||
		    (category == MODEL_CATEGORY_VEGETATION &&
		     !gVegetationManifestAvailable)))
			categorySet = MODEL_SET_CLASSIC;
		gActiveCategorySet[category] = (eModelSet)categorySet;
		gRequestedCategorySet[category] = (eModelSet)categorySet;
	}
	gInitialized = true;
	debug("Model set: active=%s requested=%s archives modern/optimized/xbox=%d/%d/%d categories W/V/C/P/G=%s/%s/%s/%s/%s\n",
		GetName(gActiveModelSet), GetName(gRequestedModelSet),
		gModernArchivePairAvailable ? 1 : 0,
		gOptimizedVegetationArchivePairAvailable ? 1 : 0,
		gXboxArchivePairAvailable ? 1 : 0,
		GetName(GetActiveForCategory(MODEL_CATEGORY_WORLD)),
		GetName(GetActiveForCategory(MODEL_CATEGORY_VEGETATION)),
		GetName(GetActiveForCategory(MODEL_CATEGORY_VEHICLES)),
		GetName(GetActiveForCategory(MODEL_CATEGORY_PEDS)),
		GetName(GetActiveForCategory(MODEL_CATEGORY_WEAPONS)));
}

eModelSet GetActive()
{
	return gActiveModelSet;
}

eModelSet GetRequested()
{
	return gRequestedModelSet;
}

void SetRequested(eModelSet modelSet)
{
	if(modelSet < MODEL_SET_CLASSIC || modelSet > MODEL_SET_MODERN)
		modelSet = MODEL_SET_CLASSIC;
	if(modelSet == MODEL_SET_MODERN && !gModernArchivePairAvailable)
		modelSet = MODEL_SET_CLASSIC;
	gRequestedModelSet = modelSet;
#ifdef _WIN32
	char value[16];
	sprintf(value, "%d", (int)modelSet);
	WritePrivateProfileStringA("VR", "ModelSet", value, gSettingsPath);
#endif
}

void CycleRequested(int direction)
{
	if(!gModernArchivePairAvailable){
		SetRequested(MODEL_SET_CLASSIC);
		return;
	}
	int requested = ((int)gRequestedModelSet+2+direction) % 2;
	SetRequested((eModelSet)requested);
}

bool IsModernActive()
{
	return gActiveModelSet == MODEL_SET_MODERN;
}

bool IsRestartRequired()
{
	if(gRequestedModelSet != gActiveModelSet)
		return true;
	if(gActiveModelSet != MODEL_SET_MODERN)
		return false;
	for(int category = 0; category < MODEL_CATEGORY_COUNT; category++)
		if(gRequestedCategorySet[category] !=
		   gActiveCategorySet[category])
			return true;
	return false;
}

bool IsAvailable(eModelSet modelSet)
{
	return modelSet == MODEL_SET_CLASSIC ||
		(modelSet == MODEL_SET_MODERN && gModernArchivePairAvailable);
}

const char *GetName(eModelSet modelSet)
{
	if(modelSet == MODEL_SET_XBOX)
		return "XBOX";
	return modelSet == MODEL_SET_MODERN ? "MODERN" : "CLASSIC";
}

const char *GetSourceName(eModelSet modelSet)
{
	if(modelSet == MODEL_SET_XBOX)
		return "XBOX VEHICLE OVERLAY";
	return modelSet == MODEL_SET_MODERN ? "MODERN OVERLAY" : "BASE INSTALL";
}

eModelSet GetActiveForCategory(eModelCategory category)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT ||
	   gActiveModelSet != MODEL_SET_MODERN)
		return MODEL_SET_CLASSIC;
	const eModelSet selected = gActiveCategorySet[category];
	if(selected == MODEL_SET_XBOX)
		return category == MODEL_CATEGORY_VEHICLES &&
			gXboxArchivePairAvailable ? MODEL_SET_XBOX : MODEL_SET_CLASSIC;
	if(selected != MODEL_SET_MODERN || !gModernArchivePairAvailable ||
	   (category == MODEL_CATEGORY_VEGETATION &&
	    !gVegetationManifestAvailable))
		return MODEL_SET_CLASSIC;
	return selected;
}

eModelSet GetRequestedForCategory(eModelCategory category)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT ||
	   gRequestedModelSet != MODEL_SET_MODERN)
		return MODEL_SET_CLASSIC;
	const eModelSet selected = gRequestedCategorySet[category];
	if(selected == MODEL_SET_XBOX)
		return category == MODEL_CATEGORY_VEHICLES &&
			gXboxArchivePairAvailable ? MODEL_SET_XBOX : MODEL_SET_CLASSIC;
	if(selected != MODEL_SET_MODERN || !gModernArchivePairAvailable ||
	   (category == MODEL_CATEGORY_VEGETATION &&
	    !gVegetationManifestAvailable))
		return MODEL_SET_CLASSIC;
	return selected;
}

void SetRequestedForCategory(eModelCategory category, eModelSet modelSet)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT)
		return;
	if(modelSet == MODEL_SET_XBOX &&
	   (category != MODEL_CATEGORY_VEHICLES || !gXboxArchivePairAvailable))
		modelSet = MODEL_SET_CLASSIC;
	if(modelSet == MODEL_SET_MODERN &&
	   (!gModernArchivePairAvailable ||
	    (category == MODEL_CATEGORY_VEGETATION &&
	     !gVegetationManifestAvailable)))
		modelSet = MODEL_SET_CLASSIC;
	if(modelSet < MODEL_SET_CLASSIC || modelSet >= MODEL_SET_COUNT)
		modelSet = MODEL_SET_CLASSIC;
	gRequestedCategorySet[category] = modelSet;
#ifdef _WIN32
	char value[16];
	sprintf(value, "%d", (int)modelSet);
	WritePrivateProfileStringA("VR", gCategorySettingNames[category],
		value, gSettingsPath);
#endif
}

void CycleRequestedCategory(eModelCategory category, int direction)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT || direction == 0)
		return;
	eModelSet candidates[3] = {
		MODEL_SET_CLASSIC, MODEL_SET_MODERN, MODEL_SET_XBOX
	};
	const int count = category == MODEL_CATEGORY_VEHICLES ? 3 : 2;
	int current = 0;
	for(int i = 0; i < count; i++)
		if(candidates[i] == gRequestedCategorySet[category])
			current = i;
	for(int attempt = 0; attempt < count; attempt++){
		current = (current+count+(direction > 0 ? 1 : -1)) % count;
		const eModelSet candidate = candidates[current];
		if(candidate == MODEL_SET_CLASSIC ||
		   (candidate == MODEL_SET_MODERN && IsCategoryAvailable(category)) ||
		   (candidate == MODEL_SET_XBOX && category == MODEL_CATEGORY_VEHICLES &&
		    gXboxArchivePairAvailable)){
			SetRequestedForCategory(category, candidate);
			return;
		}
	}
}

bool IsCategoryModernActive(eModelCategory category)
{
	return GetActiveForCategory(category) == MODEL_SET_MODERN;
}

bool IsCategoryModernRequested(eModelCategory category)
{
	return GetRequestedForCategory(category) == MODEL_SET_MODERN;
}

bool IsCategoryRestartRequired(eModelCategory category)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT)
		return false;
	return GetActiveForCategory(category) != GetRequestedForCategory(category);
}

bool IsCategoryAvailable(eModelCategory category)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT ||
	   !gModernArchivePairAvailable)
		return false;
	return category != MODEL_CATEGORY_VEGETATION ||
		gVegetationManifestAvailable;
}

const char *GetCategoryName(eModelCategory category)
{
	if(category < 0 || category >= MODEL_CATEGORY_COUNT)
		return "UNKNOWN";
	return gCategoryNames[category];
}

void DeactivateModernForSession(void)
{
	// Only the running session is downgraded; gRequestedModelSet and the
	// per-category requests stay untouched so the next launch tries the
	// Modern archive again once it is readable.
	gActiveModelSet = MODEL_SET_CLASSIC;
	for(int category = 0; category < MODEL_CATEGORY_COUNT; category++)
		gActiveCategorySet[category] = MODEL_SET_CLASSIC;
	debug("Model set: Modern deactivated for this session, using Classic\n");
}

void DeactivateOptimizedVegetationForSession(void)
{
	gOptimizedVegetationArchivePairAvailable = false;
	debug("Model set: optimized vegetation overlay unavailable; using Modern vegetation fallback\n");
}

void DeactivateXboxForSession(void)
{
	gXboxArchivePairAvailable = false;
	if(gActiveCategorySet[MODEL_CATEGORY_VEHICLES] == MODEL_SET_XBOX)
		gActiveCategorySet[MODEL_CATEGORY_VEHICLES] = MODEL_SET_CLASSIC;
	debug("Model set: Xbox vehicle overlay unavailable; using Classic vehicles\n");
}

bool HasVegetationManifest()
{
	return gVegetationManifestAvailable;
}

bool IsVegetationModel(const char *modelName)
{
	if(!modelName || !gVegetationManifestAvailable)
		return false;
	for(int i = 0; i < gNumVegetationModels; i++)
		if(MODELSET_STRICMP(modelName, gVegetationModels[i]) == 0)
			return true;
	return false;
}

bool GetModernAssetPath(const char *relativePath, char *resolvedPath,
	size_t resolvedPathSize)
{
	if(!relativePath || !resolvedPath || resolvedPathSize == 0 ||
	   !gModernArchivePairAvailable)
		return false;
	BuildModernPath(relativePath, resolvedPath, resolvedPathSize);
	return FileExists(resolvedPath);
}

bool IsModernAssetPath(const char *path)
{
	if(!path || !gInitialized)
		return false;
	char modernRoot[1024];
	snprintf(modernRoot, sizeof(modernRoot), "%smodelsets\\modern\\",
		gGameRoot);
	return MODELSET_STRNICMP(path, modernRoot, strlen(modernRoot)) == 0;
}

bool GetOptimizedVegetationAssetPath(const char *relativePath,
	char *resolvedPath, size_t resolvedPathSize)
{
	if(!relativePath || !resolvedPath || resolvedPathSize == 0 ||
	   !gOptimizedVegetationArchivePairAvailable)
		return false;
	BuildProfilePath("optimized-vegetation", relativePath, resolvedPath,
		resolvedPathSize);
	return FileExists(resolvedPath);
}

bool IsOptimizedVegetationAssetPath(const char *path)
{
	if(!path || !gInitialized)
		return false;
	char profileRoot[1024];
	snprintf(profileRoot, sizeof(profileRoot),
		"%smodelsets\\optimized-vegetation\\", gGameRoot);
	return MODELSET_STRNICMP(path, profileRoot, strlen(profileRoot)) == 0;
}

bool GetXboxAssetPath(const char *relativePath, char *resolvedPath,
	size_t resolvedPathSize)
{
	if(!relativePath || !resolvedPath || resolvedPathSize == 0 ||
	   !gXboxArchivePairAvailable)
		return false;
	BuildProfilePath("xbox", relativePath, resolvedPath, resolvedPathSize);
	return FileExists(resolvedPath);
}

bool IsXboxAssetPath(const char *path)
{
	if(!path || !gInitialized)
		return false;
	char profileRoot[1024];
	snprintf(profileRoot, sizeof(profileRoot), "%smodelsets\\xbox\\",
		gGameRoot);
	return MODELSET_STRNICMP(path, profileRoot, strlen(profileRoot)) == 0;
}

bool IsOverlayAssetPath(const char *path)
{
	return IsModernAssetPath(path) ||
		IsOptimizedVegetationAssetPath(path) || IsXboxAssetPath(path);
}

const char *ResolveAssetPath(const char *originalPath, char *resolvedPath,
	size_t resolvedPathSize)
{
	if(!originalPath || !resolvedPath || resolvedPathSize == 0 ||
	   !gInitialized || !IsModernActive())
		return originalPath;
	const char *relative = originalPath;
	while(relative[0] == '.' &&
	      (relative[1] == '\\' || relative[1] == '/'))
		relative += 2;
	if(!IsSafeModelsPath(relative))
		return originalPath;
	// GTA3 is special: keep the base archive as image zero and let the
	// streaming directory register selected entries from a second Modern image.
	// Loose TXDs still use the ordinary Modern fallback behavior.
	if(IsGta3ArchivePath(relative))
		return originalPath;
	eModelCategory category = MODEL_CATEGORY_WORLD;
	if(MODELSET_STRNICMP(relative, "models\\coll\\", 12) == 0 ||
	   MODELSET_STRNICMP(relative, "models/coll/", 12) == 0 ||
	   MODELSET_STRNICMP(relative, "models\\generic\\", 15) == 0 ||
	   MODELSET_STRNICMP(relative, "models/generic/", 15) == 0)
		category = MODEL_CATEGORY_VEHICLES;
	else if(MODELSET_STRICMP(relative, "models\\generic.txd") == 0 ||
	        MODELSET_STRICMP(relative, "models/generic.txd") == 0)
		category = MODEL_CATEGORY_VEGETATION;
	// This one loose collision library belongs to the optimized vegetation
	// pack, while vehicles.col and the generic/wheels pair are vehicle assets.
	if(MODELSET_STRICMP(relative, "models\\coll\\generic.col") == 0 ||
	   MODELSET_STRICMP(relative, "models/coll/generic.col") == 0)
		category = MODEL_CATEGORY_VEGETATION;
	const eModelSet selected = GetActiveForCategory(category);
	if(selected == MODEL_SET_CLASSIC)
		return originalPath;
	if(selected == MODEL_SET_XBOX){
		BuildProfilePath("xbox", relative, resolvedPath, resolvedPathSize);
		return FileExists(resolvedPath) ? resolvedPath : originalPath;
	}
	if(category == MODEL_CATEGORY_VEGETATION &&
	   gOptimizedVegetationArchivePairAvailable){
		BuildProfilePath("optimized-vegetation", relative, resolvedPath,
			resolvedPathSize);
		if(FileExists(resolvedPath))
			return resolvedPath;
	}
	BuildModernPath(relative, resolvedPath, resolvedPathSize);
	return FileExists(resolvedPath) ? resolvedPath : originalPath;
}
}
