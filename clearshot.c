/*
 * This file is a part of Clearshot, which has been placed under the MIT/Expat
 * license.
 *
 * Copyright (c) 2013, James Ross-Gowan.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#define UNICODE
#define NTDDI_VERSION 0x06000000
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include "winextra.h"
#include <shlobj.h>
#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include <time.h>
#include <wchar.h>
#include <zlib.h>
#include <png.h>

extern IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)

static void Fatal(wchar_t* message)
{
	MessageBoxW(NULL, message, L"Clearshot", MB_ICONERROR | MB_SYSTEMMODAL);
	ExitProcess(1);
}

static void* xmalloc(size_t size)
{
	void* ptr = malloc(size);

	if (!ptr)
		Fatal(L"Out of memory.");

	return ptr;
}

static int ShowSaveDialog(wchar_t* name, size_t nameLength)
{
	wchar_t* defaultPath = xmalloc(MAX_PATH * sizeof(wchar_t));

	OPENFILENAMEW ofn = {
		.lStructSize  = sizeof(OPENFILENAMEW),
		.lpstrFilter  = L"PNG image\0*.png\0",
		.nFilterIndex = 1,
		.lpstrFile    = name,
		.nMaxFile     = nameLength,
		.Flags        = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
	};

	/* This may seem like an archaic way of getting the default pictures
	   directory since it doesn't understand Windows 7 libraries, however
	   GetSaveFileName will open the library anyway if given CSIDL_MYPICTURES
	   and doing it this way is backwards compatible with Windows Vista */
	if (SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, defaultPath) == S_OK)
		ofn.lpstrInitialDir = defaultPath;

	if (!GetSaveFileNameW(&ofn))
	{
		free(defaultPath);
		return 0;
	}

	free(defaultPath);
	return 1;
}

static FILE* OpenFileWrite(wchar_t* name)
{
	/* Every Windows program should have a variant of this function to replace
	   fopen. That is, one that supports Unicode and FILE_SHARE_DELETE. */
	HANDLE winHandle = CreateFileW(
		name,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_DELETE,
		NULL,
		CREATE_ALWAYS,
		0, NULL);
	if (!winHandle)
		return NULL;

	int crtHandle = _open_osfhandle((intptr_t)winHandle, 0);
	if (crtHandle == -1)
	{
		CloseHandle(winHandle);
		return NULL;
	}

	FILE* result = _fdopen(crtHandle, "wb");
	if (!result)
	{
		_close(crtHandle);
		return NULL;
	}

	return result;
}

static int WritePNG(FILE* file, uint8_t* buffer, int width, int height)
{
	size_t pitch = width * 4;
	png_structp png_ptr;
	png_infop info_ptr;

	if (!(png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL))) goto png_error;
	if (!(info_ptr = png_create_info_struct(png_ptr))) goto info_error;

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		png_destroy_write_struct(&png_ptr, &info_ptr);
		goto png_error;
	}

	png_init_io(png_ptr, file);
	png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);
	png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png_ptr, info_ptr);

	for (int r = height * pitch - pitch; r >= 0; r -= pitch)
		png_write_row(png_ptr, buffer + r);

	png_write_end(png_ptr, info_ptr);
	png_destroy_write_struct(&png_ptr, &info_ptr);

	return 1;
info_error:
	png_destroy_write_struct(&png_ptr, NULL);
png_error:
	return 0;
}

static void GetBits(uint8_t* buffer, int width, int height, HBITMAP bitmap, HDC context)
{
	const BITMAPINFOHEADER header = {
		.biSize     = sizeof(BITMAPINFOHEADER),
		.biWidth    = width,
		.biHeight   = height,
		.biPlanes   = 1,
		.biBitCount = 32,
	};

	GetDIBits(context, bitmap, 0, height, buffer, (BITMAPINFO*)&header, DIB_RGB_COLORS);
}

static void ProcessAlpha(uint8_t* buffer, const uint8_t* blackBuffer, size_t size)
{
	int r, g, b, a;

	for (size_t i = 0; i < size; i += 4)
	{
		/* With two reference points (the colour of the image on a light
		   background and the colour of the image on a dark background,)
		   it's possible to reverse the Porter-Duff over operator and
		   reconstruct the alpha channel of the windows in the screenshot */
		b = buffer[i];
		g = buffer[i + 1];
		r = buffer[i + 2];
		a = ((blackBuffer[i] - b + 255) +
			(blackBuffer[i + 1] - g + 255) +
			(blackBuffer[i + 2] - r + 255)) / 3;

		if (a)
		{
			a = a > 255 ? 255 : a;
			b = (b + a - 255) * 255 / a;
			g = (g + a - 255) * 255 / a;
			r = (r + a - 255) * 255 / a;

			buffer[i] = r < 0 ? 0 : r > 255 ? 255 : r;
			buffer[i + 1] = g < 0 ? 0 : g > 255 ? 255 : g;
			buffer[i + 2] = b < 0 ? 0 : b > 255 ? 255 : b;
			buffer[i + 3] = a;
		}
		/* If the pixel is completely transparent, it's impossible to get any
		   original colour information out of it, so just set it to black */
		else
			*(uint32_t*)(buffer + i) = 0;
	}
}

static LRESULT CALLBACK ShieldWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	static int left, top, width, height;
	static HDC whiteCtx, blackCtx;
	static HBITMAP white, black;
	static uint8_t* buffer;

	switch (message)
	{
		case WM_CREATE:
		{
			CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
			HDC desktop = GetDC(NULL);

			buffer = cs->lpCreateParams;
			left = cs->x;
			top = cs->y;
			width = cs->cx;
			height = cs->cy;

			/* These bitmaps will be used to store the light and dark
			   screenshots */
			whiteCtx = CreateCompatibleDC(desktop);
			white = CreateCompatibleBitmap(desktop, width, height);
			blackCtx = CreateCompatibleDC(desktop);
			black = CreateCompatibleBitmap(desktop, width, height);
			ReleaseDC(NULL, desktop);

			if (!whiteCtx || !white || !blackCtx || !black)
				return -1;

			SetTimer(window, 101, 0, NULL);

			return 0;
		}
		case WM_TIMER:
		{
			HDC desktop = GetDC(NULL);

			KillTimer(window, 101);
			DwmFlush();

			/* The shield window is created with a white background, so take
			   the first screenshot */
			SelectObject(whiteCtx, white);
			BitBlt(whiteCtx, 0, 0, width, height, desktop, left, top, SRCCOPY | CAPTUREBLT);

			/* Change the window background to black and update it */
			SetClassLongPtr(window, GCLP_HBRBACKGROUND, (long)GetStockObject(BLACK_BRUSH));
			InvalidateRect(window, NULL, TRUE);
			UpdateWindow(window);
			DwmFlush();

			/* Now take the second screenshot */
			SelectObject(blackCtx, black);
			BitBlt(blackCtx, 0, 0, width, height, desktop, left, top, SRCCOPY | CAPTUREBLT);

			ReleaseDC(NULL, desktop);
			DestroyWindow(window);

			return 0;
		}
		case WM_DESTROY:
		{
			size_t pitch = width * 4,
			       size = height * pitch;
			uint8_t* blackBuf = xmalloc(size);

			GetBits(buffer, width, height, white, whiteCtx);
			DeleteDC(whiteCtx);
			DeleteObject(white);

			GetBits(blackBuf, width, height, black, blackCtx);
			DeleteDC(blackCtx);
			DeleteObject(black);

			/* Get an image with an alpha channel from both bitmaps */
			ProcessAlpha(buffer, blackBuf, size);
			free(blackBuf);

			/* Exit this modal message loop so the program can continue */
			PostQuitMessage(0);
			return 0;
		}
	}

	return DefWindowProc(window, message, wParam, lParam);
}

static ATOM RegisterShieldClass()
{
	static ATOM atom = 0;

	if (!atom)
	{
		const WNDCLASSEX classex = {
			.cbSize        = sizeof(WNDCLASSEX),
			.style         = CS_NOCLOSE,
			.lpfnWndProc   = ShieldWndProc,
			.hInstance     = HINST_THISCOMPONENT,
			.hbrBackground = GetStockObject(WHITE_BRUSH),
			.lpszClassName = L"ClearshotShield",
		};

		atom = RegisterClassExW(&classex);
	}

	return atom;
}

static void ShootArea(int left, int top, int width, int height, uint8_t* buffer)
{
	HWND window = CreateWindowExW(
		WS_EX_NOACTIVATE,
		(wchar_t*)(intptr_t)RegisterShieldClass(),
		L"Clearshot",
		WS_DISABLED | WS_POPUP,
		left, top, width, height,
		NULL, NULL, HINST_THISCOMPONENT, buffer);

	if (!window)
		Fatal(L"Couldn't create window.");

	/* Move the shield underneath all other windows */
	SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW | SWP_NOACTIVATE);

	/* Start the modal message loop */
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}

static void GenerateFileName(wchar_t* buffer, size_t length)
{
	/* Generate the default filename from the current date */
	__time64_t t = _time64(NULL);
	struct tm* ltime = _localtime64(&t);
	wcsftime(buffer, length, L"screenshot_%Y-%m-%d_%H-%M-%S.png", ltime);
	free(ltime);
}

static void SavePNG(uint8_t* buffer, int width, int height)
{
	wchar_t* fileName = xmalloc(32768 * sizeof(wchar_t));

	GenerateFileName(fileName, 32768);

	if (ShowSaveDialog(fileName, 32768))
	{
		FILE* file = OpenFileWrite(fileName);

		if (!file)
			Fatal(L"Couldn't write file.");

		if (!WritePNG(file, buffer, width, height))
			Fatal(L"Couldn't write PNG.");

		fclose(file);
	}

	free(fileName);
}

static void ShootAndSave(int left, int top, int width, int height)
{
	uint8_t* buffer = xmalloc(width * height * 4);

	ShootArea(left, top, width, height, buffer);
	SavePNG(buffer, width, height);

	free(buffer);
}

static int ShowDialog(int* delayMode)
{
	const static TASKDIALOG_BUTTON buttons[] = {
		{ 100, L"&Start taking screenshots" },
		{ 101, L"E&xit" },
	};

	const static TASKDIALOG_BUTTON radioButtons[] = {
		{ 102, L"Wait for &delay\n"
		"Take a screenshot five seconds after this dialog disappears, then exit." },
		{ 103, L"Wait for &key press\n"
		"Hide this dialog and wait for Ctrl+PrtScr to be pressed." },
	};

	const static TASKDIALOGCONFIG dialog = {
		.cbSize = sizeof(TASKDIALOGCONFIG),
		.hInstance = HINST_THISCOMPONENT,
		.dwFlags = TDF_SIZE_TO_CONTENT,
		.pszWindowTitle = L"Clearshot",
		.pszMainIcon = MAKEINTRESOURCE(101),
		.pszContent = L"Choose when the screenshot will be taken:",

		.cButtons = 2,
		.pButtons = buttons,
		.nDefaultButton = 100,

		.cRadioButtons = 2,
		.pRadioButtons = radioButtons,
		.nDefaultRadioButton = 102,
	};

	int result, option;

	TaskDialogIndirect(&dialog, &result, &option, NULL);

	*delayMode = (option == 102);
	return (result == 100);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prevInstance, LPSTR cmdLine, int cmdShow)
{
	SetProcessDPIAware();

	int delay,
	    left = GetSystemMetrics(SM_XVIRTUALSCREEN),
	    top = GetSystemMetrics(SM_YVIRTUALSCREEN),
	    width = GetSystemMetrics(SM_CXVIRTUALSCREEN),
	    height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	if (!ShowDialog(&delay))
		return 0;

	ShootAndSave(left, top, width, height);

	return 0;
}
