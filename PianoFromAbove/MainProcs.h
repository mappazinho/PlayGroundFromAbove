/*************************************************************************************************
*
* File: MainProcs.h
*
* Description: Defines the main GUI functions. Not C++ :/
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <string>
using namespace std;

// Message handlers for the main windows
LRESULT WINAPI WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
VOID SizeWindows( int iMainWidth, int iMainHeight );

// Render progress window (plain Win32 popup, never captured in the video)
VOID RequestCreateRenderProgressWindow();                 // main-thread create
VOID RequestDestroyRenderProgressWindow();                // main-thread destroy
VOID UpdateRenderProgressWindow( INT iPermille, const wchar_t *wText ); // thread-safe

LRESULT WINAPI GfxProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

VOID HandOffMsg( UINT msg, WPARAM wParam, LPARAM lParam );
VOID ShowControls( BOOL bShow );
VOID ShowKeyboard( BOOL bShow );
VOID SetOnTop( BOOL bOnTop );
VOID SetFullScreen( BOOL bFullScreen );
VOID SetZoomMove( BOOL bZoomMove );
VOID SetMute( BOOL bMute );
VOID SetSpeed( DOUBLE dSpeed );
VOID SetNSpeed( DOUBLE dSpeed );
VOID SetVolume( DOUBLE dVolume );
VOID SetPosition( INT iPosition );
VOID SetPlayable( BOOL bPlayable );
VOID SetPlayMode( INT ePlayMode );
VOID SetPlayPauseStop( BOOL bPlay, BOOL bPause, BOOL bStop );
BOOL PlayFile( const wstring &sFile, bool bCustomSettings = false );
VOID CheckActivity( BOOL bIsActive, POINT *ptNew = NULL, BOOL bToggleEnable = false );