code

; System calls of a game module built as a QVM: trap n is address -1-n
; (see oe_public.h, keep in sync with oeGameImport_t).

equ trap_Print                      -1
equ trap_Error                      -2
equ trap_Milliseconds               -3
equ trap_VirtualClock               -4
equ trap_TimeOfDayMs                -5
equ trap_Cvar_VariableStringBuffer  -6
equ trap_Cvar_Set                   -7
equ trap_Argc                       -8
equ trap_Argv                       -9
equ trap_GameDataSize               -10
equ trap_GameDataRead               -11
equ trap_ResSize                    -12
equ trap_ResRead                    -13
equ trap_ResString                  -14
equ trap_FileOpen                   -15
equ trap_FileRead                   -16
equ trap_FileWrite                  -17
equ trap_FileSeek                   -18
equ trap_FileSize                   -19
equ trap_FileClose                  -20
equ trap_FileRemove                 -21
equ trap_FileExists                 -22
equ trap_FileLastError              -23
equ trap_ScreenUpdate               -24
equ trap_SndLoad                    -25
equ trap_SndPlay                    -26
equ trap_SndStop                    -27
equ trap_SndIsPlaying               -28
equ trap_SndSetVolume               -29
equ trap_Vibrate                    -30
equ trap_SetWindowTitle             -31
equ trap_PipeWrite                  -32
equ trap_HwTexture                  -33
equ trap_HwScene                    -34
equ trap_HwFreeView                 -35
equ trap_FreeMove                   -36
equ trap_Exec                       -37
equ trap_ResImage                   -38
equ trap_ExportFile                 -39
equ trap_DsFile                     -40
equ trap_DsMap                      -41
equ trap_DsSprites                  -42

; the C library functions the engine runs natively for the QVM
equ memset                          -101
equ memcpy                          -102
equ memmove                         -103
equ sqrt                            -104
equ sin                             -105
