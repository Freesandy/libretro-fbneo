// CPS - Run
#include "cps.h"

// Inputs:
UINT8 CpsReset = 0;
UINT8 Cpi01A = 0, Cpi01C = 0, Cpi01E = 0;

static INT32 nInterrupt;
static INT32 nIrqLine, nIrqCycles;
static bool bEnableAutoIrq50, bEnableAutoIrq52;				// Trigger an interrupt every 32 scanlines

static const INT32 nFirstLine = 0x0C;							// The first scanline of the display
static const INT32 nVBlank = 0x0106 - 0x0C;					// The scanline at which the vblank interrupt is triggered

static INT32 nCpsCyclesExtra;

INT32 CpsDrawSpritesInReverse = 0;

INT32 nIrqLine50, nIrqLine52;

static INT32 DrvReset()
{
	// Reset machine
	if (Cps == 2 || PangEEP || Cps1Qs == 1) EEPROMReset();

	SekOpen(0);
	SekReset();
	SekClose();

	if (!Cps1Pic) {
		ZetOpen(0);
		ZetReset();
		ZetClose();
	}

	if (Cps == 2) {
		// Disable beam-synchronized interrupts
		*((UINT16*)(CpsReg + 0x4E)) = 0x0200;
		*((UINT16*)(CpsReg + 0x50)) = 0x0106;
		*((UINT16*)(CpsReg + 0x52)) = 0x0106;
	}

	SekOpen(0);
	CpsMapObjectBanks(0);
	SekClose();

	nCpsCyclesExtra = 0;

	if (Cps == 2 || Cps1Qs == 1) {			// Sound init (QSound)
		QsndReset();
	}
	
	HiscoreReset();

	return 0;
}

static const eeprom_interface qsound_eeprom_interface =
{
	7,		/* address bits */
	8,		/* data bits */
	"0110",	/*  read command */
	"0101",	/* write command */
	"0111",	/* erase command */
	0,
	0,
	0,
	0
};

static const eeprom_interface cps2_eeprom_interface =
{
	6,		/* address bits */
	16,		/* data bits */
	"0110",	/*  read command */
	"0101",	/* write command */
	"0111",	/* erase command */
	0,
	0,
	0,
	0
};

INT32 CpsRunInit()
{
	nLagObjectPalettes = 0;

	if (Cps == 2) nLagObjectPalettes = 1;

	SekInit(0, 0x68000);					// Allocate 68000
	
	if (CpsMemInit()) {						// Memory init
		return 1;
	}
	
	if (Cps == 2 || PangEEP) {
		EEPROMInit(&cps2_eeprom_interface);
	} else {
		if (Cps1Qs == 1) {
			EEPROMInit(&qsound_eeprom_interface);
		}
	}

	CpsRwInit();							// Registers setup

	if (CpsPalInit()) {						// Palette init
		return 1;
	}
	if (CpsObjInit()) {						// Sprite init
		return 1;
	}

	if ((Cps & 1) && Cps1Qs == 0 && Cps1Pic == 0) {			// Sound init (MSM6295 + YM2151)
		if (PsndInit()) {
			return 1;
		}
	}

	if (Cps == 2 || Cps1Qs == 1) {			// Sound init (QSound)
		if (QsndInit()) {
			return 1;
		}
	}

	if (Cps == 2 || PangEEP || Cps1Qs == 1) EEPROMReset();
	DrvReset();

	//Init Draw Function
	DrawFnInit();
	
	pBurnDrvPalette = CpsPal;

	return 0;
}

INT32 CpsRunExit()
{
	if (Cps == 2 || PangEEP || Cps1Qs == 1) EEPROMExit();

	// Sound exit
	if (Cps == 2 || Cps1Qs == 1) QsndExit();
	if (Cps != 2 && Cps1Qs == 0 && !Cps1Pic) PsndExit();

	// Graphics exit
	CpsObjExit();
	CpsPalExit();

	// Sprite Masking exit
	ZBuf = NULL;

	// Memory exit
	CpsRwExit();
	CpsMemExit();

	SekExit();

	return 0;
}

// nStart = 0-3, nCount=1-4
inline static void GetPalette(INT32 nStart, INT32 nCount)
{
	// Update Palette (Ghouls points to the wrong place on boot up I think)
	INT32 nPal = (*((UINT16*)(CpsReg + 0x0A)) << 8) & 0xFFF800;

	UINT8* Find = CpsFindGfxRam(nPal, 0x1000);
	if (Find) {
		memcpy(CpsSavePal + (nStart << 10), Find + (nStart << 10), nCount << 10);
	}
}

static void GetStarPalette()
{
	INT32 nPal = (*((UINT16*)(CpsReg + 0x0A)) << 8) & 0xFFF800;

	UINT8* Find = CpsFindGfxRam(nPal, 256);
	if (Find) {
		memcpy(CpsSavePal + 4096, Find + 4096, 256);
		memcpy(CpsSavePal + 5120, Find + 5120, 256);
	}
}

inline static void CopyCpsReg(INT32 i)
{
	memcpy(CpsSaveReg[i], CpsReg, 0x0100);
}

inline static void CopyCpsFrg(INT32 i)
{
	memcpy(CpsSaveFrg[i], CpsFrg, 0x0010);
}

// Schedule a beam-synchronized interrupt
static void ScheduleIRQ()
{
	INT32 nLine = 0x0106;

	if (nIrqLine50 <= nLine) {
		nLine = nIrqLine50;
	}
	if (nIrqLine52 < nLine) {
		nLine = nIrqLine52;
	}

	if (nLine < 0x0106) {
		nIrqLine = nLine;
		nIrqCycles = (nLine * nCpsCycles / 0x0106) + 1;
	} else {
		nIrqCycles = nCpsCycles + 1;
	}

	return;
}

// Execute a beam-synchronised interrupt and schedule the next one
static void DoIRQ()
{
	// 0x4E - bit 9 = 1: Beam Synchronized interrupts disabled
	// 0x50 - Beam synchronized interrupt #1 occurs at raster line.
	// 0x52 - Beam synchronized interrupt #2 occurs at raster line.

	// Trigger IRQ and copy registers.
	if (nIrqLine >= nFirstLine) {

		nInterrupt++;
		nRasterline[nInterrupt] = nIrqLine - nFirstLine;
	}

	SekSetIRQLine(4, SEK_IRQSTATUS_AUTO);
	SekRun(nCpsCycles * 0x01 / 0x0106);
	if (nRasterline[nInterrupt] < 224) {
		CopyCpsReg(nInterrupt);
		CopyCpsFrg(nInterrupt);
	} else {
		nRasterline[nInterrupt] = 0;
	}

	// Schedule next interrupt
	if (!bEnableAutoIrq50) {
		if (nIrqLine >= nIrqLine50) {
			nIrqLine50 = 0x0106;
		}
	} else {
		if (bEnableAutoIrq50 && nIrqLine == nIrqLine50) {
			nIrqLine50 += 32;
		}
	}
	if (!bEnableAutoIrq52 && nIrqLine >= nIrqLine52) {
		nIrqLine52 = 0x0106;
	} else {
		if (bEnableAutoIrq52 && nIrqLine == nIrqLine52) {
			nIrqLine52 += 32;
		}
	}
	ScheduleIRQ();
	if (nIrqCycles < SekTotalCycles()) {
		nIrqCycles = SekTotalCycles() + 1;
	}

	return;
}

INT32 Cps1Frame()
{
	INT32 nDisplayEnd, nNext, i;

	if (CpsReset) {
		DrvReset();
	}

	SekNewFrame();
	if (Cps1Qs == 1) {
		QsndNewFrame();
	} else {
		if (!Cps1Pic) {
			ZetOpen(0);
			PsndNewFrame();
		}
	}

	nCpsCycles = (INT32)((INT64)nCPS68KClockspeed * nBurnCPUSpeedAdjust >> 8);

	CpsRwGetInp();												// Update the input port values

	nDisplayEnd = (nCpsCycles * (nFirstLine + 224)) / 0x0106;	// Account for VBlank

	SekOpen(0);
	SekIdle(nCpsCyclesExtra);

	SekRun(nCpsCycles * nFirstLine / 0x0106);					// run 68K for the first few lines

	if (!CpsDrawSpritesInReverse) {
		CpsObjGet();											// Get objects
	}

	for (i = 0; i < 4; i++) {
		nNext = ((i + 1) * nCpsCycles) >> 2;					// find out next cycle count to run to

		if (SekTotalCycles() < nDisplayEnd && nNext > nDisplayEnd) {

			SekRun(nNext - nDisplayEnd);						// run 68K

			memcpy(CpsSaveReg[0], CpsReg, 0x100);				// Registers correct now

			GetPalette(0, 6);									// Get palette
			if (CpsStar) {
				GetStarPalette();
			}

			if (CpsDrawSpritesInReverse) {
				if (i == 3) CpsObjGet();   									// Get objects
			}

			SekSetIRQLine(2, SEK_IRQSTATUS_AUTO);				// Trigger VBlank interrupt
		}

		SekRun(nNext - SekTotalCycles());						// run 68K
		
//		if (pBurnDraw) {
//			CpsDraw();										// Draw frame
//		}
	}
	
	if (pBurnDraw) {
		CpsDraw();										// Draw frame
	}

	if (Cps1Qs == 1) {
		QsndEndFrame();
	} else {
		if (!Cps1Pic) {
			PsndSyncZ80(nCpsZ80Cycles);
			PsmUpdate(nBurnSoundLen);
			ZetClose();
		}
	}

	nCpsCyclesExtra = SekTotalCycles() - nCpsCycles;

	SekClose();

	return 0;
}

INT32 Cps2Frame()
{
	INT32 nDisplayEnd, nNext;									// variables to keep track of executed 68K cyles
	INT32 i;

	if (CpsReset) {
		DrvReset();
	}

//	extern INT32 prevline;
//	prevline = -1;

	SekNewFrame();
	QsndNewFrame();

	nCpsCycles = (INT32)(((INT64)nCPS68KClockspeed * nBurnCPUSpeedAdjust) / 0x0100);
	SekOpen(0);
	SekSetCyclesScanline(nCpsCycles / 262);

	CpsRwGetInp();											// Update the input port values

	nDisplayEnd = nCpsCycles * (nFirstLine + 224) / 0x0106;	// Account for VBlank

	nInterrupt = 0;
	for (i = 0; i < MAX_RASTER + 2; i++) {
		nRasterline[i] = 0;
	}

	// Determine which (if any) of the line counters generates the first IRQ
	bEnableAutoIrq50 = bEnableAutoIrq52 = false;
	nIrqLine50 = nIrqLine52 = 0x0106;
	if (*((UINT16*)(CpsReg + 0x50)) & 0x8000) {
		bEnableAutoIrq50 = true;
	}
	if (bEnableAutoIrq50 || (*((UINT16*)(CpsReg + 0x4E)) & 0x0200) == 0) {
		nIrqLine50 = (*((UINT16*)(CpsReg + 0x50)) & 0x01FF);
	}
	if (*((UINT16*)(CpsReg + 0x52)) & 0x8000) {
		bEnableAutoIrq52 = true;
	}
	if (bEnableAutoIrq52 || (*((UINT16*)(CpsReg + 0x4E)) & 0x0200) == 0) {
		nIrqLine52 = (*((UINT16*)(CpsReg + 0x52)) & 0x01FF);
	}
	ScheduleIRQ();

	SekIdle(nCpsCyclesExtra);

	if (nIrqCycles < nCpsCycles * nFirstLine / 0x0106) {
		SekRun(nIrqCycles);
		DoIRQ();
	}
	nNext = nCpsCycles * nFirstLine / 0x0106;
	if (SekTotalCycles() < nNext) {
		SekRun(nNext - SekTotalCycles());
	}

	CopyCpsReg(0);										// Get inititial copy of registers
	CopyCpsFrg(0);										//

	if (nIrqLine >= 0x0106 && (*((UINT16*)(CpsReg + 0x4E)) & 0x0200) == 0) {
		nIrqLine50 = *((UINT16*)(CpsReg + 0x50)) & 0x01FF;
		nIrqLine52 = *((UINT16*)(CpsReg + 0x52)) & 0x01FF;
		ScheduleIRQ();
	}

	GetPalette(0, 4);									// Get palettes
	CpsObjGet();										// Get objects

	for (i = 0; i < 3; i++) {
		nNext = ((i + 1) * nDisplayEnd) / 3;			// find out next cycle count to run to

		while (nNext > nIrqCycles && nInterrupt < MAX_RASTER) {
			SekRun(nIrqCycles - SekTotalCycles());
			DoIRQ();
		}
		SekRun(nNext - SekTotalCycles());				// run cpu
	}

//	nCpsCyclesSegment[0] = (nCpsCycles * nVBlank) / 0x0106;
//	nDone += SekRun(nCpsCyclesSegment[0] - nDone);

#if 1
	// This triggers the VBlank IRQ straight away at display end,
	// it causes glitches in the D&D games text (Press Start, Insert Coin, etc)
	SekSetIRQLine(2, SEK_IRQSTATUS_AUTO);				// VBlank
	SekRun(nCpsCycles - SekTotalCycles());
#else
	// Here we delay the VBlank IRQ until frame-end, or part-way through VBlank
	// fixes D&D glitching credit text but breaks lower Manhattan stage rasters in xmvsf
	SekRun(nCpsCycles - SekTotalCycles());
	SekSetIRQLine(2, SEK_IRQSTATUS_AUTO);				// VBlank
#endif
	
	if (pBurnDraw) {
		CpsDraw();
	}

	nCpsCyclesExtra = SekTotalCycles() - nCpsCycles;

	QsndEndFrame();

	SekClose();

//	bprintf(PRINT_NORMAL, _T("    -\n"));

#if 0 && defined FBA_DEBUG
	if (nInterrupt) {
		bprintf(PRINT_IMPORTANT, _T("Beam synchronized interrupt at line %2X.\r"), nRasterline[nInterrupt]);
	} else {
		bprintf(PRINT_NORMAL, _T("Beam synchronized interrupt disabled.   \r"));
	}

	extern INT32 counter;
	if (counter) {
		bprintf(PRINT_NORMAL, _T("\n\nSlices start at: "));
		for (i = 0; i < MAX_RASTER + 2; i++) {
			bprintf(PRINT_NORMAL, _T("%2X "), nRasterline[i]);
		}
		bprintf(PRINT_NORMAL, _T("\n"));
		for (i = 0; i < 0x80; i++) {
			if (*((UINT16*)(CpsSaveReg[0] + i * 2)) != *((UINT16*)(CpsSaveReg[nInterrupt] + i * 2))) {
				bprintf(PRINT_NORMAL, _T("Register %2X: %4X -> %4X\n"), i * 2, *((UINT16*)(CpsSaveReg[0] + i * 2)), *((UINT16*)(CpsSaveReg[nInterrupt] + i * 2)));
			}
		}
		bprintf(PRINT_NORMAL, _T("\n"));
		for (i = 0; i < 0x010; i++) {
			if (CpsSaveFrg[0][i] != CpsSaveFrg[nInterrupt][i]) {
				bprintf(PRINT_NORMAL, _T("FRG %X: %02X -> %02X\n"), i, CpsSaveFrg[0][i], CpsSaveFrg[nInterrupt][i]);
			}
		}
		bprintf(PRINT_NORMAL, _T("\n"));
		if (((CpsSaveFrg[0][4] << 8) | CpsSaveFrg[0][5]) != ((CpsSaveFrg[nInterrupt][4] << 8) | CpsSaveFrg[nInterrupt][5])) {
			bprintf(PRINT_NORMAL, _T("Layer-sprite priority: %04X -> %04X\n"), ((CpsSaveFrg[0][4] << 8) | CpsSaveFrg[0][5]), ((CpsSaveFrg[nInterrupt][4] << 8) | CpsSaveFrg[nInterrupt][5]));
		}

		bprintf(PRINT_NORMAL, _T("\n"));
		for (INT32 j = 0; j <= nInterrupt; j++) {
			if (j) {
				bprintf(PRINT_NORMAL, _T("IRQ : %i (triggered at line %3i)\n\n"), j, nRasterline[j]);
			} else {
				bprintf(PRINT_NORMAL, _T("Initial register status\n\n"));
			}

			for (i = 0; i < 0x080; i+= 8) {
				bprintf(PRINT_NORMAL, _T("%2X: %4X %4X %4X %4X %4X %4X %4X %4X\n"), i * 2, *((UINT16*)(CpsSaveReg[j] + 0 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 2 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 4 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 6 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 8 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 10 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 12 + i * 2)), *((UINT16*)(CpsSaveReg[j] + 14 + i * 2)));
			}

			bprintf(PRINT_NORMAL, _T("\nFRG: "));
			for (i = 0; i < 0x010; i++) {
				bprintf(PRINT_NORMAL, _T("%02X "), CpsSaveFrg[j][i]);
			}
			bprintf(PRINT_NORMAL, _T("\n\n"));

		}

		extern INT32 bRunPause;
		bRunPause = 1;
		counter = 0;
	}
#endif

	return 0;
}

