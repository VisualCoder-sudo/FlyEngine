// WinBegin.h - include before any Windows header that must coexist with raylib.
// Pair with WinEnd.h. No include guard on purpose: it only (re)defines macros.
//
// windows.h declares functions whose names collide with raylib's (CloseWindow,
// ShowCursor, Rectangle, LoadImage, DrawText, ...). Renaming the Windows
// declarations while the headers are parsed keeps both APIs usable.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #pragma warning(push)
  #pragma warning(disable: 4005)
  #undef CloseWindow
  #undef ShowCursor
  #undef Rectangle
  #undef LoadImage
  #undef DrawText
  #undef DrawTextA
  #undef DrawTextW
  #undef DrawTextEx
  #undef PlaySound
  #undef PlaySoundA
  #undef PlaySoundW
  #define CloseWindow  Win32_CloseWindow
  #define ShowCursor   Win32_ShowCursor
  #define Rectangle    Win32_Rectangle
  #define LoadImage    Win32_LoadImage
  #define DrawText     Win32_DrawText
  #define DrawTextA    Win32_DrawTextA
  #define DrawTextW    Win32_DrawTextW
  #define DrawTextEx   Win32_DrawTextEx
  #define PlaySound    Win32_PlaySound
  #define PlaySoundA   Win32_PlaySoundA
  #define PlaySoundW   Win32_PlaySoundW
#endif
