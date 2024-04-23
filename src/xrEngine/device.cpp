#include "stdafx.h"

#include "Render.h"

#include "xrCore/FS_impl.h"
#include "xrCore/Threading/TaskManager.hpp"
#include "xrScriptEngine/ScriptExporter.hpp"

#include "xrSASH.h"
#include "XR_IOConsole.h"
#include "xr_input.h"

#include "IGame_Level.h"
#include "IGame_Persistent.h"

#include <SDL.h>

ENGINE_API CRenderDevice Device;
ENGINE_API CLoadScreenRenderer load_screen_renderer;

ENGINE_API bool g_bRendering = false;

int ps_fps_limit = 501;
int ps_fps_limit_in_menu = 60;

bool g_bLoaded = false;
ref_light precache_light = 0;

using namespace xray;

bool CRenderDevice::RenderBegin()
{
    if (GEnv.isDedicatedServer)
        return true;

    switch (GEnv.Render->GetDeviceState())
    {
    case DeviceState::Normal: break;
    case DeviceState::Lost:
        // If the device was lost, do not render until we get it back
        Sleep(33);
        return false;

    case DeviceState::NeedReset:
        // Check if the device is ready to be reset
        Reset();
        return false;

    default: R_ASSERT(0);
    }
    GEnv.Render->Begin();
    FPU::m24r();
    g_bRendering = true;

    return true;
}

void CRenderDevice::Clear() { GEnv.Render->Clear(); }

namespace
{
void CheckPrivilegySlowdown()
{
#ifndef MASTER_GOLD
    const auto slowdownthread = []
    {
        for (;;)
        {
            if (Device.GetStats().fFPS < 30)
                Sleep(1);
            if (Device.mt_bMustExit.load(std::memory_order_acquire) || !pSettings || !Console || !pInput)
                return;
        }
    };

    if (strstr(Core.Params, "-slowdown"))
        Threading::SpawnThread("slowdown", slowdownthread);
    if (strstr(Core.Params, "-slowdown2x"))
    {
        Threading::SpawnThread("slowdown", slowdownthread);
        Threading::SpawnThread("slowdown", slowdownthread);
    }
#endif
}
}

void CRenderDevice::RenderEnd(void)
{
    if (GEnv.isDedicatedServer)
        return;

    if (dwPrecacheFrame)
    {
        GEnv.Sound->set_master_volume(0.f);
        dwPrecacheFrame--;
        if (!dwPrecacheFrame)
        {
            GEnv.Render->updateGamma();
            if (precache_light)
            {
                precache_light->set_active(false);
                precache_light.destroy();
            }
            GEnv.Sound->set_master_volume(1.f);
            GEnv.Render->ResourcesDestroyNecessaryTextures();
            Memory.mem_compact();
            Msg("* MEMORY USAGE: %d K", Memory.mem_usage() / 1024);
            Msg("* End of synchronization A[%d] R[%d]", b_is_Active, b_is_Ready);
            FIND_CHUNK_COUNTER_FLUSH();
            CheckPrivilegySlowdown();
            if (g_pGamePersistent->GameType() == 1 && !psDeviceFlags.test(rsAlwaysActive)) // haCk
            {
                const Uint32 flags = SDL_GetWindowFlags(m_sdlWnd);
                if ((flags & SDL_WINDOW_INPUT_FOCUS) == 0)
                    Pause(true, true, true, "application start");
            }
        }
    }
    g_bRendering = false;
    // end scene
    // Present goes here, so call OA Frame end.
    if (g_SASH.IsBenchmarkRunning())
        g_SASH.DisplayFrame(fTimeGlobal);

    GEnv.Render->End();

    vCameraPositionSaved = vCameraPosition;
    vCameraDirectionSaved = vCameraDirection;
    vCameraTopSaved = vCameraTop;
    vCameraRightSaved = vCameraRight;

    mFullTransformSaved = mFullTransform;
    mViewSaved = mView;
    mProjectSaved = mProject;
}

void CRenderDevice::PreCache(u32 amount, bool wait_user_input)
{
    if (GEnv.isDedicatedServer)
        amount = 0;
    else if (GEnv.Render->GetForceGPU_REF())
        amount = 0;

    dwPrecacheFrame = dwPrecacheTotal = amount;
    if (amount && !precache_light && g_pGameLevel && g_loading_events.empty())
    {
        precache_light = GEnv.Render->light_create();
        precache_light->set_shadow(false);
        precache_light->set_position(vCameraPosition);
        precache_light->set_color(255, 255, 255);
        precache_light->set_range(5.0f);
        precache_light->set_active(true);
    }
    if (amount && !load_screen_renderer.IsActive())
    {
        load_screen_renderer.Start(wait_user_input);
    }
}

void CRenderDevice::CalcFrameStats()
{
    stats.RenderTotal.FrameEnd();
    do
    {
        // calc FPS & TPS
        if (fTimeDeltaReal <= EPS_S)
            break;
        const float fps = 1.f / fTimeDeltaReal;
        // if (Engine.External.tune_enabled) vtune.update (fps);
        constexpr float fOne = 0.3f;
        constexpr float fInv = 1.0f - fOne;
        stats.fFPS = fInv * stats.fFPS + fOne * fps;
        if (stats.RenderTotal.result > EPS_S)
        {
            const u32 renderedPolys = GEnv.Render->GetCacheStatPolys();
            stats.fTPS = fInv * stats.fTPS + fOne * float(renderedPolys) / (stats.RenderTotal.result * 1000.f);
            stats.fRFPS = fInv * stats.fRFPS + fOne * 1000.f / stats.RenderTotal.result;
        }
    } while (false);
    stats.RenderTotal.FrameStart();
}

int g_svDedicateServerUpdateReate = 100;

ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

bool CRenderDevice::BeforeFrame()
{
    if (!b_is_Ready)
    {
        Sleep(100);
        return false;
    }

    if (psDeviceFlags.test(rsStatistic))
        g_bEnableStatGather = true; // XXX: why not use either rsStatistic or g_bEnableStatGather?
    else
        g_bEnableStatGather = false;

    if (!g_loading_events.empty())
    {
        if (g_loading_events.front()())
            g_loading_events.pop_front();
        g_pGamePersistent->LoadDraw();
        return false;
    }

    if (!dwPrecacheFrame && !g_SASH.IsBenchmarkRunning() && g_bLoaded)
        g_SASH.StartBenchmark();

    return true;
}

void CRenderDevice::BeforeRender()
{
    // Precache
    if (dwPrecacheFrame)
    {
        const float factor = float(dwPrecacheFrame) / float(dwPrecacheTotal);
        const float angle = PI_MUL_2 * factor;
        vCameraDirection.set(_sin(angle), 0, _cos(angle));
        vCameraDirection.normalize();
        vCameraTop.set(0, 1, 0);
        vCameraRight.crossproduct(vCameraTop, vCameraDirection);
        mView.build_camera_dir(vCameraPosition, vCameraDirection, vCameraTop);
    }

    // Matrices
    mInvView.invert(mView);
    mFullTransform.mul(mProject, mView);
    mInvFullTransform.invert_44(mFullTransform);
    GEnv.Render->BeforeRender();
    GEnv.Render->SetCacheXform(mView, mProject);
}

static void UpdateViewports()
{
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void CRenderDevice::DoRender()
{
    if (GEnv.isDedicatedServer)
        return;

    CStatTimer renderTotalReal;
    renderTotalReal.FrameStart();
    renderTotalReal.Begin();
    if (b_is_Active && RenderBegin())
    {
        // all rendering is done here
        seqRender.Process();

        CalcFrameStats();
        Statistic->Show();

        ImGui::Render();
        m_imgui_render->Render(ImGui::GetDrawData());
        UpdateViewports();

        RenderEnd(); // Present goes here
    }
    else
    {
        UpdateViewports();
    }
    renderTotalReal.End();
    renderTotalReal.FrameEnd();
    stats.RenderTotal.accum = renderTotalReal.accum;
}

void CRenderDevice::SecondaryThreadProc()
{
    TaskScheduler->RegisterThisThreadAsWorker();
    while (!mt_bMustExit.load(std::memory_order_acquire))
    {
        if (executeSecondaryTasks.load(std::memory_order_acquire))
        {
            for (u32 pit = 0; pit < seqParallel.size(); pit++)
                seqParallel[pit]();
            seqParallel.clear();
            seqFrameMT.Process();
            executeSecondaryTasks.store(false, std::memory_order_relaxed);
            secondaryTasksExecuted.store(true, std::memory_order_release);
        }
        TaskScheduler->ExecuteOneTask();
    }
    TaskScheduler->UnregisterThisThreadAsWorker();
    secondaryThreadFinished.store(true, std::memory_order_release);
}

void CRenderDevice::ProcessFrame()
{
    if (!BeforeFrame())
        return;

    const u64 frameStartTime = TimerGlobal.GetElapsed_ms();

    FrameMove();

    BeforeRender();

    executeSecondaryTasks.store(true, std::memory_order_release);

    DoRender();

    const u64 frameEndTime = TimerGlobal.GetElapsed_ms();
    const u64 frameTime = frameEndTime - frameStartTime;

    u32 updateDelta = 1000 / ps_fps_limit;

    if (GEnv.isDedicatedServer)
        updateDelta = 1000 / g_svDedicateServerUpdateReate;

    else if (Paused() || g_pGameLevel == nullptr)
        updateDelta = 1000 / ps_fps_limit_in_menu;

    if (frameTime < updateDelta)
        Sleep(updateDelta - frameTime);

    while (!secondaryTasksExecuted.load(std::memory_order_acquire))
        TaskScheduler->ExecuteOneTask();

    secondaryTasksExecuted.store(false, std::memory_order_relaxed);

    if (!b_is_Active)
        Sleep(1);
}

void CRenderDevice::ProcessEvent(const SDL_Event& event)
{
    switch (event.type)
    {
#if SDL_VERSION_ATLEAST(2, 0, 9)
    case SDL_DISPLAYEVENT:
    {
        switch (event.display.type)
        {
        case SDL_DISPLAYEVENT_ORIENTATION:
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_DISPLAYEVENT_CONNECTED:
        case SDL_DISPLAYEVENT_DISCONNECTED:
#endif
            CleanupVideoModes();
            FillVideoModes();
#if SDL_VERSION_ATLEAST(2, 0, 14)
            if (event.display.display == psDeviceMode.Monitor && event.display.type != SDL_DISPLAYEVENT_CONNECTED)
#else
            if (event.display.display == psDeviceMode.Monitor)
#endif
                Reset();
            else
                UpdateWindowProps();
            break;
        } // switch (event.display.type)
        break;
    }
#endif
    case SDL_WINDOWEVENT:
    {
        const auto window = SDL_GetWindowFromID(event.window.windowID);
        if (!window)
            break;
        ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(window);
        if (!viewport)
            break;

        switch (event.window.event)
        {
        case SDL_WINDOWEVENT_MOVED:
        {
            if (window == m_sdlWnd)
            {
                UpdateWindowRects();
#if !SDL_VERSION_ATLEAST(2, 0, 18) // without SDL_WINDOWEVENT_DISPLAY_CHANGED, let's detect monitor change ourselves
                const int display = SDL_GetWindowDisplayIndex(window);
                if (display != -1)
                    psDeviceMode.Monitor = display;
#endif
            }
            if (viewport)
                viewport->PlatformRequestMove = true;
            break;
        }

#if SDL_VERSION_ATLEAST(2, 0, 18)
        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
            psDeviceMode.Monitor = event.window.data1;
            break;
#endif

        case SDL_WINDOWEVENT_RESIZED:
            if (viewport)
                viewport->PlatformRequestResize = true;
            break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
            if (psDeviceMode.WindowStyle != rsFullscreen)
            {
                if (static_cast<int>(psDeviceMode.Width) == event.window.data1 &&
                    static_cast<int>(psDeviceMode.Height) == event.window.data2)
                    break; // we don't need to reset device if resolution wasn't really changed

                psDeviceMode.Width = event.window.data1;
                psDeviceMode.Height = event.window.data2;

                Reset();
            }
            else
                UpdateWindowRects();

            break;
        }

        case SDL_WINDOWEVENT_CLOSE:
        {
            if (viewport)
                viewport->PlatformRequestClose = true;

            if (window == m_sdlWnd)
            {
                Engine.Event.Defer("KERNEL:disconnect");
                Engine.Event.Defer("KERNEL:quit");
            }
            break;
        }
        } // switch (event.window.event)
    }
    } // switch (event.type)

    editor().ProcessEvent(event);
}

void CRenderDevice::Run()
{
    g_bLoaded = false;
    Log("Starting engine...");

    // Startup timers and calculate timer delta
    dwTimeGlobal = 0;
    Timer_MM_Delta = 0;
    {
        const u32 time_mm = CPU::GetTicks();
        while (CPU::GetTicks() == time_mm)
            ; // wait for next tick
        const u32 time_system = CPU::GetTicks();
        const u32 time_local = TimerAsync();
        Timer_MM_Delta = time_system - time_local;
    }

    // Pre start
    seqAppStart.Process();

    SDL_HideWindow(m_sdlWnd); // workaround for SDL bug
    UpdateWindowProps();
    SDL_ShowWindow(m_sdlWnd);
    SDL_RaiseWindow(m_sdlWnd);
    if (GEnv.isDedicatedServer || strstr(Core.Params, "-center_screen"))
        SDL_SetWindowPosition(m_sdlWnd, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void CRenderDevice::Shutdown()
{
    // Stop Balance-Thread
    mt_bMustExit.store(true, std::memory_order_release);
    while (!secondaryThreadFinished.load(std::memory_order_acquire))
        SDL_PumpEvents();

    seqAppEnd.Process();
}

u32 app_inactive_time = 0;
u32 app_inactive_time_start = 0;

void CRenderDevice::FrameMove()
{
    dwFrame++;
    Core.dwFrame = dwFrame;
    dwTimeContinual = TimerMM.GetElapsed_ms() - app_inactive_time;

    fTimeDeltaReal = Timer.GetElapsed_sec();
    if (!_valid(fTimeDeltaReal))
        fTimeDeltaReal = EPS_S + EPS_S;
    Timer.Start(); // previous frame

    if (psDeviceFlags.test(rsConstantFPS))
    {
        // 20ms = 50fps
        // fTimeDelta = 0.020f;
        // fTimeGlobal += 0.020f;
        // dwTimeDelta = 20;
        // dwTimeGlobal += 20;
        // 33ms = 30fps
        fTimeDelta = 0.033f;
        fTimeGlobal += 0.033f;
        dwTimeDelta = 33;
        dwTimeGlobal += 33;
    }
    else
    {
        if (Paused())
            fTimeDelta = 0.0f;
        else
        {
            fTimeDelta = 0.1f * fTimeDelta + 0.9f * fTimeDeltaReal; // smooth random system activity - worst case ~7% error
            clamp(fTimeDelta, EPS_S + EPS_S, .1f); // limit to 10fps minimum
        }
        fTimeGlobal = TimerGlobal.GetElapsed_sec();
        const u32 _old_global = dwTimeGlobal;
        dwTimeGlobal = TimerGlobal.GetElapsed_ms();
        dwTimeDelta = dwTimeGlobal - _old_global;
    }
    ImGui::GetIO().DeltaTime = fTimeDeltaReal;

    m_imgui_render->Frame();
    ImGui::NewFrame();

    // Frame move
    stats.EngineTotal.FrameStart();
    stats.EngineTotal.Begin();
    // TODO: HACK to test loading screen.
    // if(!g_bLoaded)

    seqFrame.Process();

    g_bLoaded = true;
    // else
    // seqFrame.Process(rp_Frame);
    stats.EngineTotal.End();
    stats.EngineTotal.FrameEnd();

    ImGui::EndFrame();
}

ENGINE_API bool bShowPauseString = true;

void CRenderDevice::Pause(bool bOn, bool bTimer, bool bSound, [[maybe_unused]] pcstr reason)
{
    static int snd_emitters_ = -1;
    if (g_bBenchmark || GEnv.isDedicatedServer)
        return;

    if (bOn)
    {
        if (!Paused())
        {
            if (editor_mode())
                bShowPauseString = false;
#ifdef DEBUG
            else if (xr_strcmp(reason, "li_pause_key_no_clip") == 0)
                bShowPauseString = false;
#endif
            else
                bShowPauseString = true;
        }
        if (bTimer && (!g_pGamePersistent || g_pGamePersistent->CanBePaused()))
        {
            g_pauseMngr().Pause(true);
#ifdef DEBUG
            if (xr_strcmp(reason, "li_pause_key_no_clip") == 0)
                TimerGlobal.Pause(false);
#endif
        }
        if (bSound && GEnv.Sound)
            snd_emitters_ = GEnv.Sound->pause_emitters(true);
    }
    else
    {
        if (bTimer && g_pauseMngr().Paused())
        {
            fTimeDelta = EPS_S + EPS_S;
            g_pauseMngr().Pause(false);
        }
        if (bSound)
        {
            if (snd_emitters_ > 0) // avoid crash
                snd_emitters_ = GEnv.Sound->pause_emitters(false);
            else
            {
#ifdef DEBUG
                Log("GEnv.Sound->pause_emitters underflow");
#endif
            }
        }
    }
}

bool CRenderDevice::Paused() { return g_pauseMngr().Paused(); }

void CRenderDevice::OnWindowActivate(SDL_Window* window, bool activated)
{
    if (editor().GetState() == editor::ide::visible_state::full)
    {
        if (window != m_sdlWnd)
        {
            if (activated)
                editor().OnAppActivate();
            else
                editor().OnAppDeactivate();
        }
        return;
    }

    if (!GEnv.isDedicatedServer && activated)
        pInput->GrabInput(true);
    else
        pInput->GrabInput(false);

    b_is_Active = activated || psDeviceFlags.test(rsAlwaysActive);

    if (activated != b_is_InFocus)
    {
        b_is_InFocus = activated;
        if (b_is_InFocus)
        {
            TaskScheduler->Pause(false);
            seqAppActivate.Process();
            app_inactive_time += TimerMM.GetElapsed_ms() - app_inactive_time_start;
        }
        else
        {
            app_inactive_time_start = TimerMM.GetElapsed_ms();
            seqAppDeactivate.Process();
            TaskScheduler->Pause(true);
        }
    }
}

void CRenderDevice::time_factor(const float time_factor)
{
    Timer.time_factor(time_factor);
    TimerGlobal.time_factor(time_factor);
    if (!strstr(Core.Params, "-sound_constant_speed"))
        psSoundTimeFactor = time_factor; //--#SM+#--
}

void CRenderDevice::AddSeqFrame(pureFrame* f, bool mt)
{
    if (mt)
        seqFrameMT.Add(f, REG_PRIORITY_HIGH);
    else
        seqFrame.Add(f, REG_PRIORITY_LOW);
}

void CRenderDevice::RemoveSeqFrame(pureFrame* f)
{
    seqFrameMT.Remove(f);
    seqFrame.Remove(f);
}

CRenderDevice* get_device() { return &Device; }
u32 script_time_global() { return Device.dwTimeGlobal; }
u32 script_time_global_async() { return Device.TimerAsync_MMT(); }

SCRIPT_EXPORT(Device, (),
{
    using namespace luabind;
    module(luaState)
    [
        def("time_global", &script_time_global),
        def("time_global_async", &script_time_global_async),
        def("device", &get_device),
        def("is_enough_address_space_available", &is_enough_address_space_available)
    ];
});

void CLoadScreenRenderer::Start(bool b_user_input)
{
    Device.seqFrame.Add(this, 0);
    Device.seqRender.Add(this, 0);
    m_registered = true;
    m_need_user_input = b_user_input;

    g_pGamePersistent->ShowLoadingScreen(true);
    g_pGamePersistent->LoadBegin();
}

void CLoadScreenRenderer::Stop()
{
    if (!m_registered)
        return;
    Device.seqFrame.Remove(this);
    Device.seqRender.Remove(this);

    m_registered = false;
    m_need_user_input = false;

    g_pGamePersistent->ShowLoadingScreen(false);
    g_pGamePersistent->LoadEnd();
}

void CLoadScreenRenderer::OnFrame()
{
    g_pGamePersistent->LoadStage(false);
}

void CLoadScreenRenderer::OnRender()
{
    g_pGamePersistent->load_draw_internal();
}
