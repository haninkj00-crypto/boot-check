.8086

_TEXT SEGMENT BYTE PUBLIC 'CODE'
_TEXT ENDS

_DATA SEGMENT WORD PUBLIC 'DATA'
_DATA ENDS

_BSS  SEGMENT WORD PUBLIC 'BSS'
bss_start label byte
_BSS  ENDS

DGROUP GROUP _TEXT, _DATA, _BSS

_TEXT SEGMENT BYTE PUBLIC 'CODE'
ASSUME CS:DGROUP, DS:DGROUP, ES:DGROUP, SS:DGROUP
ORG 0100h

EXTRN _main:NEAR
PUBLIC DGROUP@
PUBLIC _DGROUP@

start:
    cli
    mov ax, 07F0h               ; 물리 0x8000 = 07F0:0100h
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0FFFEh
    mov word ptr ds:[DGROUP@], ax

    ; initialize BSS to zero (bss_start .. 0FF00h, 스택 여유 256B 보존)
    xor ax, ax
    mov di, offset DGROUP:bss_start
    mov cx, 0FF00h
    sub cx, di
    cld
    rep stosb

    sti
    db 0EAh                     ; JMP FAR 07F0h:0116h
    dw offset relocated, 07F0h

relocated:
    call _main

halt_forever:
    hlt
    jmp short halt_forever

; Borland/Turbo C 컴파일러가 near -> far 변환 시 참조하는 데이터 세그먼트 변수
DGROUP@   label word
_DGROUP@ dw 07F0h

_TEXT ENDS
END start
