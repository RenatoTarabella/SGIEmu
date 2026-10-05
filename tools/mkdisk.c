/*
 * mkdisk: crea un'immagine di disco SCSI vuota (tutti zeri) per l'Indy.
 *
 *   mkdisk FILE MB
 *
 * Il disco non ha volume header: va partizionato da IRIX (fx) prima di
 * installarci il sistema. Il file e' sparse dove il file system lo permette.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mkdisk FILE MB   (e.g. mkdisk disks\\irix.img 2048)\n");
        return 1;
    }
    const char *path = argv[1];
    long long mb = atoll(argv[2]);
    if (mb < 16 || mb > 65536) { fprintf(stderr, "size: 16 to 65536 MB\n"); return 1; }
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); fprintf(stderr, "%s already exists: not overwritten\n", path); return 1; }
    long long bytes = mb << 20;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot create %s\n", path); return 1; }
    DWORD ret;
    DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);   /* se non riesce, pazienza */
    LARGE_INTEGER size;
    size.QuadPart = bytes;
    int ok = SetFilePointerEx(h, size, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    if (!ok) { DeleteFileA(path); fprintf(stderr, "cannot size %s (disk full?)\n", path); return 1; }
#else
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); return 1; }
    int ok = fseek(f, (long)(bytes - 1), SEEK_SET) == 0 && fputc(0, f) != EOF;
    if (fclose(f) || !ok) { remove(path); fprintf(stderr, "cannot size %s\n", path); return 1; }
#endif
    printf("%s: %lld MB, empty\n", path, mb);
    return 0;
}
