# boot-check
Test your PC's compatibilities.

---

## How to build

**1.** The source codes are all in UTF-8, and maybe LF-only. So, convert to CP949(since comments are in Korean)

**2.** Move the entire tree to DOS. Just make a floppy image or use any method you prefer.

**3.** Run `build.bat` on DOS. It assumes MASM 6a, Turbo C++ 3 and Turbo Link 5 installed.

**4.** Run `make-image.sh`, which makes a 64MB FAT32 partition. I recommend not to modify or delete the default `RESULT.TXT`.

**5.** Run `process-sector.py`, which process MBR and places the program properly.

**6.** Boot and test. The `RESULT.TXT` can store about 5 records. The oldest line will be deleted if there isn't enough space.

---

## How to use

I'll write ASAP. But you can easily understand reading source codes and trying executing.

---

## How it works

I'll wirte ASAP. Read the source codes or ask to LLMs, please.
