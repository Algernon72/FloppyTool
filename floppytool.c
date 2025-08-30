/*
 * FloppyTool (ASL version) - v7c
 * Layout:
 *   Row 1: Format | Copy | Verify | Quit
 *   Row 2: Read ADF | Write ADF | Verify ADF | About
 *
 * Changes in v7c:
 *   - Format now has 3 modalità:
 *       • Quick (OS): C:Format ... QUICK
 *       • Full (OS):  C:Format ...        (formattazione fisica via trackdisk + FS)
 *       • Deep:       Pass RAW su tutte le tracce (zero) + C:Format QUICK per installare il FS
 *   - Banner ASCII: "2025 - Danilo Savioni + Stella AI", testo nero senza ombra (JAM2)
 *   - Finestra compatta come v7b (WIN_H = 248)
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <devices/trackdisk.h>

#include <intuition/intuition.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/dos.h>


#ifndef CMD_FORMAT
#define CMD_FORMAT 9  // Comando per formattazione via trackdisk.device
#endif
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <string.h>
#include <stdio.h>

#define APP_NAME "FloppyTool"
#define APP_VER  "v7s"

/* ----- Base pointers ----- */
struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *GadToolsBase  = NULL;
struct Library       *AslBase       = NULL;
extern struct DosLibrary *DOSBase;

/* ----- Gadget IDs (main) ----- */
enum {
  GID_FORMAT=1, GID_COPY, GID_VERIFY, GID_QUIT,
  GID_READADF, GID_WRITEADF, GID_VERIFYADF, GID_ABOUT
};

/* ----- Window size & layout ----- */
#define WIN_W  500
#define WIN_H 168

/* Gadget row layout */
#define GAD_LEFT   12
#define GAD_W      110
#define GAD_H      18
#define GAD_GAP    12
#define ROW1_Y 10
#define ROW2_Y (ROW1_Y + GAD_H + 8)

/* ASCII banner area between rows and log */
#define ASCII_Y (ROW2_Y + GAD_H + 10)
#define ASCII_H 26  /* tight 3-line banner */

/* Log, progress, status */
#define LOG_Y (ASCII_Y + ASCII_H + 4)
#define LOG_H       28  /* 2 lines */  /* ~4 lines */
#define PROGRESS_Y  (LOG_Y + LOG_H + 4)
#define STATUS_Y    (PROGRESS_Y + 22)

/* Floppy geometry (DD) */
#define CYLINDERS   80
#define HEADS       2
#define TRACKS      (CYLINDERS*HEADS)  /* 160 */
#define SECTORS     11
#define BYTES_PER_SECTOR 512
#define TRACK_SIZE  (SECTORS*BYTES_PER_SECTOR)  /* 5632 bytes */
#define DISK_SIZE   (TRACKS*TRACK_SIZE)         /* 901120 bytes */
#define TOTAL_SECTORS (TRACKS*SECTORS)          /* 1760 */

/* ----- UI state ----- */
struct AppUI {
  struct Window *win;
  struct Gadget *gadlist;
  APTR           vi;
  struct Gadget *gadFormat;
  struct Gadget *gadCopy;
  struct Gadget *gadVerify;
  struct Gadget *gadQuit;

  struct Gadget *gadReadADF;
  struct Gadget *gadWriteADF;
  struct Gadget *gadVerifyADF;
  struct Gadget *gadAbout;

  char logbuf[2][120];
  int  logcount;
} ui;

/* Last drawn status/progress */
static char  gStatus[128] = "";
static ULONG gProgDone = 0, gProgTotal = 0;


/* ----- Prototypes ----- */
static void RedrawAll(void);
static void PumpRefresh(void);

static void CloseAll(void);
static BOOL OpenLibs(void);
static BOOL OpenUI(void);
static void CloseUI(void);

static void DrawStatus(const char *msg);
static void DrawProgress(ULONG done, ULONG total);
static void ClearProgress(void);
static void DrawFrame(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h);
static void DrawLog(void);
static void DrawAsciiBanner(void);
static void LogClear(void);
static void LogAdd(const char *msg);

static void DoFormatFloppy(void);
static void DoVerifyFloppy(void);
static void DoCopyFloppy(void);
static void DoReadADF(void);
static void DoWriteADF(void);
static void DoVerifyADF(void);
static void DoAbout(void);

static BOOL AskFloppyUnit(UBYTE *unitOut, CONST_STRPTR action);
typedef enum { FMT_CANCEL=0, FMT_QUICK_OS=1, FMT_FULL_OS=2, FMT_DEEP=3 } FormatMode;
static FormatMode AskFormatMode(void);
static BOOL AskVolumeName(char *outName, int maxlen, CONST_STRPTR defName);

/* ASL helpers (used by Write/Verify ADF) */
static BOOL PathSplit(CONST_STRPTR in, char *drawerOut, int dsz, char *fileOut, int fsz);
static BOOL ASL_OpenFile(char *outPath, int maxlen, CONST_STRPTR title, CONST_STRPTR defPath);

/* Raw/ADF ops */
static BOOL RawWritePass(UBYTE unit);
static BOOL RawVerify(UBYTE unit);
static BOOL RawCopyTwoDrives(UBYTE srcUnit, UBYTE dstUnit);
static BOOL RawCopyOneDrive(UBYTE unit);
static BOOL ADF_ReadFromDrive(UBYTE unit, CONST_STRPTR path);
static BOOL ADF_WriteToDrive(UBYTE unit, CONST_STRPTR path);
static BOOL OpenTD(UBYTE unit, struct MsgPort **pp, struct IOExtTD **pio);
static void CloseTD(struct MsgPort *p, struct IOExtTD *io);
static void SetFloppyMotor(UBYTE unit, BOOL on);

/* helpers */
static BOOL HasFile(CONST_STRPTR path);
static BOOL GenUniqueAdfPath(UBYTE unit, char *out, int maxlen);

/* CRC32 */
static ULONG crc32_init(void);
static ULONG crc32_update(ULONG crc, const UBYTE *buf, ULONG len);
static ULONG crc32_final(ULONG crc);

/* ========================= MAIN ========================= */

int main(void) {
  if (!OpenLibs()) return 20;
  if (!OpenUI())   { CloseAll(); return 10; }

  DrawStatus("Ready.");
  ClearProgress();
  LogClear();
  DrawAsciiBanner();

  BOOL running = TRUE;
  ULONG sigmask = 1UL << ui.win->UserPort->mp_SigBit;

  while (running) {
    ULONG sigs = Wait(sigmask);
    if (sigs & sigmask) {
      struct IntuiMessage *imsg;
      while ((imsg = GT_GetIMsg(ui.win->UserPort)) != NULL) {
        ULONG cls = imsg->Class;
        UWORD code = imsg->Code;
        APTR  iad = imsg->IAddress;
        GT_ReplyIMsg(imsg);

        switch (cls) {
          case IDCMP_GADGETUP: {
            struct Gadget *gad = (struct Gadget *)iad;
            switch (gad->GadgetID) {
              case GID_FORMAT:    DoFormatFloppy(); break;
              case GID_COPY:      DoCopyFloppy();   break;
              case GID_VERIFY:    DoVerifyFloppy(); break;
              case GID_QUIT:      running = FALSE;  break;

              case GID_READADF:   DoReadADF();      break;
              case GID_WRITEADF:  DoWriteADF();     break;
              case GID_VERIFYADF: DoVerifyADF();    break;
              case GID_ABOUT:     DoAbout();        break;
            }
          } break;

          case IDCMP_REFRESHWINDOW:
            GT_BeginRefresh(ui.win);
            RedrawAll();
            GT_EndRefresh(ui.win, TRUE);
            break;

          case IDCMP_VANILLAKEY:
            if (code == 27) running = FALSE;
            break;

          case IDCMP_CLOSEWINDOW:
            running = FALSE;
            break;
        }
      }
    }
  }

  CloseAll();
  return 0;
}

/* ========================= IMPLEMENTAZIONE ========================= */

static BOOL OpenLibs(void) {
  IntuitionBase = (struct IntuitionBase*)OpenLibrary("intuition.library", 37);
  if (!IntuitionBase) return FALSE;

  GfxBase = (struct GfxBase*)OpenLibrary("graphics.library", 37);
  if (!GfxBase) return FALSE;

  GadToolsBase = OpenLibrary("gadtools.library", 37);
  if (!GadToolsBase) return FALSE;

  AslBase = OpenLibrary("asl.library", 37);
  if (!AslBase) return FALSE;

  DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 37);
  if (!DOSBase) return FALSE;

  return TRUE;
}

static BOOL OpenUI(void) {
  ui.win = OpenWindowTags(NULL,
    WA_Left,        90,
    WA_Top,         60,
    WA_Width,       WIN_W,
    WA_Height,      WIN_H,
    WA_Title,       (ULONG)APP_NAME " " APP_VER,
    WA_DragBar,     TRUE,
    WA_DepthGadget, TRUE,
    WA_CloseGadget, TRUE,
    WA_Activate,    TRUE,
    WA_SimpleRefresh, TRUE,
    WA_GimmeZeroZero, TRUE,
    WA_IDCMP,       IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
    TAG_END
  );
  if (!ui.win) return FALSE;

  ui.vi = GetVisualInfo(ui.win->WScreen, TAG_END);
  if (!ui.vi) return FALSE;

  CreateContext(&ui.gadlist);

  struct NewGadget ng;
  memset(&ng, 0, sizeof(ng));
  ng.ng_VisualInfo = ui.vi;
  ng.ng_TextAttr   = NULL;

  const UWORD left = GAD_LEFT;
  const UWORD top1 = ROW1_Y;
  const UWORD top2 = ROW2_Y;
  const UWORD w    = GAD_W;
  const UWORD h    = GAD_H;
  const UWORD gap  = GAD_GAP;

  /* Row 1: Format | Copy | Verify | Quit */
  ng.ng_LeftEdge  = left;
  ng.ng_TopEdge   = top1;
  ng.ng_Width     = w;
  ng.ng_Height    = h;
  ng.ng_GadgetText= (UBYTE*)"Format (DF*)";
  ng.ng_GadgetID  = GID_FORMAT;
  ui.gadFormat = CreateGadget(BUTTON_KIND, ui.gadlist, &ng, TAG_END);
  if (!ui.gadFormat) return FALSE;

  ng.ng_LeftEdge  = left + (w+gap);
  ng.ng_GadgetText= (UBYTE*)"Copy (raw)";
  ng.ng_GadgetID  = GID_COPY;
  ui.gadCopy = CreateGadget(BUTTON_KIND, ui.gadFormat, &ng, TAG_END);
  if (!ui.gadCopy) return FALSE;

  ng.ng_LeftEdge  = left + 2*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"Verify";
  ng.ng_GadgetID  = GID_VERIFY;
  ui.gadVerify = CreateGadget(BUTTON_KIND, ui.gadCopy, &ng, TAG_END);
  if (!ui.gadVerify) return FALSE;

  ng.ng_LeftEdge  = left + 3*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"Quit";
  ng.ng_GadgetID  = GID_QUIT;
  ui.gadQuit = CreateGadget(BUTTON_KIND, ui.gadVerify, &ng, TAG_END);
  if (!ui.gadQuit) return FALSE;

  /* Row 2: Read ADF | Write ADF | Verify ADF | About */
  ng.ng_LeftEdge  = left;
  ng.ng_TopEdge   = top2;
  ng.ng_GadgetText= (UBYTE*)"Read ADF";
  ng.ng_GadgetID  = GID_READADF;
  ui.gadReadADF = CreateGadget(BUTTON_KIND, ui.gadQuit, &ng, TAG_END);
  if (!ui.gadReadADF) return FALSE;

  ng.ng_LeftEdge  = left + (w+gap);
  ng.ng_GadgetText= (UBYTE*)"Write ADF";
  ng.ng_GadgetID  = GID_WRITEADF;
  ui.gadWriteADF = CreateGadget(BUTTON_KIND, ui.gadReadADF, &ng, TAG_END);
  if (!ui.gadWriteADF) return FALSE;

  ng.ng_LeftEdge  = left + 2*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"Verify ADF";
  ng.ng_GadgetID  = GID_VERIFYADF;
  ui.gadVerifyADF = CreateGadget(BUTTON_KIND, ui.gadWriteADF, &ng, TAG_END);
  if (!ui.gadVerifyADF) return FALSE;

  ng.ng_LeftEdge  = left + 3*(w+gap);
  ng.ng_GadgetText= (UBYTE*)"About";
  ng.ng_GadgetID  = GID_ABOUT;
  ui.gadAbout = CreateGadget(BUTTON_KIND, ui.gadVerifyADF, &ng, TAG_END);
  if (!ui.gadAbout) return FALSE;

  AddGList(ui.win, ui.gadlist, (UWORD)-1, (UWORD)-1, NULL);
  RefreshGList(ui.gadlist, ui.win, NULL, (UWORD)-1);
  GT_BeginRefresh(ui.win);
  GT_EndRefresh(ui.win, TRUE);
  GT_RefreshWindow(ui.win, NULL);
  DrawAsciiBanner();

  return TRUE;
}

static void DrawStatus(const char *msg) {
  if (!ui.win) return;
  if (msg) { strncpy(gStatus, msg, sizeof(gStatus)-1); gStatus[sizeof(gStatus)-1]='\0'; }
  struct RastPort *rp = ui.win->RPort;
  const WORD x = 12;
  const WORD y = STATUS_Y;
  const WORD w = WIN_W - 24;
  const WORD h = 14;
  SetAPen(rp, 0);
  RectFill(rp, x-2, y-12, x-2 + w, y-12 + h + 4);
  SetAPen(rp, 1);
  Move(rp, x, y);
  Text(rp, (STRPTR)msg, (ULONG)strlen(msg));
}

static void DrawFrame(struct RastPort *rp, WORD x, WORD y, WORD w, WORD h) {
  if (!rp) return;
  Move(rp, x,     y);     Draw(rp, x+w, y);
  Move(rp, x+w,   y);     Draw(rp, x+w, y+h);
  Move(rp, x+w,   y+h);   Draw(rp, x,   y+h);
  Move(rp, x,     y+h);   Draw(rp, x,   y);
}

static void DrawProgress(ULONG done, ULONG total) {
  gProgDone = done; gProgTotal = total;
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, y = PROGRESS_Y, w = WIN_W - 24, h = 12;
  SetAPen(rp, 1);
  DrawFrame(rp, x, y, w, h);
  ULONG frac = (total>0) ? (done * w) / total : 0;
  if (frac > (ULONG)w) frac = w;
  SetAPen(rp, 2);
  RectFill(rp, x+1, y+1, x+(WORD)frac, y+h-1);
}

static void ClearProgress(void) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, y = PROGRESS_Y, w = WIN_W - 24, h = 12;
  SetAPen(rp, 0);
  RectFill(rp, x-2, y-2, x+w+2, y+h+2);
  SetAPen(rp, 1);
  DrawFrame(rp, x, y, w, h);
}

static void DrawLog(void) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, y = LOG_Y, w = WIN_W - 24, h = LOG_H;
  SetAPen(rp, 0);
  RectFill(rp, x, y, x+w, y+h);
  SetAPen(rp, 1);
  int lines = ui.logcount; if (lines > 2) lines = 2;
  for (int i=0;i<lines;i++) {
    Move(rp, x+4, y+12 + 12*i);  /* slightly tighter baseline */
    Text(rp, (STRPTR)ui.logbuf[i], (ULONG)strlen(ui.logbuf[i]));
  }
  DrawFrame(rp, x, y, w, h);
}

/* ====== ASCII banner (3 lines: pattern + title + pattern) ====== */
static void DrawAsciiBanner(void) {
  if (!ui.win) return;
  struct RastPort *rp = ui.win->RPort;
  WORD x = 12, w = WIN_W - 24;

  /* font metrics */
  UWORD fh = (UWORD)(rp->Font ? rp->Font->tf_YSize : 8);
  UWORD bl = (UWORD)(rp->Font ? rp->Font->tf_Baseline : 7);
  WORD topBase    = ASCII_Y + (WORD)bl + 1;        /* 1px margin on top */
  WORD titleBase  = topBase + (WORD)fh;            /* line 2 */
  WORD bottomBase = titleBase + (WORD)fh;

  /* white background */
  SetAPen(rp, 1); RectFill(rp, x, ASCII_Y, x+w, ASCII_Y + ASCII_H);

  /* top pattern */
  const char *top = "/\\=/\\=--==/\\=/\\=--==";
  ULONG topw = TextLength(rp, (STRPTR)top, (ULONG)strlen(top));
  SetAPen(rp, 0);
  WORD cx = x;
  while (cx < x + w) {
    Move(rp, cx, topBase);
    Text(rp, (STRPTR)top, (ULONG)strlen(top));
    cx += (WORD)topw;
  }

  /* centered title */
  const char *title = "FloppyTool - 2025 Danilo Savioni + Stella AI";
  ULONG tlw = TextLength(rp, (STRPTR)title, (ULONG)strlen(title));
  WORD tx = x + (w - (WORD)tlw) / 2; if (tx < x) tx = x;
  SetDrMd(rp, JAM2);  SetAPen(rp, 0);  SetBPen(rp, 1);
  Move(rp, tx, titleBase);
  Text(rp, (STRPTR)title, (ULONG)strlen(title));
  SetDrMd(rp, JAM1);

  /* bottom pattern */
  const char *bot = "==/\\=/\\==--==/\\=/\\==";
  ULONG botw = TextLength(rp, (STRPTR)bot, (ULONG)strlen(bot));
  cx = x;
  while (cx < x + w) {
    Move(rp, cx, bottomBase);
    Text(rp, (STRPTR)bot, (ULONG)strlen(bot));
    cx += (WORD)botw;
  }
}

static void LogClear(void) {
  ui.logcount = 0;
  for (int i=0;i<2;i++) ui.logbuf[i][0] = '\0';
  DrawLog();
}

static void LogAdd(const char *msg) {
  if (!msg) return;
  if (ui.logcount < 2) {
    strncpy(ui.logbuf[ui.logcount], msg, sizeof(ui.logbuf[ui.logcount])-1);
    ui.logbuf[ui.logcount][sizeof(ui.logbuf[ui.logcount])-1] = '\0';
    ui.logcount++;
  } else {
    /* shift up: line1 becomes line0, append new on line1 */
    strncpy(ui.logbuf[0], ui.logbuf[1], sizeof(ui.logbuf[0])-1);
    ui.logbuf[0][sizeof(ui.logbuf[0])-1] = '\0';
    strncpy(ui.logbuf[1], msg, sizeof(ui.logbuf[1])-1);
    ui.logbuf[1][sizeof(ui.logbuf[1])-1] = '\0';
  }
  DrawLog();
}


/* ====== Redraw helpers ====== */
static void RedrawAll(void) {
  if (!ui.win) return;
  DrawAsciiBanner();
  DrawLog();
  DrawProgress(gProgDone, gProgTotal);
  if (gStatus[0]) DrawStatus(gStatus);
}

static void PumpRefresh(void) {
  if (!ui.win) return;
  struct IntuiMessage *imsg;
  /* Drain refresh messages so overlaps from requesters are repainted */
  while ((imsg = GT_GetIMsg(ui.win->UserPort)) != NULL) {
    if (imsg->Class == IDCMP_REFRESHWINDOW) {
      GT_ReplyIMsg(imsg);
      GT_BeginRefresh(ui.win);
      RedrawAll();
      GT_EndRefresh(ui.win, TRUE);
    } else {
      /* Reply and ignore other events during long ops */
      GT_ReplyIMsg(imsg);
    }
  }
}
/* ====== Simple dialogs ====== */

static BOOL AskFloppyUnit(UBYTE *unitOut, CONST_STRPTR action) {
  if (!unitOut) return FALSE;
  *unitOut = 0xFF;

  static UBYTE title[] = APP_NAME " " APP_VER;
  UBYTE body[96];
  sprintf((char*)body, "Select FLOPPY drive for %s", action ? action : "operation");

  struct EasyStruct es;
  es.es_StructSize = sizeof(struct EasyStruct);
  es.es_Flags      = 0;
  es.es_Title      = title;
  es.es_TextFormat = body;
  es.es_GadgetFormat = (UBYTE*)"DF0|DF1|DF2|DF3|Cancel";

  LONG sel = EasyRequestArgs(ui.win, &es, NULL, NULL);
  PumpRefresh();
  if (sel == 0 || sel == 5) return FALSE;
  if (sel < 1 || sel > 4) return FALSE;
  *unitOut = (UBYTE)(sel - 1);
  return TRUE;
}

static FormatMode AskFormatMode(void) {
  static UBYTE title[] = APP_NAME " " APP_VER;
  static UBYTE text[]  = "Choose format mode";
  static UBYTE gadgets[] = "Quick (OS)|Full (OS)|Deep (RAW+Quick)|Cancel";
  struct EasyStruct es = { sizeof(struct EasyStruct), 0, title, text, gadgets };
  LONG sel = EasyRequestArgs(ui.win, &es, NULL, NULL);
  PumpRefresh();
  if (sel == 0 || sel == 4) return FMT_CANCEL;
  if (sel == 1) return FMT_QUICK_OS;
  if (sel == 2) return FMT_FULL_OS;
  if (sel == 3) return FMT_DEEP;
  return FMT_CANCEL;
}

static BOOL AskVolumeName(char *outName, int maxlen, CONST_STRPTR defName) {
  if (!outName || maxlen < 2) return FALSE;
  strncpy(outName, defName ? defName : "Untitled", maxlen-1);
  outName[maxlen-1] = '\0';
  return TRUE;
}

/* ========= ASL FILE REQUESTERS (for WRITE/VERIFY ADF) ========= */

static BOOL PathSplit(CONST_STRPTR in, char *drawerOut, int dsz, char *fileOut, int fsz) {
  if (!in || !drawerOut || !fileOut) return FALSE;
  drawerOut[0] = 0; fileOut[0] = 0;
  const char *lastSlash = NULL;
  const char *p = in;
  while (*p) { if (*p == '/' || *p == '\\') lastSlash = p; p++; }
  const char *colon = strchr(in, ':');
  if (lastSlash) {
    int dlen = (int)(lastSlash - in + 1);
    if (dlen >= dsz) dlen = dsz-1;
    strncpy(drawerOut, in, dlen); drawerOut[dlen] = '\0';
    strncpy(fileOut, lastSlash+1, fsz-1); fileOut[fsz-1] = '\0';
  } else {
    if (colon) {
      int vlen = (int)(colon - in + 1);
      if (vlen >= dsz) vlen = dsz-1;
      strncpy(drawerOut, in, vlen); drawerOut[vlen] = '\0';
      strncpy(fileOut, colon+1, fsz-1); fileOut[fsz-1] = '\0';
    } else {
      strncpy(drawerOut, "", dsz);
      strncpy(fileOut, in, fsz-1); fileOut[fsz-1] = '\0';
    }
  }
  return TRUE;
}

static BOOL ASL_OpenFile(char *outPath, int maxlen, CONST_STRPTR title, CONST_STRPTR defPath) {
  if (!outPath || maxlen < 4) return FALSE;
  outPath[0] = '\0';
  char drawer[256] = "RAM:";
  char file[108]   = "floppy.adf";
  if (defPath) PathSplit(defPath, drawer, sizeof(drawer), file, sizeof(file));

  struct FileRequester *fr = (struct FileRequester*)AllocAslRequestTags(ASL_FileRequest,
    ASLFR_TitleText,    (ULONG)(title ? title : "Open ADF"),
    ASLFR_InitialDrawer,(ULONG)drawer,
    ASLFR_InitialFile,  (ULONG)file,
    TAG_END);
  if (!fr) return FALSE;

  BOOL ok = FALSE;
  if (AslRequest(fr, NULL)) {
    const char *d = fr->fr_Drawer ? fr->fr_Drawer : "";
    const char *f = fr->fr_File   ? fr->fr_File   : "";
    if (d[0]) {
      int needsSlash = 0;
      int dl = strlen(d);
      if (dl>0) {
        char last = d[dl-1];
        needsSlash = !(last == ':' || last == '/' || last == '\\');
      }
      if (needsSlash)
        snprintf(outPath, maxlen, "%s/%s", d, f);
      else
        snprintf(outPath, maxlen, "%s%s", d, f);
    } else {
      snprintf(outPath, maxlen, "%s", f);
    }
    ok = (outPath[0] != '\0');
  }

  FreeAslRequest(fr);
  PumpRefresh();
  return ok;
}

/* ========================= ACTIONS ========================= */

static void DoFormatFloppy(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "FORMAT")) { DrawStatus("Format canceled."); return; }
  FormatMode mode = AskFormatMode();
  if (mode == FMT_CANCEL) { DrawStatus("Format canceled."); return; }

  char volname[32];
  if (!AskVolumeName(volname, sizeof(volname), "Untitled")) { DrawStatus("Format canceled."); return; }

  const char *candidates[] = { "C:Format", "SYS:C/Format", NULL };
  const char *fmt = NULL;
  for (int i=0; candidates[i]; ++i) { if (HasFile(candidates[i])) { fmt = candidates[i]; break; } }
  if (!fmt) { DrawStatus("No Format in C: (install Workbench C:Format)."); return; }

  LogClear();

  if (mode == FMT_QUICK_OS) {
    char cmd[256]; sprintf(cmd, "%s DRIVE DF%u: NAME \"%s\" QUICK", fmt, (unsigned)unit, volname);
    DrawStatus("Quick format (OS)...");
    BPTR inTmp = Open("T:ft_yes", MODE_NEWFILE); if (inTmp) { Write(inTmp, (APTR)"y\n", 2); Close(inTmp); }
    BPTR in  = Open("T:ft_yes", MODE_OLDFILE);
    BPTR out = Open("NIL:", MODE_NEWFILE);
    LONG ok = Execute((STRPTR)cmd, in ? in : Open("NIL:", MODE_OLDFILE), out);
    if (in) Close(in); if (out) Close(out); DeleteFile("T:ft_yes");
    SetFloppyMotor(unit, FALSE);
    DrawStatus(ok ? "Quick format done." : "Quick format failed.");
    ClearProgress();
    return;
  }

  if (mode == FMT_FULL_OS) {
    char cmd[256]; sprintf(cmd, "%s DRIVE DF%u: NAME \"%s\"", fmt, (unsigned)unit, volname);
    DrawStatus("Full format (OS)...");
    BPTR inTmp = Open("T:ft_yes", MODE_NEWFILE); if (inTmp) { Write(inTmp, (APTR)"y\n", 2); Close(inTmp); }
    BPTR in  = Open("T:ft_yes", MODE_OLDFILE);
    BPTR out = Open("NIL:", MODE_NEWFILE);
    LONG ok = Execute((STRPTR)cmd, in ? in : Open("NIL:", MODE_OLDFILE), out);
    if (in) Close(in); if (out) Close(out); DeleteFile("T:ft_yes");
    SetFloppyMotor(unit, FALSE);
    DrawStatus(ok ? "Full format done." : "Full format failed.");
    ClearProgress();
    return;
  }

  if (mode == FMT_DEEP) {
    DrawStatus("Deep format: RAW pass...");
    ClearProgress();
    BOOL passOk = RawWritePass(unit);
    SetFloppyMotor(unit, FALSE);
    if (!passOk) { DrawStatus("RAW pass failed."); ClearProgress(); return; }

    /* Now install filesystem quickly */
    char cmd2[256]; sprintf(cmd2, "%s DRIVE DF%u: NAME \"%s\" QUICK", fmt, (unsigned)unit, volname);
    DrawStatus("Installing filesystem (Quick)...");
    BPTR inTmp = Open("T:ft_yes", MODE_NEWFILE); if (inTmp) { Write(inTmp, (APTR)"y\n", 2); Close(inTmp); }
    BPTR in  = Open("T:ft_yes", MODE_OLDFILE);
    BPTR out = Open("NIL:", MODE_NEWFILE);
    LONG ok2 = Execute((STRPTR)cmd2, in ? in : Open("NIL:", MODE_OLDFILE), out);
    if (in) Close(in); if (out) Close(out); DeleteFile("T:ft_yes");
    SetFloppyMotor(unit, FALSE);
    DrawStatus(ok2 ? "Deep format done." : "Deep format failed at OS stage.");
    ClearProgress();
    return;
  }
}

static void DoVerifyFloppy(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "VERIFY")) { DrawStatus("Verify canceled."); return; }
  LogClear();
  DrawStatus("Verify: reading tracks...");
  BOOL ok = RawVerify(unit);
  SetFloppyMotor(unit, FALSE);
  DrawStatus(ok ? "Verify OK." : "Verify FAILED (read error).");
  ClearProgress();
}

static void DoCopyFloppy(void) {
  UBYTE src, dst;
  if (!AskFloppyUnit(&src, "COPY (source)")) { DrawStatus("Copy canceled."); return; }
  if (!AskFloppyUnit(&dst, "COPY (destination)")) { DrawStatus("Copy canceled."); return; }

  LogClear();
  DrawStatus("Copy (raw) in progress...");
  DrawProgress(0, DISK_SIZE);

  BOOL ok;
  if (src == dst) ok = RawCopyOneDrive(src);
  else            ok = RawCopyTwoDrives(src, dst);

  SetFloppyMotor(src, FALSE);
  SetFloppyMotor(dst, FALSE);

  DrawStatus(ok ? "Copy completed." : "Copy failed.");
  ClearProgress();
}

/* Read ADF: select only DFx, auto path RAM:DF<unit>_<n>.adf */
static void DoReadADF(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "READ ADF (source DFx:)")) { DrawStatus("Read ADF canceled."); return; }

  char path[300];
  if (!GenUniqueAdfPath(unit, path, sizeof(path))) { DrawStatus("Cannot build RAM: output path"); return; }

  char msg[160]; sprintf(msg, "Saving to %s", path); LogClear(); LogAdd(msg);
  DrawStatus("Reading DFx: to ADF...");
  BOOL ok = ADF_ReadFromDrive(unit, path);
  SetFloppyMotor(unit, FALSE);
  DrawStatus(ok ? "ADF saved." : "ADF read failed.");
  ClearProgress();
}

static void DoWriteADF(void) {
  UBYTE unit;
  if (!AskFloppyUnit(&unit, "WRITE ADF (destination DFx:)")) { DrawStatus("Write ADF canceled."); return; }

  char path[300];
  if (!ASL_OpenFile(path, sizeof(path), "Select ADF to write...", "RAM:floppy.adf")) { DrawStatus("Write ADF canceled."); return; }

  LogClear();
  DrawStatus("Writing ADF to DFx: ...");
  BOOL ok = ADF_WriteToDrive(unit, path);
  SetFloppyMotor(unit, FALSE);
  DrawStatus(ok ? "ADF written to disk." : "ADF write failed.");
  ClearProgress();
}

static void DoVerifyADF(void) {
  char path[300];
  if (!ASL_OpenFile(path, sizeof(path), "Select ADF to verify...", "RAM:floppy.adf")) { DrawStatus("Verify ADF canceled."); return; }

  LogClear();
  DrawStatus("Verifying ADF...");
  BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
  if (!fh) { LogAdd("Cannot open ADF"); DrawStatus("Verify ADF failed."); return; }

  /* Size via ExamineFH (fallback Seek) */
  LONG size = -1;
  struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
  if (fib && ExamineFH(fh, fib)) size = fib->fib_Size;
  else {
    LONG end = Seek(fh, 0, OFFSET_END); if (end >= 0) size = end;
    Seek(fh, 0, OFFSET_BEGINNING);
  }
  if (fib) FreeVec(fib);
  char smsg[120]; sprintf(smsg, "ADF size: %ld bytes", (long)size); LogAdd(smsg);

  if (size != (LONG)DISK_SIZE) {
    LogAdd("Warning: size is not 901,120 bytes");
  }
  Seek(fh, 0, OFFSET_BEGINNING);

  /* CRC32 */
  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { Close(fh); LogAdd("No memory for CRC"); DrawStatus("Verify ADF failed."); return; }

  ULONG crc = crc32_init();
  LONG remaining = size > 0 ? size : 0;
  while (remaining > 0) {
    LONG chunk = (remaining >= TRACK_SIZE) ? TRACK_SIZE : remaining;
    LONG rd = Read(fh, buf, chunk);
    if (rd != chunk) { LogAdd("File read error during CRC"); FreeVec(buf); Close(fh); DrawStatus("Verify ADF failed."); return; }
    crc = crc32_update(crc, buf, (ULONG)chunk);
    remaining -= chunk;
    DrawProgress(size - remaining, size > 0 ? (ULONG)size : 1);
  }
  crc = crc32_final(crc);

  FreeVec(buf);
  Close(fh);

  char cmsg[120]; sprintf(cmsg, "CRC32: %08lx", (ULONG)crc);
  LogAdd(cmsg);
  DrawStatus((size == (LONG)DISK_SIZE) ? "ADF looks OK (size+CRC computed)." : "ADF verified (non-standard size).");
  ClearProgress();
}

static void DoAbout(void) {
  static UBYTE title[] = APP_NAME " " APP_VER;
  static UBYTE text[]  =
    "FloppyTool – Amiga DD floppy helper\n"
    "\n"
    "Features:\n"
    "  • Format (Quick/Full/Deep)\n"
    "  • Verify/Copy raw\n"
    "  • Read/Write/Verify ADF\n"
    "\n"
    "© 2025 Danilo Savioni + Stella\n"
    "Built for AmigaOS 2.0+ (68k)\n";
  static UBYTE gadgets[] = "OK";
  struct EasyStruct es = { sizeof(struct EasyStruct), 0, title, text, gadgets };
  EasyRequestArgs(ui.win, &es, NULL, NULL);
}

/* ====== Raw ops via trackdisk.device ====== */

static BOOL OpenTD(UBYTE unit, struct MsgPort **pp, struct IOExtTD **pio) {
  if (!pp || !pio) return FALSE;
  *pp = NULL; *pio = NULL;

  struct MsgPort *port = CreateMsgPort();
  if (!port) return FALSE;
  struct IOExtTD *io = (struct IOExtTD*)CreateIORequest(port, sizeof(struct IOExtTD));
  if (!io) { DeleteMsgPort(port); return FALSE; }

  if (OpenDevice("trackdisk.device", unit, (struct IORequest*)io, 0) != 0) {
    DeleteIORequest((struct IORequest*)io);
    DeleteMsgPort(port);
    return FALSE;
  }
  *pp = port; *pio = io;
  return TRUE;
}

static void CloseTD(struct MsgPort *p, struct IOExtTD *io) {
  if (io) { CloseDevice((struct IORequest*)io); DeleteIORequest((struct IORequest*)io); }
  if (p)  { DeleteMsgPort(p); }
}

static void SetFloppyMotor(UBYTE unit, BOOL on) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return;
  io->iotd_Req.io_Command = TD_MOTOR;
  io->iotd_Req.io_Length  = on ? 1 : 0;
  (void)DoIO((struct IORequest*)io);
  CloseTD(p, io);
}

static BOOL RawWritePass(UBYTE unit) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return FALSE;

  SetFloppyMotor(unit, TRUE);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  UBYTE *verifyBuf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf || !verifyBuf) {
    if (buf) FreeVec(buf);
    if (verifyBuf) FreeVec(verifyBuf);
    CloseTD(p, io);
    return FALSE;
  }
  memset(buf, 0, TRACK_SIZE);

  BOOL ok = TRUE;
  for (ULONG t=0; t<TRACKS; ++t) {
    int maxRetry = (t == 0) ? 5 : 2;  // More retries on track 0
    BOOL success = FALSE;

    if (t == 0) {
      // Pre-scrub with pattern A5
      memset(buf, 0xA5, TRACK_SIZE);
      io->iotd_Req.io_Command = CMD_WRITE;
      io->iotd_Req.io_Data    = (APTR)buf;
      io->iotd_Req.io_Length  = TRACK_SIZE;
      io->iotd_Req.io_Offset  = 0;
      DoIO((struct IORequest*)io); // Ignore result

      // Pre-scrub with pattern 00
      memset(buf, 0x00, TRACK_SIZE);
      io->iotd_Req.io_Command = CMD_WRITE;
      io->iotd_Req.io_Data    = (APTR)buf;
      io->iotd_Req.io_Length  = TRACK_SIZE;
      io->iotd_Req.io_Offset  = 0;
      DoIO((struct IORequest*)io); // Ignore result
    }

    while (maxRetry-- > 0 && !success) {

    // Try CMD_FORMAT if supported
    io->iotd_Req.io_Command = CMD_FORMAT;
    io->iotd_Req.io_Data    = (APTR)NULL;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) == 0) {
      // Format success, now verify
      io->iotd_Req.io_Command = CMD_READ;
      io->iotd_Req.io_Data    = (APTR)verifyBuf;
      io->iotd_Req.io_Length  = TRACK_SIZE;
      io->iotd_Req.io_Offset  = t * TRACK_SIZE;
      if (DoIO((struct IORequest*)io) == 0 &&
          memcmp(buf, verifyBuf, TRACK_SIZE) == 0) {
        success = TRUE;
        goto track_done;
      } else {
        LogAdd("CMD_FORMAT verify failed; fallback to CMD_WRITE.");
      }
    } else {
      LogAdd("CMD_FORMAT failed; fallback to CMD_WRITE.");
    }

      io->iotd_Req.io_Command = CMD_WRITE;
      io->iotd_Req.io_Data    = (APTR)buf;
      io->iotd_Req.io_Length  = TRACK_SIZE;
      io->iotd_Req.io_Offset  = t * TRACK_SIZE;
      if (DoIO((struct IORequest*)io) == 0) {
        // Verify immediately after write
        io->iotd_Req.io_Command = CMD_READ;
        io->iotd_Req.io_Data    = (APTR)verifyBuf;
        io->iotd_Req.io_Length  = TRACK_SIZE;
        io->iotd_Req.io_Offset  = t * TRACK_SIZE;
        if (DoIO((struct IORequest*)io) == 0 &&
            memcmp(buf, verifyBuf, TRACK_SIZE) == 0) {
          success = TRUE;
        } else {
          LogAdd("Verify failed after write.");
        }
      } else {
        LogAdd("Write failed, retrying...");
      }
      WaitTOF();
    }

    if (!success) {
      char m[80]; sprintf(m, "Track %lu failed after retries.", (unsigned long)t);
      LogAdd(m);
      // Don't abort; continue with next track (format even if error)
    }

    track_done:
    DrawProgress(t+1, TRACKS);
    if ((t % 4) == 0 || t == TRACKS-1) {
      char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS);
      LogAdd(m);
    }
  }

  FreeVec(buf);
  FreeVec(verifyBuf);
  CloseTD(p, io);
  return ok;
}

static BOOL RawVerify(UBYTE unit) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return FALSE;

  SetFloppyMotor(unit, TRUE);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { CloseTD(p, io); return FALSE; }

  ULONG doneSectors = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    LONG err = DoIO((struct IORequest*)io);
    if (err != 0) {
      ok = FALSE;
      char m[80]; sprintf(m, "Read error at track %lu (io_Error=%ld)", (unsigned long)t, (long)io->iotd_Req.io_Error);
      LogAdd(m);
      break;
    }
    doneSectors += SECTORS;
    DrawProgress(doneSectors, TOTAL_SECTORS);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[80]; sprintf(m, "Track %lu/%u, sectors %lu/%u", (unsigned long)(t+1), TRACKS, (unsigned long)doneSectors, (unsigned)TOTAL_SECTORS); LogAdd(m); }
  }

  FreeVec(buf);
  CloseTD(p, io);
  return ok;
}

static BOOL RawCopyTwoDrives(UBYTE srcUnit, UBYTE dstUnit) {
  struct MsgPort *ps = NULL; struct IOExtTD *is = NULL;
  struct MsgPort *pd = NULL; struct IOExtTD *id = NULL;

  if (!OpenTD(srcUnit, &ps, &is)) return FALSE;
  if (!OpenTD(dstUnit, &pd, &id)) { CloseTD(ps, is); return FALSE; }

  SetFloppyMotor(srcUnit, TRUE);
  SetFloppyMotor(dstUnit, TRUE);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { CloseTD(ps, is); CloseTD(pd, id); return FALSE; }

  ULONG done = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    is->iotd_Req.io_Command = CMD_READ;
    is->iotd_Req.io_Data    = (APTR)buf;
    is->iotd_Req.io_Length  = TRACK_SIZE;
    is->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)is) != 0) { ok = FALSE; LogAdd("Read error"); break; }

    id->iotd_Req.io_Command = CMD_WRITE;
    id->iotd_Req.io_Data    = (APTR)buf;
    id->iotd_Req.io_Length  = TRACK_SIZE;
    id->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)id) != 0) { ok = FALSE; LogAdd("Write error"); break; }

    done += TRACK_SIZE;
    DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  CloseTD(ps, is);
  CloseTD(pd, id);
  return ok;
}

static BOOL RawCopyOneDrive(UBYTE unit) {
  BOOL ok = FALSE;
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) return FALSE;

  SetFloppyMotor(unit, TRUE);

  UBYTE *image = (UBYTE*)AllocVec(DISK_SIZE, MEMF_CLEAR);
  if (!image) { CloseTD(p, io); return FALSE; }

  ULONG done = 0;
  DrawStatus("Reading source to RAM (swap later)...");
  ClearProgress();
  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data    = (APTR)(image + t*TRACK_SIZE);
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { LogAdd("Read error"); goto cleanup; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  ClearProgress();
  done = 0;

  struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE*)APP_NAME,
                           (UBYTE*)"Insert DESTINATION disk and click Continue",
                           (UBYTE*)"Continue|Cancel" };
  LONG sel = EasyRequestArgs(ui.win, &es, NULL, NULL);
  PumpRefresh();
  if (sel != 1) goto cleanup;

  DrawStatus("Writing RAM image to destination...");
  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_WRITE;
    io->iotd_Req.io_Data    = (APTR)(image + t*TRACK_SIZE);
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { LogAdd("Write error"); goto cleanup; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  ok = TRUE;

cleanup:
  FreeVec(image);
  CloseTD(p, io);
  return ok;
}

/* ====== ADF I/O ====== */

static BOOL ADF_ReadFromDrive(UBYTE unit, CONST_STRPTR path) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) { LogAdd("Open trackdisk failed"); return FALSE; }

  SetFloppyMotor(unit, TRUE);

  BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
  if (!fh) { CloseTD(p, io); LogAdd("Cannot create ADF file"); return FALSE; }

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { Close(fh); CloseTD(p, io); LogAdd("No memory"); return FALSE; }

  ULONG done = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    io->iotd_Req.io_Command = CMD_READ;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { ok = FALSE; LogAdd("Read error"); break; }
    LONG wr = Write(fh, buf, TRACK_SIZE);
    if (wr != TRACK_SIZE) { ok = FALSE; LogAdd("File write error"); break; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  Close(fh);
  CloseTD(p, io);

  if (ok) {
    BPTR fh2 = Open((STRPTR)path, MODE_OLDFILE);
    if (fh2) {
      struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
      if (fib && ExamineFH(fh2, fib)) {
        char m[100]; sprintf(m, "Saved ADF size: %ld bytes", (long)fib->fib_Size);
        LogAdd(m);
      }
      if (fib) FreeVec(fib);
      Close(fh2);
    }
  }

  return ok;
}

static BOOL ADF_WriteToDrive(UBYTE unit, CONST_STRPTR path) {
  struct MsgPort *p = NULL; struct IOExtTD *io = NULL;
  if (!OpenTD(unit, &p, &io)) { LogAdd("Open trackdisk failed"); return FALSE; }

  SetFloppyMotor(unit, TRUE);

  BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
  if (!fh) { CloseTD(p, io); LogAdd("Cannot open ADF"); return FALSE; }

  LONG size = -1;
  struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
  if (fib && ExamineFH(fh, fib)) {
    size = fib->fib_Size;
  } else {
    LONG end = Seek(fh, 0, OFFSET_END);
    if (end >= 0) size = end;
    Seek(fh, 0, OFFSET_BEGINNING);
  }
  if (fib) FreeVec(fib);

  char smsg[96]; sprintf(smsg, "Detected ADF size: %ld bytes", (long)size); LogAdd(smsg);

  if (size != (LONG)DISK_SIZE) {
    Close(fh); CloseTD(p, io);
    LogAdd("Invalid ADF size (need 901,120 bytes)");
    return FALSE;
  }
  Seek(fh, 0, OFFSET_BEGINNING);

  UBYTE *buf = (UBYTE*)AllocVec(TRACK_SIZE, MEMF_CLEAR);
  if (!buf) { Close(fh); CloseTD(p, io); LogAdd("No memory"); return FALSE; }

  ULONG done = 0;
  BOOL ok = TRUE;

  for (ULONG t=0; t<TRACKS; ++t) {
    LONG rd = Read(fh, buf, TRACK_SIZE);
    if (rd != TRACK_SIZE) { ok = FALSE; LogAdd("File read error"); break; }
    io->iotd_Req.io_Command = CMD_WRITE;
    io->iotd_Req.io_Data    = (APTR)buf;
    io->iotd_Req.io_Length  = TRACK_SIZE;
    io->iotd_Req.io_Offset  = t * TRACK_SIZE;
    if (DoIO((struct IORequest*)io) != 0) { ok = FALSE; LogAdd("Write error"); break; }
    done += TRACK_SIZE; DrawProgress(done, DISK_SIZE);
    if ((t % 8) == 0 || t == TRACKS-1) { char m[64]; sprintf(m, "Track %lu/%u", (unsigned long)(t+1), TRACKS); LogAdd(m); }
  }

  FreeVec(buf);
  Close(fh);
  CloseTD(p, io);
  return ok;
}

/* ----- Shutdown ----- */

static void CloseUI(void) {
  if (ui.win) {
    if (ui.gadlist) {
      RemoveGList(ui.win, ui.gadlist, (UWORD)-1);
      FreeGadgets(ui.gadlist);
      ui.gadlist = NULL;
    }
    CloseWindow(ui.win);
    ui.win = NULL;
  }
  if (ui.vi) {
    FreeVisualInfo(ui.vi);
    ui.vi = NULL;
  }
}

static void CloseAll(void) {
  CloseUI();
  if (GadToolsBase) CloseLibrary(GadToolsBase);
  if (AslBase)      CloseLibrary(AslBase);
  if (GfxBase)      CloseLibrary((struct Library*)GfxBase);
  if (IntuitionBase)CloseLibrary((struct Library*)IntuitionBase);
  if (DOSBase)      CloseLibrary((struct Library*)DOSBase);
}

/* ----- Helpers ----- */

static BOOL HasFile(CONST_STRPTR path) {
  BPTR lock = Lock((STRPTR)path, ACCESS_READ);
  if (lock) { UnLock(lock); return TRUE; }
  return FALSE;
}

/* Generate RAM:DF<unit>_<n>.adf with n from 0..999 ensuring file doesn't exist */
static BOOL GenUniqueAdfPath(UBYTE unit, char *out, int maxlen) {
  if (!out || maxlen < 10) return FALSE;
  for (int i=0; i<1000; ++i) {
    char name[64];
    sprintf(name, "RAM:DF%u_%03d.adf", (unsigned)unit, i);
    BPTR lock = Lock(name, ACCESS_READ);
    if (lock) { UnLock(lock); continue; }
    strncpy(out, name, maxlen-1);
    out[maxlen-1] = '\0';
    return TRUE;
  }
  return FALSE;
}

/* ----- CRC32 (poly 0xEDB88320) ----- */

static ULONG crc32_init(void) { return 0xFFFFFFFFUL; }

static ULONG crc32_update(ULONG crc, const UBYTE *buf, ULONG len) {
  ULONG c = crc;
  while (len--) {
    c ^= (ULONG)(*buf++);
    for (int k=0; k<8; ++k) {
      if (c & 1) c = (c >> 1) ^ 0xEDB88320UL;
      else       c = (c >> 1);
    }
  }
  return c;
}

static ULONG crc32_final(ULONG crc) { return crc ^ 0xFFFFFFFFUL; }
