// Author: Leshev.
// Version app: 2.45. 


#define _WIN32_WINNT 0x0600
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <urlmon.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <filesystem>
#include <strsafe.h>
#include <fstream>
#include "resources.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

// Глобальные переменные
HINSTANCE hInst;
const wchar_t* WINDOW_CLASS = L"RealPack";
const wchar_t* WINDOW_TITLE = L"RealPack";

// Объявления функций
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void RunInstallation(HWND hWnd);
void CheckForUpdates(HWND hWnd);
DWORD WINAPI InstallationThread(LPVOID lpParam);
DWORD WINAPI UpdateCheckThread(LPVOID lpParam);

std::wstring GetTargetDirectory() {
    wchar_t userProfile[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, userProfile) != S_OK) {
        MessageBoxW(NULL, L"Не удалось получить путь к профилю пользователя", L"Ошибка", MB_ICONERROR);
        return L"";
    }
    
    std::wstring targetDir = std::wstring(userProfile) + L"\\AppData\\Roaming\\MCGL\\MinecraftLauncher2\\repository\\mclient\\plugins\\realpack";
    
    try {
        // Проверяем и создаем все необходимые папки в пути
        if (!fs::exists(targetDir)) {
            if (!fs::create_directories(targetDir)) {
                MessageBoxW(NULL, L"Не удалось создать целевую директорию", L"Ошибка", MB_ICONERROR);
                return L"";
            }
            // Устанавливаем атрибуты папки (скрытая, если нужно)
            SetFileAttributesW(targetDir.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        
        // Проверяем, что это действительно папка и доступна для записи
        if (!(GetFileAttributesW(targetDir.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
            MessageBoxW(NULL, L"Целевой путь не является директорией", L"Ошибка", MB_ICONERROR);
            return L"";
        }
        
        return targetDir;
    } catch (const std::exception& e) {
        std::wstring errorMsg = L"Ошибка при работе с файловой системой: ";
        errorMsg += std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(e.what());
        MessageBoxW(NULL, errorMsg.c_str(), L"Ошибка", MB_ICONERROR);
        return L"";
    }
}

HRESULT DownloadFile(HWND hWnd, const std::wstring& url, const std::wstring& localPath) {
    SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Установка связи с сервером...");
    
    DeleteFileW(localPath.c_str());
    HRESULT hr = URLDownloadToFileW(NULL, url.c_str(), localPath.c_str(), 0, NULL);
    
    if (SUCCEEDED(hr)) {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Загрузка...");
    } else {
        wchar_t msg[256];
        StringCchPrintfW(msg, 256, L"Ошибка загрузки: 0x%08X", hr);
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
    }
    return hr;
}

bool ExtractZipWithPowerShell(HWND hWnd, const std::wstring& zipPath, const std::wstring& extractPath) {
    SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Распаковка...");

    // Создаем команду PowerShell для распаковки
    std::wstring command = L"powershell -command \"Expand-Archive -Path '" + 
                          zipPath + L"' -DestinationPath '" + extractPath + L"' -Force\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // Скрываем окно PowerShell

    if (!CreateProcessW(NULL, (LPWSTR)command.c_str(), NULL, NULL, FALSE, 
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, 
                   (LPARAM)L"Ошибка запуска PowerShell.");
        return false;
    }

    // Ждем завершения процесса
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, 
                   (LPARAM)L"Ошибка при распаковке архива.");
        return false;
    }

    SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, 
               (LPARAM)L"Распаковка успешно завершена.");
    return true;
}

bool ReplaceFiles(HWND hWnd, const std::wstring& sourceDir, const std::wstring& targetDir) {
    try {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Замена файлов в целевой директории...");
        
        // Проверка прав доступа к целевой директории
        DWORD access = GetFileAttributesW(targetDir.c_str());
        if (access == INVALID_FILE_ATTRIBUTES || !(access & FILE_ATTRIBUTE_DIRECTORY)) {
            SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, 
                       (LPARAM)L"Ошибка: Невозможно получить доступ к целевой директории");
            return false;
        }

        // Сначала удаляем все существующие файлы в целевой директории
        if (fs::exists(targetDir)) {
            for (const auto& entry : fs::recursive_directory_iterator(targetDir)) {
                try {
                    if (entry.is_directory()) continue;
                    fs::remove(entry.path());
                } catch (const std::exception& e) {
                    wchar_t msg[256];
                    StringCchPrintfW(msg, 256, L"Предупреждение: Не удалось удалить старый файл %s: %S", 
                                    entry.path().filename().c_str(), e.what());
                    SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
                }
            }
        }

        // Затем копируем все новые файлы из источника в цель
        for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
            try {
                if (entry.is_directory()) continue;
                
                fs::path relativePath = fs::relative(entry.path(), sourceDir);
                fs::path targetPath = fs::path(targetDir) / relativePath;
                
                fs::create_directories(targetPath.parent_path());
                fs::copy_file(entry.path(), targetPath, fs::copy_options::overwrite_existing);
                
                wchar_t msg[256];
                StringCchPrintfW(msg, 256, L"Заменен: %s", relativePath.c_str());
                SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
            } catch (const std::exception& e) {
                wchar_t msg[256];
                StringCchPrintfW(msg, 256, L"Ошибка замены файла %s: %S", 
                                entry.path().filename().c_str(), e.what());
                SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
                return false;
            }
        }
        
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Замена файлов успешно завершена.");
        return true;
    } catch (const std::exception& e) {
        wchar_t msg[256];
        StringCchPrintfW(msg, 256, L"Ошибка замены файлов: %S", e.what());
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
        return false;
    }
}

DWORD WINAPI InstallationThread(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);

    std::wstring zipUrl = L"https://github.com/Leshev/realpack/raw/refs/heads/main/realpack.zip";
    std::wstring txtUrl = L"https://github.com/Leshev/realpack/raw/refs/heads/main/version.txt";
    std::wstring zipPath = std::wstring(tempPath) + L"realpack.zip";
    std::wstring extractPath = std::wstring(tempPath) + L"realpack_extracted\\";
    std::wstring targetDir = GetTargetDirectory();

    if (targetDir.empty()) {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Не удалось определить целевую директорию.");
        return 1;
    }

    if (FAILED(DownloadFile(hWnd, zipUrl, zipPath))) return 1;
    if (!ExtractZipWithPowerShell(hWnd, zipPath, extractPath)) return 1;
    if (!ReplaceFiles(hWnd, extractPath, targetDir)) return 1;

    fs::remove_all(extractPath);
    fs::remove(zipPath);
    
    // Сохранение информации о версии после успешной установки
    //std::wstring versionFilePath = targetDir + L"\\version.txt";
    //std::ofstream versionFile(versionFilePath.c_str());
   // if (versionFile.is_open()) {
   //     versionFile << ""; // Укажите актуальную версию пакета
       // versionFile.close();
     //   SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Версия сохранена");
   // } else {
     //   SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Не удалось сохранить информацию о версии");
   // }
    
    // Очищаем список сообщений перед выводом финального
    SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
    // Выводим финальное сообщение
    SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Установка успешно завершена!");
    
    EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
    EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
    
    return 0;
}

DWORD WINAPI UpdateCheckThread(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Проверка обновлений...");

    // Получаем текущую версию (если есть)
    std::wstring targetDir = GetTargetDirectory();
    std::wstring versionFilePath = targetDir + L"\\version.txt";
    
    DWORD currentVersion = 0;
    if (fs::exists(versionFilePath)) {
        std::ifstream versionFile(versionFilePath.c_str());
        if (versionFile.is_open()) {
            versionFile >> currentVersion;
            versionFile.close();
            
            wchar_t msg[256];
            StringCchPrintfW(msg, 256, L"Текущая версия: %d", currentVersion);
            SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
        }
    } else {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Отсутствует файл version.txt");
    }

    // Загружаем информацию о последней версии с сервера
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring versionUrl = L"https://github.com/Leshev/realpack/raw/refs/heads/main/version.txt";
    std::wstring remoteVersionPath = std::wstring(tempPath) + L"remote_version.txt";

    if (FAILED(DownloadFile(hWnd, versionUrl, remoteVersionPath))) {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Не удалось проверить обновления");
        EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
        EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
        return 1;
    }

    // Читаем версию с сервера
    DWORD remoteVersion = 0;
    std::ifstream remoteVersionFile(remoteVersionPath.c_str());
    if (remoteVersionFile.is_open()) {
        remoteVersionFile >> remoteVersion;
        remoteVersionFile.close();
        fs::remove(remoteVersionPath);
        
        wchar_t msg[256];
        StringCchPrintfW(msg, 256, L"Доступная версия: %d", remoteVersion);
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)msg);
    } else {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Не удалось прочитать версию с сервера");
        EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
        EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
        return 1;
    }

    if (remoteVersion > currentVersion) {
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Доступно обновление! Начинаю установку...");
        
        // Вызываем процедуру установки
        wchar_t zipUrl[MAX_PATH] = L"https://github.com/Leshev/realpack/raw/refs/heads/main/realpack.zip";
        wchar_t zipPath[MAX_PATH];
        PathCombineW(zipPath, tempPath, L"realpack.zip");
        
        wchar_t extractPath[MAX_PATH];
        PathCombineW(extractPath, tempPath, L"realpack_extracted");
        
        if (FAILED(DownloadFile(hWnd, zipUrl, zipPath))) {
            EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
            EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
            return 1;
        }
        
        if (!ExtractZipWithPowerShell(hWnd, zipPath, extractPath)) {
            EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
            EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
            return 1;
        }
        
        if (!ReplaceFiles(hWnd, extractPath, targetDir)) {
            EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
            EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
            return 1;
        }
        
        // Обновляем информацию о версии
        std::ofstream versionFile(versionFilePath.c_str());
        if (versionFile.is_open()) {
            versionFile << remoteVersion;
            versionFile.close();
        }
        
        // Очищаем список сообщений
        SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
        // Выводим финальное сообщение
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"Обновление успешно установлено!");
    } else {
        // Очищаем список сообщений если версия актуальна
        SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
        SendMessageW(GetDlgItem(hWnd, 1002), LB_ADDSTRING, 0, (LPARAM)L"У вас актуальная версия");
    }

    EnableWindow(GetDlgItem(hWnd, 1001), TRUE);
    EnableWindow(GetDlgItem(hWnd, 1003), TRUE);
    return 0;
}

void RunInstallation(HWND hWnd) {
    EnableWindow(GetDlgItem(hWnd, 1001), FALSE);
    EnableWindow(GetDlgItem(hWnd, 1003), FALSE);
    SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
    
    HANDLE hThread = CreateThread(NULL, 0, InstallationThread, hWnd, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

void CheckForUpdates(HWND hWnd) {
    EnableWindow(GetDlgItem(hWnd, 1001), FALSE);
    EnableWindow(GetDlgItem(hWnd, 1003), FALSE);
    SendMessageW(GetDlgItem(hWnd, 1002), LB_RESETCONTENT, 0, 0);
    
    HANDLE hThread = CreateThread(NULL, 0, UpdateCheckThread, hWnd, 0, NULL);
    if (hThread) CloseHandle(hThread);
}



int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;

    // Регистрация класса окна
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszClassName = WINDOW_CLASS;
	wcex.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    
    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"Ошибка регистрации окна!", L"Ошибка!", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    // Создание окна
    HWND hWnd = CreateWindowExW(
        0,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 150,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        MessageBoxW(NULL, L"Ошибка создания окна!", L"Ошибка!", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Цикл обработки сообщений
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        // Создание кнопки установки
        CreateWindowW(
            L"BUTTON", L"Установить",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            10, 10, 150, 30,
            hWnd, (HMENU)1001, hInst, NULL);

        // Создание кнопки проверки
        CreateWindowW(
            L"BUTTON", L"Обновить",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            170, 10, 150, 30,
            hWnd, (HMENU)1003, hInst, NULL);

        // Создание списка логов
        CreateWindowW(
            L"LISTBOX", NULL,
            WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | ES_AUTOVSCROLL,
            10, 50, 460, 300,
            hWnd, (HMENU)1002, hInst, NULL);
        break;
    }
	case WM_COMMAND: {
    if (LOWORD(wParam) == 1001) { // Кнопка установки
        RunInstallation(hWnd);
    }
    else if (LOWORD(wParam) == 1003) { // Кнопка обновления
        // Используем ту же функцию, что и для установки, но с проверкой версий
        CheckForUpdates(hWnd);
    }
    break;
}
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

