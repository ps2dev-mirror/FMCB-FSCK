#include <kernel.h>
#include <libmc.h>
#include <libpad.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <osd_config.h>
#include <timer.h>
#include <limits.h>

#include <libgs.h>

#include "main.h"
#include "system.h"
#include "pad.h"
#include "graphics.h"
#include "font.h"
#include "UI.h"
#include "menu.h"

extern struct UIDrawGlobal UIDrawGlobal;
extern GS_IMAGE BackgroundTexture;
extern GS_IMAGE PadLayoutTexture;

enum SCAN_RESULTS_SCREEN_ID{
	SCN_SCREEN_ID_TITLE	= 1,
	SCN_SCREEN_ID_RESULT,
	SCN_SCREEN_ID_LBL_ERRS_FOUND,
	SCN_SCREEN_ID_ERRS_FOUND,
	SCN_SCREEN_ID_LBL_ERRS_FIXED,
	SCN_SCREEN_ID_ERRS_FIXED,
	SCN_SCREEN_ID_LBL_SOME_ERRS_NOT_FIXED,
	SCN_SCREEN_ID_BTN_OK,
};

static struct UIMenuItem ScanResultsScreenItems[]={
	{MITEM_LABEL, SCN_SCREEN_ID_TITLE, 0, 0, 0, 0, 0, SYS_UI_LBL_SCAN_RESULTS},
	{MITEM_SEPERATOR},
	{MITEM_BREAK},

	{MITEM_STRING, SCN_SCREEN_ID_RESULT, MITEM_FLAG_READONLY}, {MITEM_BREAK}, {MITEM_BREAK},

	{MITEM_LABEL, SCN_SCREEN_ID_LBL_ERRS_FOUND, 0, 0, 0, 0, 0, SYS_UI_LBL_ERRORS_FOUND}, {MITEM_TAB}, {MITEM_VALUE, SCN_SCREEN_ID_ERRS_FOUND, MITEM_FLAG_READONLY}, {MITEM_BREAK},
	{MITEM_LABEL, SCN_SCREEN_ID_LBL_ERRS_FIXED, 0, 0, 0, 0, 0, SYS_UI_LBL_ERRORS_FIXED}, {MITEM_TAB}, {MITEM_VALUE, SCN_SCREEN_ID_ERRS_FIXED, MITEM_FLAG_READONLY}, {MITEM_BREAK},
	{MITEM_BREAK},

	{MITEM_LABEL, SCN_SCREEN_ID_LBL_SOME_ERRS_NOT_FIXED, 0, 0, 0, 0, 0, SYS_UI_LBL_SOME_ERRORS_NOT_FIXED}, {MITEM_BREAK},

	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},
	{MITEM_BREAK},

	{MITEM_BUTTON, SCN_SCREEN_ID_BTN_OK, MITEM_FLAG_POS_MID, 0, 16},

	{MITEM_TERMINATOR}
};

static struct UIMenu ScanResultsScreen = {NULL, NULL, ScanResultsScreenItems, {{BUTTON_TYPE_SYS_SELECT, SYS_UI_LBL_OK}, {-1, -1}}};

static void DrawMenuEntranceSlideInMenuAnimation(int SelectedOption)
{
	int i;
	GS_RGBAQ rgbaq;

	rgbaq.r = 0;
	rgbaq.g = 0;
	rgbaq.b = 0;
	rgbaq.q = 0;
	for(i=30; i>0; i--)
	{
		rgbaq.a = 0x80-(i*4);
		DrawSprite(&UIDrawGlobal,	0, 0,	
						UIDrawGlobal.width, UIDrawGlobal.height,
						0, rgbaq);
		SyncFlipFB(&UIDrawGlobal);
	}
}

static void DrawMenuExitAnimation(void)
{
	int i;
	GS_RGBAQ rgbaq;

	rgbaq.r = 0;
	rgbaq.g = 0;
	rgbaq.b = 0;
	rgbaq.q = 0;
	for(i=30; i>0; i--)
	{
		rgbaq.a = 0x80-(i*4);
		DrawSprite(&UIDrawGlobal,	0, 0,	
						UIDrawGlobal.width, UIDrawGlobal.height,
						0, rgbaq);
		SyncFlipFB(&UIDrawGlobal);
	}
}

void MainMenu(void)
{
	DrawMenuEntranceSlideInMenuAnimation(0);

	if(DisplayPromptMessage(SYS_UI_MSG_AUTO_SCAN_DISK_CFM, SYS_UI_LBL_OK, SYS_UI_LBL_CANCEL)==1)
		ScanDisk(0);

	DrawMenuExitAnimation();
}

void DisplayScanCompleteResults(unsigned int ErrorsFound, unsigned int ErrorsFixed)
{
	UISetString(&ScanResultsScreen, SCN_SCREEN_ID_RESULT, GetUIString(SYS_UI_MSG_SCAN_DISK_COMPLETED_OK));

	if(ErrorsFound)
	{
		UISetValue(&ScanResultsScreen, SCN_SCREEN_ID_ERRS_FOUND, ErrorsFound);
		UISetValue(&ScanResultsScreen, SCN_SCREEN_ID_ERRS_FIXED, ErrorsFixed);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_LBL_ERRS_FOUND, 1);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_ERRS_FOUND, 1);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_LBL_ERRS_FIXED, 1);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_ERRS_FIXED, 1);
	} else {
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_LBL_ERRS_FOUND, 0);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_ERRS_FOUND, 0);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_LBL_ERRS_FIXED, 0);
		UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_ERRS_FIXED, 0);
	}

	UISetVisible(&ScanResultsScreen, SCN_SCREEN_ID_LBL_SOME_ERRS_NOT_FIXED, (ErrorsFound != ErrorsFixed));

	UIExecMenu(&ScanResultsScreen, 0, NULL, NULL);
}

void RedrawLoadingScreen(unsigned int frame)
{
	int NumDots;
	GS_RGBAQ rgbaq;

	SyncFlipFB(&UIDrawGlobal);

	NumDots=frame%240/60;

	DrawBackground(&UIDrawGlobal, &BackgroundTexture);
	FontPrintf(&UIDrawGlobal, 10, 10, 0, 1.5f, GS_WHITE_FONT, "HDDChecker v"HDDC_VERSION);

	FontPrintf(&UIDrawGlobal, 420, 380, 0, 1.0f, GS_WHITE_FONT, "Loading");
	switch(NumDots)
	{
		case 1:
			FontPrintf(&UIDrawGlobal, 560, 380, 0, 1.0f, GS_WHITE_FONT, ".");
			break;
		case 2:
			FontPrintf(&UIDrawGlobal, 560, 380, 0, 1.0f, GS_WHITE_FONT, "..");
			break;
		case 3:
			FontPrintf(&UIDrawGlobal, 560, 380, 0, 1.0f, GS_WHITE_FONT, "...");
			break;
	}

	if(frame < 60)
	{	//Fade in
		rgbaq.r = 0;
		rgbaq.g = 0;
		rgbaq.b = 0;
		rgbaq.q = 0;
		rgbaq.a = 0x80-(frame*2);
		DrawSprite(&UIDrawGlobal,	0, 0,	
						UIDrawGlobal.width, UIDrawGlobal.height,
						0, rgbaq);
	}
}
