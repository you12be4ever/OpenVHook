#include "ScriptManager.h"
#include "ScriptEngine.h"
#include "..\Utility\Log.h"
#include "..\Utility\General.h"
#include "..\Addresses.h"
#include "Types.h"

using namespace Utility;

#pragma comment(lib, "winmm.lib")
#define DLL_EXPORT __declspec( dllexport )

enum eGameVersion;

ScriptManagerThread g_ScriptManagerThread;

static HANDLE		mainFiber;
static Script *		currentScript;

std::mutex mutex;

void Script::Tick() {

	if ( mainFiber == nullptr ) {
		mainFiber = ConvertThreadToFiber( nullptr );
	}

	if ( timeGetTime() < wakteAt ) {
		return;
	}

	if ( scriptFiber ) {

		currentScript = this;
		SwitchToFiber( scriptFiber );
		currentScript = nullptr;
	}

	else if (ScriptEngine::GetGameState() == GameStatePlaying) {

		LOG_PRINT("Launching main()");

		scriptFiber = CreateFiber(NULL, [](LPVOID handler) {
			__try {

				reinterpret_cast<Script*>(handler)->Run();
			} __except (EXCEPTION_EXECUTE_HANDLER) {

				LOG_ERROR("Error in script->Run");
			}
		}, this);
	}
}

void Script::Run() {
	callbackFunction();
}

void Script::Yield( uint32_t time ) {

	wakteAt = timeGetTime() + time;
	SwitchToFiber( mainFiber );
}

void ScriptManagerThread::DoRun() {

	std::unique_lock<std::mutex> lock(mutex);

	scriptMap thisIterScripts( m_scripts );

	for ( auto & pair : thisIterScripts ) {
		pair.second->Tick();
	}
}

eThreadState ScriptManagerThread::Reset( uint32_t scriptHash, void * pArgs, uint32_t argCount ) {

	// Collect all scripts
	scriptMap tempScripts;

	for ( auto && pair : m_scripts ) {
		tempScripts[pair.first] = pair.second;
	}

	// Clear the scripts
	m_scripts.clear();

	// Start all scripts
	for ( auto && pair : tempScripts ) {
		AddScript( pair.first, pair.second->GetCallbackFunction() );
	}

	return ScriptThread::Reset( scriptHash, pArgs, argCount );
}

void ScriptManagerThread::AddScript( HMODULE module, void( *fn )( ) ) {

	const std::string moduleName = GetModuleNameWithoutExtension( module );

	LOG_PRINT( "Registering script '%s' (0x%p)", moduleName.c_str(), fn );

	std::unique_lock<std::mutex> lock(mutex);

	if ( m_scripts.find( module ) != m_scripts.end() ) {

		LOG_ERROR( "Script '%s' is already registered", moduleName.c_str() );
		return;
	}

	m_scripts[module] = std::make_shared<Script>( fn );
}

void ScriptManagerThread::RemoveScript( void( *fn )( ) ) {

	for ( auto it = m_scripts.begin(); it != m_scripts.end(); ++it ) {

		auto pair = *it;
		if ( pair.second->GetCallbackFunction() == fn ) {

			RemoveScript( pair.first );
			break;
		}
	}
}

void ScriptManagerThread::RemoveScript( HMODULE module ) {

	std::unique_lock<std::mutex> lock(mutex);

	auto pair = m_scripts.find( module );
	if ( pair == m_scripts.end() ) {

		LOG_ERROR( "Could not find script for module 0x%p", module );
		return;
	}

	LOG_PRINT( "Unregistered script '%s'", GetModuleNameWithoutExtension( module ).c_str() );
	m_scripts.erase( pair );
}

void DLL_EXPORT scriptWait( unsigned long waitTime ) {

	currentScript->Yield( waitTime );
}

void DLL_EXPORT scriptRegister( HMODULE module, void( *function )( ) ) {

	g_ScriptManagerThread.AddScript( module, function );
}

void DLL_EXPORT scriptUnregister( void( *function )( ) ) {

	g_ScriptManagerThread.RemoveScript( function );
}

void DLL_EXPORT scriptUnregister( HMODULE module ) {
	
	g_ScriptManagerThread.RemoveScript( module );
}

eGameVersion DLL_EXPORT getGameVersion() {

	return (eGameVersion)gameVersion;
}

void DLL_EXPORT scriptRegisterAdditionalThread( HMODULE module, void( *function )( ) ) {

	// TODO: Implement this at some point, to lazy right now
	LOG_WARNING( "Plugin is trying to use 'scriptRegisterAdditionalThread' Implement me!!" );
}

static ScriptManagerContext g_context;
static uint64_t g_hash;

void DLL_EXPORT nativeInit( uint64_t hash ) {

	g_context.Reset();
	g_hash = hash;
}

void DLL_EXPORT nativePush64( uint64_t value ) {

	g_context.Push( value );
}

DLL_EXPORT uint64_t * nativeCall() {

	auto fn = ScriptEngine::GetNativeHandler( g_hash );

	if ( fn != 0 ) {

		__try {

			fn( &g_context );
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {

			LOG_ERROR( "Error in nativeCall" );
		}
	}

	return reinterpret_cast<uint64_t*>( g_context.GetResultPointer() );
}

typedef void( *TKeyboardFn )( DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow );

static std::set<TKeyboardFn> g_keyboardFunctions;

void DLL_EXPORT keyboardHandlerRegister( TKeyboardFn function ) {

	g_keyboardFunctions.insert( function );
}

void DLL_EXPORT keyboardHandlerUnregister( TKeyboardFn function ) {

	g_keyboardFunctions.erase( function );
}

void ScriptManager::HandleKeyEvent(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow) {

	auto functions = g_keyboardFunctions;

	for (auto & function : functions) {
		function(key, repeats, scanCode, isExtended, isWithAlt, wasDownBefore, isUpNow);
	}
}

DLL_EXPORT uint64_t* getGlobalPtr(int index)
{
	return (uint64_t*)globalTable.AddressOf(index);
}

BYTE DLL_EXPORT *getScriptHandleBaseAddress(int handle)
{
	auto scriptEntityPool = *(fwPool<fwScriptGuid>**)entityPoolAddressArr[gameVersion];

	int index = handle >> 8;

	if (index > scriptEntityPool->m_count || !scriptEntityPool->isValid(index)) return NULL;

	auto * poolObj = scriptEntityPool->m_pData + index * scriptEntityPool->m_itemSize;

	return poolObj ? reinterpret_cast<BYTE*>(poolObj->m_pEntity) : NULL;
}

int DLL_EXPORT worldGetAllVehicles(int* array, int arraySize)
{
	auto vehiclePool = **(VehiclePool***)vehiclePoolAddressArr[gameVersion];
	auto scriptEntityPool = *(fwPool<fwScriptGuid>**)entityPoolAddressArr[gameVersion];
	auto getScriptHandleFn = (int32_t(*)(LPVOID))getEntityScrHandleAddressArr[gameVersion];

	if (vehiclePool->m_count <= 0)
		return 0;

	int index = 0;

	for (auto i = 0; i < vehiclePool->m_count; i++)
	{
		if (i >= arraySize || i >= vehiclePool->m_count || scriptEntityPool->full())
			break;

		auto address = vehiclePool->getAddress(i);

		if (!vehiclePool->isValid(i) || !address)
			continue;

		array[index++] = getScriptHandleFn(address);
	}

	return index;
}

int DLL_EXPORT worldGetAllPeds(int* array, int arraySize)
{
	auto pedPool = *(fwGenericPool**)pedPoolAddressArr[gameVersion];
	auto scriptEntityPool = *(fwPool<fwScriptGuid>**)entityPoolAddressArr[gameVersion];
	auto getScriptHandleFn = (int32_t(*)(LPVOID))getEntityScrHandleAddressArr[gameVersion];

	if (pedPool->m_count <= 0)
		return 0;

	int index = 0;

	for (auto i = 0; i < pedPool->m_count; i++)
	{
		if (i >= arraySize || i >= pedPool->m_count || scriptEntityPool->full())
			break;

		auto current = pedPool->m_pData + i * pedPool->m_itemSize;

		if (!pedPool->isValid(i) || !current)
			continue;

		array[index++] = getScriptHandleFn(current);
	}

	return index;
}

int DLL_EXPORT worldGetAllObjects(int* array, int arraySize)
{
	auto objectPool = (fwGenericPool*)objectPoolAddressArr[gameVersion];
	auto scriptEntityPool = (fwPool<fwScriptGuid>*)entityPoolAddressArr[gameVersion];
	auto getScriptHandleFn = (int32_t(*)(LPVOID))getEntityScrHandleAddressArr[gameVersion];

	if (objectPool->m_count <= 0)
		return 0;

	int index = 0;

	for (auto i = 0; i < objectPool->m_count; i++)
	{
		if (i >= arraySize || i >= objectPool->m_count || scriptEntityPool->full())
			break;

		auto current = objectPool->m_pData + i * objectPool->m_itemSize;

		if (!objectPool->isValid(i) || !current)
			continue;

		array[index++] = getScriptHandleFn(current);
	}

	return index;
}

int DLL_EXPORT worldGetAllPickups(int* array, int arraySize)
{
	auto pickupPool = (fwGenericPool*)pickupPoolAddressArr[gameVersion];
	auto scriptEntityPool = (fwPool<fwScriptGuid>*)entityPoolAddressArr[gameVersion];
	auto getScriptHandleFn = (int32_t(*)(LPVOID))getEntityScrHandleAddressArr[gameVersion];

	if (pickupPool->m_count <= 0)
		return 0;

	int index = 0;

	for (auto i = 0; i < pickupPool->m_count; i++)
	{
		if (i >= arraySize || i >= pickupPool->m_count || scriptEntityPool->full())
			break;

		auto current = pickupPool->m_pData + i * pickupPool->m_itemSize;

		if (!pickupPool->isValid(i) || !current)
			continue;

		array[index++] = getScriptHandleFn(current);
	}

	return index;
}

DLL_EXPORT int createTexture(const char* fileName)
{	
	LOG_WARNING("plugin is trying to use createTexture");
	return 0;
}

DLL_EXPORT void drawTexture(int id, int index, int level, int time,
	float sizeX, float sizeY, float centerX, float centerY,
	float posX, float posY, float rotation, float screenHeightScaleFactor,
	float r, float g, float b, float a)
{
	LOG_WARNING("plugin is trying to use drawTexture");
}

