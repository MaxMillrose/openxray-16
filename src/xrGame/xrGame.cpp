////////////////////////////////////////////////////////////////////////////
//	Module 		: xrGame.cpp
//	Created 	: 07.01.2001
//  Modified 	: 27.05.2004
//	Author		: Aleksandr Maksimchuk and Oles' Shyshkovtsov
//	Description : Defines the entry point for the DLL application.
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "xrGame.h"

#include "GamePersistent.h"
#include "object_factory.h"

#include "xrEngine/xr_level_controller.h"
#include "xrEngine/profiler.h"

#include "xrUICore/XML/xrUIXmlParser.h"
#include "xrUICore/ui_styles.h"

xrGameModule xrGame;

void CCC_RegisterCommands();

extern float g_fTimeFactor;

extern "C"
{
XR_EXPORT IFactoryObject* __cdecl xrFactory_Create(CLASS_ID clsid)
{
    IFactoryObject* object = object_factory().client_object(clsid);
#ifdef DEBUG
    if (!object)
        return (0);
#endif
    // XXX nitrocaster XRFACTORY: set clsid during factory initialization
    object->GetClassId() = clsid;
    return (object);
}

XR_EXPORT void __cdecl xrFactory_Destroy(IFactoryObject* O) { xr_delete(O); }
}

void xrGameModule::initialize(Factory_Create*& pCreate, Factory_Destroy*& pDestroy)
{
    ZoneScoped;

    pCreate = &xrFactory_Create;
    pDestroy = &xrFactory_Destroy;

    g_fTimeFactor = pSettings->r_float("alife", "time_factor"); // XXX: find a better place

    // Fill ui style token
    UIStyles = xr_new<UIStyleManager>();
    // register console commands
    CCC_RegisterCommands();
    // register localization
    StringTable().Init();
    // keyboard binding
    CCC_RegisterInput(); // XXX: Move to xrEngine
#ifdef DEBUG
    g_profiler = xr_new<CProfiler>();
#endif

    ImGui::SetAllocatorFunctions(
        [](size_t size, void* /*user_data*/)
        {
            return xr_malloc(size);
        },
        [](void* ptr, void* /*user_data*/)
        {
            xr_free(ptr);
        }
    );
    ImGui::SetCurrentContext(Device.GetImGuiContext());
}

void xrGameModule::finalize()
{
    xr_delete(UIStyles);
    StringTable().Destroy();
    CCC_DeregisterInput(); // XXX: Remove if possible

#ifdef DEBUG
    xr_delete(g_profiler);
#endif
}

IGame_Persistent* xrGameModule::create_persistent()
{
    object_factory(); // XXX: remove this call
    return xr_new<CGamePersistent>();
}

void xrGameModule::destroy_persistent(IGame_Persistent*& persistent)
{
    xr_delete(persistent);
}
