@echo off
SET LIBPATH=C:\TC\LIB

REM [1] 부트 섹터
ml /c boot.asm
tlink /s /m boot,BOOT.EXE,BOOT.MAP

REM [2] 진입 스텁 및 C 모듈 컴파일
ml /c c0boot.asm
tcc -mt -v -y -c start.c
tcc -mt -v -y -c test\csm.c
tcc -mt -v -y -c test\kbd.c
tcc -mt -v -y -c test\speaker.c
tcc -mt -v -y -c test\timer.c
tcc -mt -v -y -c test\mem.c
tcc -mt -v -y -c test\disk.c
tcc -mt -v -y -c test\mouse.c
tcc -mt -v -y -c test\port.c
tcc -mt -v -y -c test\gfx.c
tcc -mt -v -y -c test\cpu.c
tcc -mt -v -y -c test\rtc.c

tlink /v /s /m c0boot start csm kbd speaker timer mem disk mouse port gfx cpu rtc,START.EXE,START.MAP,C:\TC\LIB\CS.LIB
tlink /t /x c0boot start csm kbd speaker timer mem disk mouse port gfx cpu rtc,START.COM,,C:\TC\LIB\CS.LIB