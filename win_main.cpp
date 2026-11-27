#include <stdexcept>
#include <system_error>
#include <windows.h>

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    FillRect(hdc, &ps.rcPaint, GetSysColorBrush(COLOR_WINDOW));
    EndPaint(hwnd, &ps);
    return 0;
  }
  default:
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
  }
}

int WINAPI wWinMain(HINSTANCE h_instance, HINSTANCE h_prev, PWSTR cmd_str,
                    int cmd_show)
{
  try
  {
    const char *CLASS_NAME = "Starbreaker_WindowClass";

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = h_instance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (not RegisterClassEx(&wc))
      throw std::system_error{static_cast<int>(GetLastError()),
                              std::system_category(), "RegisterClassEx"};

    HWND window_hdl =
        CreateWindowEx(0, CLASS_NAME, "Minimal C++ GUI App (wWinMain)",
                       WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500,
                       300, nullptr, nullptr, h_instance, nullptr);
    if (not window_hdl)
      throw std::system_error{static_cast<int>(GetLastError()),
                              std::system_category(), "CreateWindowEx"};
    ShowWindow(window_hdl, cmd_show);
    UpdateWindow(window_hdl);

    MSG msg{};
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
  }
  catch (const std::exception &e)
  {
    MessageBox(nullptr, e.what(), nullptr, MB_ICONERROR);
    return 1;
  }
}
