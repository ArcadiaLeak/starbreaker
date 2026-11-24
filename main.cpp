#include <windows.h>

// The window procedure handles messages sent to the window.
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
  case WM_DESTROY:
    // When the window is closed, post a quit message to end the application.
    PostQuitMessage(0);
    return 0;
  case WM_PAINT:
  {
    // Paint the background with the standard window color.
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
    EndPaint(hwnd, &ps);
    return 0;
  }
  default:
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

// The Unicode application entry point.
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow)
{
  const wchar_t CLASS_NAME[] = L"MinimalWindowClass";

  // 1. Register the window class.
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = CLASS_NAME;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);

  if (!RegisterClassExW(&wc))
  {
    MessageBoxW(NULL, L"Window Registration Failed!", L"Error", MB_ICONERROR);
    return 1;
  }

  // 2. Create the window.
  HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Minimal C++ GUI App (wWinMain)",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              500, 300, NULL, NULL, hInstance, NULL);

  if (!hwnd)
  {
    MessageBoxW(NULL, L"Window Creation Failed!", L"Error", MB_ICONERROR);
    return 1;
  }

  // 3. Show the window and run the message loop.
  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg = {};
  while (GetMessageW(&msg, NULL, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  return (int)msg.wParam;
}
