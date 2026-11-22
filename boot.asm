.8086
_TEXT SEGMENT BYTE PUBLIC 'CODE'
ASSUME CS:_TEXT, DS:_TEXT, ES:_TEXT, SS:_TEXT
ORG 7C00h

entry:
    jmp short start
    nop

    ; 0x03 ~ 0x59: FAT32 BPB 영역 (87 bytes)
    db 87 dup (0)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 7BF0h               ; 7BF4h~7BFFh를 32비트 스크래치 변수로 사용
    sti
    cld
    db 0EAh                     ; JMP FAR 0000h:norm_cs
    dw offset norm_cs, 0000h

norm_cs:
    mov ds:[7C40h], dl          ; 부트 드라이브 번호(DL) 저장

    ; -------------------------------------------------------
    ; 16비트 연산으로 data_lba 계산 -> ds:[7BFCh] (Low), ds:[7BFEh] (High)
    ; data_lba = BPB_RsvdSecCnt + (BPB_NumFATs * BPB_FATSz32)
    ; -------------------------------------------------------
    mov al, byte ptr ds:[7C10h] ; BPB_NumFATs (보통 2)
    cbw
    mov cx, ax                  ; CX = NumFATs
    mov ax, word ptr ds:[7C24h] ; FATSz32 Low
    mul cx                      ; DX:AX = FATSz32_Lo * NumFATs
    mov bx, ax
    mov di, dx                  ; DI:BX에 임시 보관
    mov ax, word ptr ds:[7C26h] ; FATSz32 High
    mul cx
    add di, ax                  ; DI:BX = NumFATs * FATSz32

    add bx, word ptr ds:[7C0Eh] ; + BPB_RsvdSecCnt
    adc di, 0
    add bx, word ptr ds:[7C1Ch] ; + BPB_HiddSec Low (파티션 시작 LBA 오프셋)
    adc di, word ptr ds:[7C1Eh] ; + BPB_HiddSec High
    mov ds:[7BFCh], bx          ; data_lba Low
    mov ds:[7BFEh], di          ; data_lba High

    ; 루트 디렉터리 시작 클러스터 로드 (DX:AX)
    mov ax, word ptr ds:[7C2Ch]
    mov dx, word ptr ds:[7C2Eh]

next_dir_clus:
    and dh, 0Fh
    cmp dx, 0FFFh
    jb dir_clus_ok
    cmp ax, 0FFF8h
    jae err_halt                ; EOC 도달 시 파일 없음
dir_clus_ok:
    mov ds:[7BF4h], ax          ; 현재 디렉터리 클러스터 백업
    mov ds:[7BF6h], dx
    call clus_to_lba            ; DX:AX = 시작 LBA, CX = SecPerClus

dir_sec_loop:
    push cx
    push dx
    push ax
    mov bx, 0700h               ; 0700:0000 (0x7000)에 디렉터리 섹터 로드
    call read_sec
    mov es, bx
    xor di, di
    mov cx, 16                  ; 섹터당 엔트리 16개

dir_ent_loop:
    cmp byte ptr es:[di], 0
    jz err_halt
    push cx
    push di
    mov si, offset filename
    mov cx, 11
    repe cmpsb
    pop di
    pop cx
    jz found_file
    add di, 20h
    loop dir_ent_loop

    pop ax
    pop dx
    add ax, 1                   ; 다음 섹터 LBA (32비트 +1)
    adc dx, 0
    pop cx
    loop dir_sec_loop

    mov ax, ds:[7BF4h]          ; 현재 디렉터리 클러스터 복원
    mov dx, ds:[7BF6h]
    call next_clus              ; 다음 클러스터 조회 -> DX:AX
    jmp short next_dir_clus

found_file:
    add sp, 6                   ; 스택에 남은 AX, DX, CX 정리
    mov dx, es:[di+14h]         ; DIR_FstClusHI
    mov ax, es:[di+1Ah]         ; DIR_FstClusLO
    mov bx, 0800h               ; 적재 세그먼트 = 0800h (물리 0x8000)

load_loop:
    and dh, 0Fh
    cmp dx, 0FFFh
    jb load_clus_ok
    cmp ax, 0FFF8h
    jae load_done               ; EOC 도달 시 적재 완료
load_clus_ok:
    mov ds:[7BF4h], ax
    mov ds:[7BF6h], dx
    call clus_to_lba            ; DX:AX = LBA, CX = SecPerClus

load_sec_loop:
    call read_sec               ; BX:0000h에 1섹터 읽기
    add ax, 1
    adc dx, 0
    add bx, 20h                 ; 세그먼트 +0x20 (512바이트 전진)
    loop load_sec_loop

    mov ax, ds:[7BF4h]
    mov dx, ds:[7BF6h]
    call next_clus
    jmp short load_loop

load_done:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov dl, ds:[7C40h]
    db 0EAh                     ; JMP FAR 07F0h:0100h (물리 0x8000 = .COM 진입점)
    dw 0100h, 07F0h

; ---------------------------------------------------------------------------
; clus_to_lba: DX:AX = 클러스터 번호 -> DX:AX = 시작 LBA, CX = SecPerClus
; ---------------------------------------------------------------------------
clus_to_lba proc near
    sub ax, 2
    sbb dx, 0
    mov cl, byte ptr ds:[7C0Dh] ; BPB_SecPerClus (2의 거듭제곱: 1,2,4,8...)
    xor ch, ch
    push cx                     ; 반환할 CX(SecPerClus) 보존
    jmp short shift_chk
shift_loop:
    shl ax, 1
    rcl dx, 1
shift_chk:
    shr cl, 1
    jnz shift_loop              ; SecPerClus만큼 32비트 좌측 시프트

    add ax, ds:[7BFCh]          ; + data_lba Low
    adc dx, ds:[7BFEh]          ; + data_lba High
    pop cx
    ret
clus_to_lba endp

; ---------------------------------------------------------------------------
; read_sec: DX:AX = LBA, BX = 목적지 세그먼트 (레지스터 전부 보존)
; ---------------------------------------------------------------------------
read_sec proc near
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    xor cx, cx
    push cx                     ; DAP[14]: LBA [48..63] = 0
    push cx                     ; DAP[12]: LBA [32..47] = 0
    push dx                     ; DAP[10]: LBA [16..31]
    push ax                     ; DAP[8] : LBA [0..15]
    push bx                     ; DAP[6] : Buffer Segment
    push cx                     ; DAP[4] : Buffer Offset = 0
    inc cx
    push cx                     ; DAP[2] : Sector Count = 1
    mov cl, 10h
    push cx                     ; DAP[0] : Packet Size = 16

    mov si, sp
    mov ah, 42h
    mov dl, ds:[7C40h]
    int 13h
    jc err_halt
    add sp, 10h

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
read_sec endp

; ---------------------------------------------------------------------------
; next_clus: DX:AX = 현재 클러스터 -> DX:AX = 다음 클러스터 (BX 보존)
; ---------------------------------------------------------------------------
next_clus proc near
    push bx                     ; 적재 세그먼트(BX) 보존!
    mov di, ax
    and di, 7Fh
    shl di, 1
    shl di, 1

    shl ax, 1
    rcl dx, 1
    mov al, ah
    mov ah, dl
    mov dl, dh
    xor dh, dh

    add ax, word ptr ds:[7C0Eh] ; + BPB_RsvdSecCnt
    adc dx, 0
    add ax, word ptr ds:[7C1Ch] ; + BPB_HiddSec Low
    adc dx, word ptr ds:[7C1Eh] ; + BPB_HiddSec High
    mov bx, 0600h
    call read_sec
    mov es, bx
    mov ax, es:[di]
    mov dx, es:[di+2]
    pop bx                      ; 적재 세그먼트(BX) 복원!
    ret
next_clus endp

err_halt:
    xor ax, ax
    mov ds, ax
    mov si, offset err_msg
err_print:
    lodsb
    or al, al
    jz short halt_loop
    mov ah, 0Eh
    int 10h
    jmp short err_print
halt_loop:
    hlt
    jmp short halt_loop

err_msg  db "ERR", 0
filename db "START   COM"

    org 7DFEh
    dw 0AA55h

_TEXT ENDS
END entry