// fuckds.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <boost/crc.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <set>
#include <vector>

// Compact some stuff so it can all be sent by a pointer to get_process_window
struct process_window_helper
{
	// Set this before calling get_process_window as the process to match
	DWORD pid;
	// get_process_window will set this pointing to a visible window belonging to the process
	HWND hwnd;
};

BOOL CALLBACK get_process_window(HWND hwnd, LPARAM lParam)
{
	// Find all windows belonging to the process
	DWORD pid;
	GetWindowThreadProcessId(hwnd, &pid);
	if (((struct process_window_helper*)lParam)->pid == pid) {
		// Any given process may have many invisible top level windows
		// DMMViewer has about 4, but should only have one visible (the main window)
		if (IsWindowVisible(hwnd)) {
			((struct process_window_helper*)lParam)->hwnd = hwnd;
			return FALSE;
		}
	}

	return TRUE;
}

int _tmain(int argc, const char* argv[])
{
	static const int VIEWER_BUTTON_PARENT_CODE = 0xE805;
	static const int VIEWER_BUTTON_FIRST_CODE = 0x8023;
	static const int VIEWER_BUTTON_NEXT_CODE = 0x8020;
	static const int VIEWER_BUTTON_ZOOM_CODE = 0x800A;
	static const int VIEWER_AREA_PARENT_CODE = 0xE900;
	static const int VIEWER_AREA_MAIN_CODE = 0xE900;

	if (argc < 2)
		return -1;

	const DWORD viewer_process_id = atoi(argv[1]);

	printf("Try to open process... %d\n",viewer_process_id);

	// Open the process with permission to read memory and process info
	HANDLE viewer_process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, viewer_process_id);

	printf("Process handle %d\n", viewer_process);

	if (!viewer_process)
		return -1;

	struct process_window_helper pwh;

	pwh.pid = viewer_process_id;
	pwh.hwnd = (HWND)0xdeaddead;

	printf("Try to find window...\n");

	EnumWindows(&get_process_window, (LPARAM)&pwh);

	printf("Window hwnd: %d\n",pwh.hwnd);

	// Find all the subwindow by control ID
	HWND viewer_window_main = pwh.hwnd;
	HWND viewer_button_parent = GetDlgItem(viewer_window_main, VIEWER_BUTTON_PARENT_CODE);
	HWND viewer_button_first = GetDlgItem(viewer_button_parent, VIEWER_BUTTON_FIRST_CODE);
	HWND viewer_button_next = GetDlgItem(viewer_button_parent, VIEWER_BUTTON_NEXT_CODE);
	HWND viewer_button_zoom = GetDlgItem(viewer_button_parent, VIEWER_BUTTON_ZOOM_CODE);
	HWND viewer_area_parent = GetDlgItem(viewer_window_main, VIEWER_AREA_PARENT_CODE);
	HWND viewer_area_main = GetDlgItem(viewer_area_parent, VIEWER_AREA_MAIN_CODE);

	// Store original window position and state
	// Window will be shrunken to ensure window size is smaller than the image
	// This is necessary so that the immage in memory will not be padded
	WINDOWPLACEMENT original_window_placement;
	original_window_placement.length = sizeof(original_window_placement);
	GetWindowPlacement(viewer_window_main, &original_window_placement);
	WINDOWPLACEMENT temp_window_placement;
	temp_window_placement = original_window_placement;
	temp_window_placement.showCmd = SW_SHOW;
	SetWindowPlacement(viewer_window_main, &temp_window_placement);

	// Set zoom level to 100%
	int original_zoom_selection = ComboBox_GetCurSel(viewer_button_zoom);

	ComboBox_SelectString(viewer_button_zoom, -1, "100%");

	// Programatically selecting a ComboBox option will not send a selection change message to the parent, so do that manually
	SendMessage(viewer_button_parent, WM_COMMAND, MAKEWPARAM(VIEWER_BUTTON_ZOOM_CODE, CBN_SELCHANGE), (LPARAM)viewer_button_zoom);

	RECT original_window_rect;
	GetWindowRect(viewer_window_main, &original_window_rect);

	SetWindowPos(viewer_window_main, 0, 0, 0, 256, 256, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOACTIVATE);

	// Send a virtual click to the first page button
	// SendMessage(viewer_button_first, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
	// SendMessage(viewer_button_first, WM_LBUTTONUP, MK_LBUTTON, MAKELPARAM(1, 1));

	// It seems it takes some time for messages to propagate to other buttons, so wait until the next button is enabled
	// This will prevent the bug where if the Viewer was opened to the last page, it would only export the first image
	if (GetWindowLong(viewer_button_next, GWL_STYLE) & WS_DISABLED)
	{
		Sleep(1000);
	}

	std::vector<char> image;

	std::set<boost::crc_32_type::value_type> checksums;

	// Continue until the next button becomes disabled, indicating the last image
	for (size_t pages = 0;; pages++)
	{
		// As a hack, the dimensions of the images are determined by
		// looking at the size of the scroll area
		// This requires that the image area be smaller than the image
		SCROLLINFO sih, siv;

		sih.cbSize = sizeof(sih);
		sih.fMask = SIF_ALL;
		siv.cbSize = sizeof(siv);
		siv.fMask = SIF_ALL;

		GetScrollInfo(viewer_area_main, SB_HORZ, &sih);
		GetScrollInfo(viewer_area_main, SB_VERT, &siv);

		int w = sih.nMax - sih.nMin + 1;
		int h = siv.nMax - siv.nMin + 1;

		image.resize((((w * 32 + 31) & ~31) >> 3) * h);

		BITMAPFILEHEADER bmf = { 0 };
		bmf.bfType = 0x4d42;
		bmf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + image.size();
		bmf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

		BITMAPINFOHEADER bmi = { 0 };
		bmi.biSize = sizeof(BITMAPINFOHEADER);
		bmi.biWidth = w;
		bmi.biHeight = -h;
		bmi.biPlanes = 1;
		bmi.biBitCount = 32;
		bmi.biSizeImage = image.size();

		for (LPVOID address = 0;;)
		{
			MEMORY_BASIC_INFORMATION mbi;

			while (VirtualQueryEx(viewer_process, address, &mbi, sizeof(mbi))) {
				if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= image.size()) {
					if (!ReadProcessMemory(viewer_process, mbi.BaseAddress, &image[0], image.size(), NULL)) {
						printf("RPM failed\n");
						continue;
					}
					printf("RPM success\n");
					boost::crc_32_type crc32;
					crc32.process_bytes(&image[0], image.size());
					if (!checksums.insert(crc32.checksum()).second) {
						printf("Invalid checksum\n");
						continue;
					}
					printf("Check passed! \n");
				}
				address = (char*)mbi.BaseAddress + mbi.RegionSize;
			}

			char filename[FILENAME_MAX];

			_snprintf(filename, FILENAME_MAX, "%03u_%p.bmp", pages, mbi.BaseAddress);

			FILE *fp = fopen(filename, "wb");

			if (!fp) {
				printf("Unable to open local file to write\n");
				continue;
			}

			fwrite(&bmf, sizeof(bmf), 1, fp);
			fwrite(&bmi, sizeof(bmi), 1, fp);
			fwrite(&image[0], image.size(), 1, fp);

			fclose(fp);

			printf("Image dump ok, switch page.");
			break;
		}

		// Once image memory is ours, 'click' to the next image
		SendMessage(viewer_button_next, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
		SendMessage(viewer_button_next, WM_LBUTTONUP, MK_LBUTTON, MAKELPARAM(1, 1));

		// If the next button is disabled, we reached the end; grab one last image and finish
		if (GetWindowLong(viewer_button_next, GWL_STYLE) & WS_DISABLED)
			break;
	}

	// Restore window size and state
	SetWindowPos(viewer_window_main, 0, 0, 0, original_window_rect.right - original_window_rect.left, original_window_rect.top - original_window_rect.bottom, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
	SetWindowPlacement(viewer_window_main, &original_window_placement);

	// Restore zoom level
	ComboBox_SetCurSel(viewer_button_zoom, original_zoom_selection);
	SendMessage(viewer_button_parent, WM_COMMAND, MAKEWPARAM(VIEWER_BUTTON_ZOOM_CODE, CBN_SELCHANGE), (LPARAM)viewer_button_zoom);

	CloseHandle(viewer_process);

	return 0;
}