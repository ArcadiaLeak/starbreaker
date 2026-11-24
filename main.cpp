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

static std::wstring ToWide(std::string_view sv)
{
  if (sv.empty())
    return {};
  int outlen = MultiByteToWideChar(CP_UTF8, 0, sv.data(),
                                   static_cast<int>(sv.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(outlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()),
                      out.data(), outlen);
  return out;
}

int WINAPI wWinMain(HINSTANCE h_instance, HINSTANCE h_prev, PWSTR cmd_str,
                    int cmd_show)
{
  try
  {
    const wchar_t CLASS_NAME[] = L"Starbreaker_WindowClass";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = h_instance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (not RegisterClassExW(&wc))
      throw std::system_error{static_cast<int>(GetLastError()),
                              std::system_category(), "RegisterClassExW"};

    HWND window_hdl =
        CreateWindowExW(0, CLASS_NAME, L"Minimal C++ GUI App (wWinMain)",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500,
                        300, nullptr, nullptr, h_instance, nullptr);
    if (not window_hdl)
      throw std::system_error{static_cast<int>(GetLastError()),
                              std::system_category(), "CreateWindowExW"};
    ShowWindow(window_hdl, cmd_show);
    UpdateWindow(window_hdl);

    MSG msg{};
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
  }
  catch (const std::exception &e)
  {
    std::wstring msg_wide = ToWide(e.what());
    MessageBoxW(nullptr, msg_wide.c_str(), nullptr, MB_ICONERROR);
    return 1;
  }
}
