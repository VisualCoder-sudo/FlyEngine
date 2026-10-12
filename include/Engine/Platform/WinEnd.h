// WinEnd.h - see WinBegin.h.
#ifdef _WIN32
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
  #pragma warning(pop)
  // raylib.h (already included) renames these two to avoid a link clash with user32.
  #ifdef RL_WIN32_SYMBOL_RENAME
    #define CloseWindow rl_CloseWindow
    #define ShowCursor  rl_ShowCursor
  #endif
#endif
