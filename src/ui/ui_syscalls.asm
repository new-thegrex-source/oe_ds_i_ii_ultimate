code

; System calls of the ui module built as a QVM: trap n is address -1-n
; (see ui_public.h, keep in sync with uiImport_t).

equ trap_Print                      -1
equ trap_Error                      -2
equ trap_Milliseconds               -3
equ trap_Cvar_Set                   -4
equ trap_Cvar_VariableStringBuffer  -5
equ trap_Argc                       -6
equ trap_Argv                       -7
equ trap_Cmd_ExecuteText            -8
equ trap_FS_ReadFile                -9
equ trap_R_RegisterShaderNoMip      -10
equ trap_R_SetColor                 -11
equ trap_R_DrawStretchPic           -12
equ trap_R_ConsoleFont              -13
equ trap_Key_GetCatcher             -14
equ trap_Key_SetCatcher             -15
equ trap_R_White                    -16
equ trap_R_RegisterFont             -17

; the C library functions the engine runs natively for the QVM
equ memset                          -101
equ memcpy                          -102
equ memmove                         -103
equ sqrt                            -104
equ sin                             -105
