#include <windows.h>
#include <ctime>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// --- Konfiguracja gry ---
const int BOARD_W = 10;
const int BOARD_H = 20;
const int CELL_SIZE = 24;

const UINT ID_TIMER = 1;
const UINT TIMER_INTERVAL = 600; // ms, tempo spadania

// --- Struktury gry ---
struct Piece {
    int x;       // pozycja w komórkach (kolumna)
    int y;       // pozycja w komórkach (wiersz)
    int shape;   // 0..6
    int rot;     // 0..3
};

// globalny stan gry
int board[BOARD_H][BOARD_W] = { 0 };
Piece currentPiece;
bool gameOver = false;
int score = 0;

// kolory dla figur (1..7)
COLORREF g_colors[8] = {
    RGB(0, 0, 0),         // 0 - puste
    RGB(0, 255, 255),     // 1 - I
    RGB(0, 0, 255),       // 2 - J
    RGB(255, 165, 0),     // 3 - L (pomarańczowy)
    RGB(255, 255, 0),     // 4 - O
    RGB(0, 255, 0),       // 5 - S
    RGB(160, 32, 240),    // 6 - T (fiolet)
    RGB(255, 0, 0)        // 7 - Z
};

HBRUSH g_brushes[8] = { 0 };

// definicje klocków w orientacji bazowej (rot = 0) w układzie 4x4
// współrzędne x,y w obrębie 4x4
struct Block { int x, y; };

Block baseShapes[7][4] = {
    // I
    { {0,1}, {1,1}, {2,1}, {3,1} },
    // J
    { {0,0}, {0,1}, {1,1}, {2,1} },
    // L
    { {2,0}, {0,1}, {1,1}, {2,1} },
    // O
    { {1,0}, {2,0}, {1,1}, {2,1} },
    // S
    { {1,0}, {2,0}, {0,1}, {1,1} },
    // T
    { {1,0}, {0,1}, {1,1}, {2,1} },
    // Z
    { {0,0}, {1,0}, {1,1}, {2,1} }
};

// Obrót punktu w macierzy 4x4 (0..3)
Block rotateBlock(const Block& b, int rot) {
    Block r = b;
    for (int i = 0; i < (rot & 3); ++i) {
        int x = r.x;
        int y = r.y;
        // obrót 90° CW: (x, y) -> (3 - y, x)
        r.x = 3 - y;
        r.y = x;
    }
    return r;
}

void getPieceBlocks(const Piece& p, Block out[4]) {
    for (int i = 0; i < 4; ++i) {
        Block b = rotateBlock(baseShapes[p.shape][i], p.rot);
        out[i].x = p.x + b.x;
        out[i].y = p.y + b.y;
    }
}

bool isCollision(const Piece& p) {
    Block blocks[4];
    getPieceBlocks(p, blocks);
    for (int i = 0; i < 4; ++i) {
        int x = blocks[i].x;
        int y = blocks[i].y;
        if (x < 0 || x >= BOARD_W || y < 0 || y >= BOARD_H)
            return true;
        if (board[y][x] != 0)
            return true;
    }
    return false;
}

void resetBoard() {
    for (int y = 0; y < BOARD_H; ++y)
        for (int x = 0; x < BOARD_W; ++x)
            board[y][x] = 0;
    score = 0;
    gameOver = false;
}

void spawnNewPiece() {
    currentPiece.shape = rand() % 7;
    currentPiece.rot = 0;
    currentPiece.x = BOARD_W / 2 - 2;
    currentPiece.y = 0;

    if (isCollision(currentPiece)) {
        gameOver = true;
    }
}

void lockPiece() {
    Block blocks[4];
    getPieceBlocks(currentPiece, blocks);
    for (int i = 0; i < 4; ++i) {
        int x = blocks[i].x;
        int y = blocks[i].y;
        if (y >= 0 && y < BOARD_H && x >= 0 && x < BOARD_W) {
            board[y][x] = currentPiece.shape + 1; // 1..7
        }
    }
}

void clearLines() {
    int lines = 0;
    for (int y = BOARD_H - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < BOARD_W; ++x) {
            if (board[y][x] == 0) {
                full = false;
                break;
            }
        }
        if (full) {
            // przesuń wszystko w dół
            for (int yy = y; yy > 0; --yy) {
                for (int x = 0; x < BOARD_W; ++x) {
                    board[yy][x] = board[yy - 1][x];
                }
            }
            for (int x = 0; x < BOARD_W; ++x) {
                board[0][x] = 0;
            }
            ++lines;
            ++y; // sprawdź jeszcze raz ten sam wiersz po przesunięciu
        }
    }
    // prosty system punktów: 100 za linię
    score += lines * 100;
}

void movePiece(int dx, int dy) {
    if (gameOver) return;
    Piece tmp = currentPiece;
    tmp.x += dx;
    tmp.y += dy;
    if (!isCollision(tmp)) {
        currentPiece = tmp;
    }
    else if (dy != 0) {
        // kolizja przy ruchu w dół -> blokujemy, kasujemy linie, generujemy nową figurę
        lockPiece();
        clearLines();
        spawnNewPiece();
    }
}

void rotatePiece() {
    if (gameOver) return;
    Piece tmp = currentPiece;
    tmp.rot = (tmp.rot + 1) & 3;
    if (!isCollision(tmp)) {
        currentPiece = tmp;
    }
}

// "Hard drop": zrzut na dół
void hardDrop() {
    if (gameOver) return;
    Piece tmp = currentPiece;
    while (!isCollision(tmp)) {
        currentPiece = tmp;
        tmp.y += 1;
    }
    lockPiece();
    clearLines();
    spawnNewPiece();
}

// --- Rysowanie ---
void drawBoard(HDC hdc, RECT clientRect) {
    int boardPxW = BOARD_W * CELL_SIZE;
    int boardPxH = BOARD_H * CELL_SIZE;

    // tło
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
    RECT r = { 0, 0, clientRect.right, clientRect.bottom };
    FillRect(hdc, &r, bg);
    DeleteObject(bg);

    // przesunięcie planszy, żeby było trochę marginesu
    int offsetX = 20;
    int offsetY = 20;

    // ramka planszy
    HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(200, 200, 200));
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    Rectangle(hdc,
              offsetX - 1, offsetY - 1,
              offsetX + boardPxW + 1, offsetY + boardPxH + 1);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    // rysuj komórki z planszy
    for (int y = 0; y < BOARD_H; ++y) {
        for (int x = 0; x < BOARD_W; ++x) {
            int v = board[y][x];
            if (v != 0) {
                HBRUSH b = g_brushes[v];
                RECT cell = {
                    offsetX + x * CELL_SIZE,
                    offsetY + y * CELL_SIZE,
                    offsetX + (x + 1) * CELL_SIZE,
                    offsetY + (y + 1) * CELL_SIZE
                };
                FillRect(hdc, &cell, b);
                // delikatna ramka
                FrameRect(hdc, &cell, (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
        }
    }

    // rysuj aktualny klocek
    if (!gameOver) {
        Block blocks[4];
        getPieceBlocks(currentPiece, blocks);
        HBRUSH b = g_brushes[currentPiece.shape + 1];
        for (int i = 0; i < 4; ++i) {
            int x = blocks[i].x;
            int y = blocks[i].y;
            if (y < 0) continue; // nad widocznym obszarem
            RECT cell = {
                offsetX + x * CELL_SIZE,
                offsetY + y * CELL_SIZE,
                offsetX + (x + 1) * CELL_SIZE,
                offsetY + (y + 1) * CELL_SIZE
            };
            FillRect(hdc, &cell, b);
            FrameRect(hdc, &cell, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
    }

    // tekst – punkty i komunikaty
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(220, 220, 220));

    TCHAR buf[128];
    wsprintf(buf, TEXT("Score: %d"), score);
    TextOut(hdc, offsetX + boardPxW + 20, offsetY, buf, lstrlen(buf));

    if (gameOver) {
        const TCHAR* msg = TEXT("GAME OVER - nacisnij Enter");
        TextOut(hdc, offsetX + boardPxW + 20, offsetY + 40, msg, lstrlen(msg));
    }
    else {
        const TCHAR* help =
            TEXT("Sterowanie:\n")
            TEXT("←/→  - ruch\n")
            TEXT("↓    - szybciej w dol\n")
            TEXT("↑    - obrot\n")
            TEXT("Spacja - hard drop");
        TextOut(hdc, offsetX + boardPxW + 20, offsetY + 40, help, lstrlen(help));
    }
}

// --- Okno / WinAPI ---

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // kolory jako pędzle
            for (int i = 0; i < 8; ++i) {
                g_brushes[i] = CreateSolidBrush(g_colors[i]);
            }
            srand((unsigned int)time(nullptr));
            resetBoard();
            spawnNewPiece();
            SetTimer(hwnd, ID_TIMER, TIMER_INTERVAL, nullptr);
            return 0;
        }
        case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        for (int i = 0; i < 8; ++i) {
            if (g_brushes[i]) DeleteObject(g_brushes[i]);
        }
        PostQuitMessage(0);
        return 0;

        case WM_TIMER:
        if (wParam == ID_TIMER) {
            if (!gameOver) {
                movePiece(0, 1);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

        case WM_KEYDOWN:
        if (gameOver) {
            if (wParam == VK_RETURN) {
                resetBoard();
                spawnNewPiece();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        switch (wParam) {
            case VK_LEFT:
            movePiece(-1, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
            case VK_RIGHT:
            movePiece(1, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
            case VK_DOWN:
            movePiece(0, 1);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
            case VK_UP:
            rotatePiece();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
            case VK_SPACE:
            hardDrop();
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
            default:
            break;
        }
        return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            // podwójne buforowanie
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(
                hdc, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            drawBoard(memDC, clientRect);

            BitBlt(hdc, 0, 0,
                   clientRect.right - clientRect.left,
                   clientRect.bottom - clientRect.top,
                   memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPSTR     lpCmdLine,
                     int       nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    const TCHAR CLASS_NAME[] = TEXT("TetrisWindowClass");

    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassEx(&wc)) {
        MessageBox(nullptr, TEXT("Nie moge zarejestrować klasy okna."),
                   TEXT("Błąd"), MB_ICONERROR | MB_OK);
        return 0;
    }

    int boardPxW = BOARD_W * CELL_SIZE + 200; // miejsce na panel boczny
    int boardPxH = BOARD_H * CELL_SIZE + 100;

    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        TEXT("Tetris - C++ / WinAPI"),
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME, // bez zmiany rozmiaru
        CW_USEDEFAULT, CW_USEDEFAULT,
        boardPxW, boardPxH,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        MessageBox(nullptr, TEXT("Nie moge utworzyć okna."),
                   TEXT("Błąd"), MB_ICONERROR | MB_OK);
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
